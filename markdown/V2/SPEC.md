# V2 — Spécification Technique (Cahier des Charges)

---

## 0. Périmètre

V2 ajoute le **parcours de la liste des processus Linux** (`task_struct`)
au co-kernel. Il extrait PID, TGID, UID et nom de chaque processus, et
écrit un snapshot binaire dans le `data_buf` de la comm page.

**Base** : V1 est complet et fonctionnel. V2 NE DOIT PAS casser les
garanties V0 ni V1.

---

## 1. Invariants (hérités de V0/V1, NE DOIVENT PAS être cassés)

| ID | Invariant | Vérification |
|----|-----------|--------------|
| INV-1 | Pas d'entrée dans `/proc/modules` | `lsmod \| wc -l` = 0 |
| INV-2 | Pas d'entrée dans `/sys/module/parasite*` | `ls /sys/module/parasite*` échoue |
| INV-3 | Pas de consommation mémoire visible | `MemTotal` stable (±4 MB) |
| INV-4 | Pas d'anomalie CPU visible | `perf stat sleep 1` normal |
| INV-5 | Pas de trace dmesg | `dmesg \| grep -ci parasite` = 0 |
| INV-6 | Handler PMI sans crash | 10 min d'opération, pas de panic |
| INV-7 | Heartbeat | `tick_count` non-nul et croissant |
| INV-8 | Direct map | `init_comm` = `"swapper/0"` |

---

## 2. Environnement matériel et VM

| Paramètre | Valeur |
|-----------|--------|
| Hypervisor | QEMU 9.x + KVM |
| Machine | q35 (ICH9) |
| CPU | `-cpu host` — 1 vCPU |
| RAM | 1 GB (`-m 1024`) |
| Noyau | Debian 6.12.73+deb13-amd64 |
| Console | Série (`-nographic`, `console=ttyS0`) |
| Host CPU | AMD Ryzen 7 5825U (ou compatible) |
| APIC | x2APIC |
| PMU | AMD PerfEvtSel0/PerfCtr0 |

---

## 3. Exigences fonctionnelles

### 3.1 Parcours de la liste des processus

| ID | Exigence | Détails |
|----|----------|---------|
| **TASK-1** | Parcourir `task_struct.tasks` | Partir de `init_task_dm`, suivre `.tasks.next` jusqu'à retour à `init_task` |
| **TASK-2** | Extraire PID | `*(int32_t *)(task + offset_pid)` |
| **TASK-3** | Extraire TGID | `*(int32_t *)(task + offset_tgid)` — pour distinguer threads de processus |
| **TASK-4** | Extraire comm | `memcpy(entry.comm, task + offset_comm, 16)` |
| **TASK-5** | Extraire UID | Lire `*(void **)(task + offset_real_cred)` → `*(uint32_t *)(cred + offset_cred_uid)` |
| **TASK-6** | Filtrage par PID == TGID | Ne rapporter que les leaders de thread group (processus, pas threads) |
| **TASK-7** | Limite de boucle | Maximum 1024 itérations pour protéger contre liste corrompue |
| **TASK-8** | Validation de pointeurs | Chaque pointeur `tasks.next` doit être dans le range `[LINUX_DIRECT_MAP_BASE, LINUX_DIRECT_MAP_BASE + ram_size)` |

**VERIFY-TASK** : le snapshot contient les mêmes PIDs que `ps -eo pid`.

### 3.2 Format du snapshot

| ID | Exigence | Détails |
|----|----------|---------|
| **SNAP-1** | Header | `data_buf[0..7]` = `uint32_t nr_tasks` + `uint32_t reserved` |
| **SNAP-2** | Entrées | `data_buf[8..]` = tableau de `ck_process_entry` (32 octets chacun) |
| **SNAP-3** | Capacité | Maximum 124 processus (3992 / 32 = 124) |
| **SNAP-4** | Troncation | Si > 124 processus, stocker les 124 premiers, `nr_tasks` = 124 |
| **SNAP-5** | Atomicité visuelle | `data_seq` incrémenté APRÈS l'écriture complète du snapshot |
| **SNAP-6** | Périodicité | Snapshot pris toutes les `SNAPSHOT_INTERVAL` ticks (512) |

**VERIFY-SNAP** : `ck_reader` décode le `data_buf` et affiche la liste complète.

### 3.3 Bootstrap data (ajouts V2)

| ID | Exigence | Détails |
|----|----------|---------|
| **BOOT-V2-1** | `offset_tasks` | `offsetof(struct task_struct, tasks)` — résolu par le loader |
| **BOOT-V2-2** | `offset_pid` | `offsetof(struct task_struct, pid)` |
| **BOOT-V2-3** | `offset_tgid` | `offsetof(struct task_struct, tgid)` |
| **BOOT-V2-4** | `offset_real_cred` | `offsetof(struct task_struct, real_cred)` |
| **BOOT-V2-5** | `offset_cred_uid` | `offsetof(struct cred, uid)` |
| **BOOT-V2-6** | Compatibilité V1 | Les 8 champs V1 ne changent PAS de position |

**VERIFY-BOOT** : les offsets sont imprimés dans dmesg au chargement.

### 3.4 Comm page (changements V2)

| ID | Exigence | Détails |
|----|----------|---------|
| **COMM-V2-1** | `data_seq` incrémenté | Chaque nouveau snapshot incrémente `data_seq` |
| **COMM-V2-2** | `data_buf` écrit | Contient le snapshot binaire des processus |
| **COMM-V2-3** | `version` inchangé | Reste à 1 (le format est extensible via `data_buf`) |
| **COMM-V2-4** | Rétrocompatibilité | `magic`, `tick_count`, `last_tsc`, `status`, `init_comm` inchangés |

### 3.5 ck_reader (mise à jour)

| ID | Exigence | Détails |
|----|----------|---------|
| **READER-1** | Décoder le snapshot | Lire `nr_tasks`, itérer sur les `ck_process_entry` |
| **READER-2** | Afficher la liste | Format: `[PID] COMM (uid=UID)` pour chaque processus |
| **READER-3** | Afficher le compteur | `data_seq = N` |

---

## 4. Structures de données

### 4.1 `ck_bootstrap_data` (V2)

```c
struct ck_bootstrap_data {
    /* V1 fields (positions FIXED — do NOT reorder) */
    uint64_t magic;             /* 0x434B424F4F540001 ("CKBOOT\x00\x01") */
    uint64_t init_task_dm;      /* VA of init_task in direct map */
    uint64_t ram_size;          /* total physical RAM in bytes */
    uint64_t self_phys_base;    /* physical base of co-kernel allocation */
    uint64_t direct_map_base;   /* 0xFFFF888000000000 */
    uint64_t comm_page_va;      /* VA of comm page in co-kernel CR3 */
    uint64_t comm_page_phys;    /* physical address for /dev/mem */
    uint64_t offset_task_comm;  /* offsetof(task_struct, comm) */

    /* V2 additions */
    uint64_t offset_tasks;      /* offsetof(task_struct, tasks) */
    uint64_t offset_pid;        /* offsetof(task_struct, pid) */
    uint64_t offset_tgid;       /* offsetof(task_struct, tgid) */
    uint64_t offset_real_cred;  /* offsetof(task_struct, real_cred) */
    uint64_t offset_cred_uid;   /* offsetof(struct cred, uid) */
};
/* V2: 104 bytes (13 × 8). Fits within COKERNEL_DATA_SIZE (64 KB). */
```

### 4.2 `ck_process_entry` (NEW in V2)

```c
struct ck_process_entry {
    int32_t  pid;           /* task->pid */
    int32_t  tgid;          /* task->tgid */
    uint32_t uid;           /* task->real_cred->uid.val */
    uint32_t reserved;      /* alignment padding */
    char     comm[16];      /* task->comm (TASK_COMM_LEN = 16) */
};
/* 32 bytes per entry. */
```

### 4.3 `data_buf` layout

```
Offset      Content
─────────────────────────────────
0x0000      uint32_t nr_tasks       (nombre de processus)
0x0004      uint32_t reserved       (padding / futur usage)
0x0008      ck_process_entry[0]     (32 bytes)
0x0028      ck_process_entry[1]     (32 bytes)
...
0x0F98      ck_process_entry[123]   (32 bytes : dernier possible)
0x0FB8      espace non utilisé (40 octets)
```

Capacité maximale : **(4000 - 8) / 32 = 124 entrées** (124 × 32 + 8 = 3976).

---

## 5. Algorithme de parcours des processus

```
function take_process_snapshot(comm, boot):
    first = boot->init_task_dm + boot->offset_tasks   // adresse list_head dans init_task
    ptr = *(uint64_t *)first                           // first->next = premier élément
    count = 0
    entries = &comm->data_buf[8]                       // après le header

    while ptr != first AND count < MAX_PROCS AND count < 1024:
        task = ptr - boot->offset_tasks                // container_of(ptr, task_struct, tasks)

        if NOT in_direct_map_range(task, boot):
            break                                      // pointeur invalide, arrêt

        pid  = *(int32_t *)(task + boot->offset_pid)
        tgid = *(int32_t *)(task + boot->offset_tgid)

        // Filtrer : ne garder que les thread group leaders
        if pid == tgid:
            entries[count].pid  = pid
            entries[count].tgid = tgid

            // Lire UID via real_cred
            cred_ptr = *(uint64_t *)(task + boot->offset_real_cred)
            if in_direct_map_range(cred_ptr, boot):
                entries[count].uid = *(uint32_t *)(cred_ptr + boot->offset_cred_uid)
            else:
                entries[count].uid = 0xFFFFFFFF        // marqueur d'erreur

            memcpy(entries[count].comm, task + boot->offset_task_comm, 16)
            count++

        ptr = *(uint64_t *)ptr                         // ptr->next

    *(uint32_t *)&comm->data_buf[0] = count            // nr_tasks
    comm->data_seq++                                   // signal nouveau snapshot
```

**Complexité** : O(n) avec n = nombre de processus. Pour un système de test
(~30-80 processus), le parcours prend < 5 μs.

---

## 6. Garde-fous et limites de sécurité

| ID | Garde-fou | Implémentation |
|----|-----------|----------------|
| **SAFE-1** | Limite d'itérations | `while count < 1024` — arrêt forcé |
| **SAFE-2** | Limite de capacité | `count < MAX_SNAPSHOT_PROCS` (124) |
| **SAFE-3** | Validation de pointeur | Chaque `tasks.next` vérifié dans le range `[direct_map_base, direct_map_base + ram_size)` |
| **SAFE-4** | Validation cred | Le pointeur `real_cred` vérifié dans le range direct map |
| **SAFE-5** | Lecture seule | Aucune écriture dans les structures Linux (sauf comm page) |
| **SAFE-6** | Budget temps | Snapshot < 20 μs (pas de lock, pas d'allocation) |
| **SAFE-7** | Interruption impossible | Le co-kernel s'exécute en PMI (NMI-like), pas interruptible |

---

## 7. Plan de fichiers (delta depuis V1)

```
Modifiés :
  include/shared.h            ← 5 champs bootstrap + ck_process_entry + constantes
  cokernel/cokernel.c         ← take_process_snapshot() + appel périodique
  module/parasite_main.c      ← résolution + écriture des 5 nouveaux offsets
  module/ck_reader.c          ← décodage + affichage snapshot processus
  scripts/build_rootfs.sh     ← test de comparaison ps / snapshot

Aucun nouveau fichier.
```

---

## 8. Pipeline de build et test

### 8.1 Build

Identique à V1 : `make module && make rootfs && make run`

### 8.2 Séquence de test en VM

```bash
# 1. Le module se charge automatiquement au boot (init script)
#    → insmod /parasite.ko

# 2. Vérification V0 : 6 checks d'invisibilité
#    → All PASS

# 3. Vérification V1 : heartbeat + init_comm
#    → tick_count > 0, init_comm = swapper/0

# 4. Vérification V2 : snapshot processus
#    → insmod /ck_reader.ko comm_phys=0x...
#    → dmesg montre la liste des processus

# 5. Comparaison avec ps
#    → ps -eo pid,comm
#    → Les PIDs du snapshot doivent correspondre

# 6. Snapshot heartbeat
#    → sleep 2
#    → insmod /ck_reader.ko comm_phys=0x...
#    → data_seq a augmenté, tick_count a augmenté
```

### 8.3 Critères d'acceptation

| ID | Critère | Condition de succès |
|----|---------|---------------------|
| **V2-1** | Invariants V0/V1 | INV-1 à INV-8 passent |
| **V2-2** | Snapshot présent | `nr_tasks > 0` dans `data_buf` |
| **V2-3** | PID 0 (swapper) | Présent, comm = `"swapper/0"`, uid = 0 |
| **V2-4** | PID 1 (init) | Présent dans la liste |
| **V2-5** | Correspondance ps | Tous les PIDs de `ps -eo pid` sont dans le snapshot |
| **V2-6** | UID correct | UID de chaque processus correspond à `ps -eo pid,uid` |
| **V2-7** | Snapshot heartbeat | `data_seq` augmente entre deux lectures |
| **V2-8** | Stabilité | 10 min d'opération continue, pas de crash |

---

## 9. Contraintes

| ID | Contrainte |
|----|------------|
| C-1 | **CPU unique** — multi-CPU en V6 |
| C-2 | **50 μs par tick maximum** — snapshot < 20 μs |
| C-3 | **Aucun appel de fonction Linux** depuis le co-kernel |
| C-4 | **Aucune allocation dynamique** au runtime |
| C-5 | **Build contre kernel Debian packagé** — headers `linux-headers-*` |
| C-6 | **Noyau cible : Debian 6.12.x amd64** |
| C-7 | **QEMU q35 + KVM** |
| C-8 | **Rétrocompatibilité V1** — champs existants de bootstrap_data et comm_page inchangés |

---

## 10. Hors périmètre (pas dans V2)

| Élément | Reporté à |
|---------|-----------|
| Lecture du page cache (fichiers) | V3 |
| Écriture dans le page cache | V4 |
| Injection réseau (sk_buff) | V5 |
| Multi-CPU (SMP) | V6 |
| Parcours de l'arborescence VFS | V3 |
| Détection de modules cachés | Hors roadmap |
