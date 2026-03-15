# V2 — Lecture de l'état du noyau (task_struct)

### Parcours de la liste des processus · Extraction PID/comm/UID · Rapport structuré · x86-64 · Linux

---

## 1. Objectif

V1 a prouvé que le co-kernel peut lire la mémoire de Linux (lecture de
`init_task.comm` via le direct map) et communiquer les résultats via la
comm page.

V2 passe à la **première lecture structurée** : parcourir la **liste chaînée
des processus** Linux (`task_struct.tasks`) et rapporter un instantané complet
de tous les processus actifs — PID, nom, UID.

C'est le fondement de toute opération future : naviguer dans les structures
de données du noyau sans appeler aucune fonction Linux.

---

## 2. Changements par rapport à V1

| Composant | V1 | V2 |
|-----------|----|----|
| Co-kernel behavior | Lit `init_task.comm` seul | Parcourt la liste complète `init_task → .tasks.next → ...` |
| Bootstrap data | 8 champs (64 octets) | + offsets `tasks`, `pid`, `tgid`, `real_cred`, `cred.uid` |
| Comm page | `init_comm` + `data_buf` vide | `data_buf` contient un snapshot binaire des processus |
| `data_seq` | Fixé à 1 | Incrémenté à chaque nouveau snapshot |
| `ck_reader.ko` | Affiche magic/tick_count/init_comm | + décode et affiche la liste des processus |
| Scripts de test | Vérifie heartbeat | + compare la liste co-kernel avec `ps` |

---

## 3. Design technique

### 3.1 Parcours de la liste des processus

La liste des processus Linux est une **liste circulaire doublement chaînée**
reliée par le champ `task_struct.tasks` (de type `struct list_head`).

```
init_task.tasks ──→ task_A.tasks ──→ task_B.tasks ──→ ... ──→ init_task.tasks
                ←──                ←──                ←──
```

Le co-kernel :
1. Part de `init_task_dm` (adresse direct map, fournie par bootstrap_data)
2. Lit `tasks.next` (premier `next` de la `list_head`)
3. Soustrait l'offset de `.tasks` pour obtenir le `task_struct *` suivant
4. Lit `pid`, `tgid`, `comm`, et le UID (via `real_cred→uid`)
5. Répète jusqu'à revenir à `init_task`

**container_of** en freestanding :
```c
#define TASK_FROM_TASKS(tasks_ptr, offset_tasks) \
    ((unsigned long)(tasks_ptr) - (offset_tasks))
```

### 3.2 Extraction du UID

Le UID est accessible via `task_struct.real_cred→uid` :

```
task_struct
  └── .real_cred (pointeur vers struct cred)
        └── .uid (kuid_t → uint32_t)
```

Le co-kernel :
1. Lit `*(uint64_t *)(task + offset_real_cred)` → pointeur `cred *`
2. Ce pointeur est une adresse noyau Linux → accessible via direct map
3. Lit `*(uint32_t *)(cred + offset_cred_uid)` → UID

### 3.3 Format du snapshot dans `data_buf`

Le snapshot utilise un format binaire compact dans les 4000 octets de
`data_buf` :

```
Offset  Size    Field
──────────────────────────────
0       4       nr_tasks (uint32_t) — nombre de processus
4       4       reserved (padding)
8       N×32    tableau de process_entry (32 octets chacun)

Chaque process_entry (32 octets) :
  Offset  Size  Field
  0       4     pid (int32_t)
  4       4     tgid (int32_t)
  8       4     uid (uint32_t)
  12      4     reserved
  16      16    comm[16] (char, null-terminated)
```

Avec 32 octets par entrée et 4000 - 8 = 3992 octets disponibles,
on peut stocker **124 processus** — largement suffisant pour un
système de test en VM.

### 3.4 Logique du co-kernel (V2)

Le snapshot est pris **périodiquement** (pas à chaque tick, pour limiter
le coût) — toutes les N ticks, configurable.

```c
void component_tick(void)
{
    /* ... heartbeat (unchanged from V1) ... */

    /* Snapshot tous les SNAPSHOT_INTERVAL ticks */
    if (comm->tick_count % SNAPSHOT_INTERVAL == 0) {
        take_process_snapshot(comm, boot);
    }
}
```

**Première invocation** : le snapshot est pris lors de l'initialisation
(premier tick), puis répété périodiquement.

### 3.5 Garde-fous

Le parcours de la liste des processus est **dangereux** si une structure est
corrompue ou si la liste est en cours de modification. Protections :

| Garde-fou | Mécanisme |
|-----------|-----------|
| Limite du nombre de processus | Maximum 124 (capacité `data_buf`) — arrêt si dépassé |
| Limite de boucle | Maximum 1024 itérations — protection contre liste circulaire cassée |
| Validation du pointeur | Vérifier que l'adresse est dans le range du direct map |
| Pas d'écriture dans les structures Linux | Lecture seule — aucun risque de corruption |
| Budget temps | Snapshot < 20 μs (parcours léger, pas de lock) |

---

## 4. Modifications des fichiers

### 4.1 `include/shared.h`

- Ajout des champs d'offset dans `ck_bootstrap_data` :
  - `offset_tasks` — `offsetof(task_struct, tasks)`
  - `offset_pid` — `offsetof(task_struct, pid)`
  - `offset_tgid` — `offsetof(task_struct, tgid)`
  - `offset_real_cred` — `offsetof(task_struct, real_cred)`
  - `offset_cred_uid` — `offsetof(struct cred, uid)`

- Ajout de la structure `ck_process_entry` (32 octets)

- Ajout des constantes :
  - `SNAPSHOT_INTERVAL` — fréquence de snapshot (ex: 512 ticks)
  - `MAX_SNAPSHOT_PROCS` — 124

### 4.2 `cokernel/cokernel.c`

- Nouvelle fonction `take_process_snapshot()` :
  - Parcourt `init_task.tasks` via linked list
  - Écrit les entrées dans `comm->data_buf`
  - Met à jour `comm->data_seq`

- `component_tick()` appelle le snapshot périodiquement

### 4.3 `module/parasite_main.c`

- Résolution et écriture des nouveaux offsets dans `bootstrap_data`

### 4.4 `module/ck_reader.c`

- Décodage et affichage du snapshot de processus depuis `data_buf`

### 4.5 `scripts/build_rootfs.sh`

- Ajout d'un test de comparaison `ps` / snapshot co-kernel

---

## 5. Critères de vérification

| ID | Test | Résultat attendu |
|----|------|------------------|
| **V2-1** | Invariants V0 | Les 6 checks d'invisibilité passent toujours |
| **V2-2** | Heartbeat V1 | `tick_count` > 0 et croissant |
| **V2-3** | `init_comm` V1 | `"swapper/0"` |
| **V2-4** | Snapshot présent | `data_seq` > 0 et `nr_tasks` > 0 dans `data_buf` |
| **V2-5** | PID 0 (swapper) | Présent dans la liste avec comm=`"swapper/0"` |
| **V2-6** | PID 1 (init) | Présent dans la liste |
| **V2-7** | Cohérence avec `ps` | Tous les PIDs visibles par `ps` sont présents dans le snapshot |
| **V2-8** | Snapshot heartbeat | `data_seq` augmente au fil du temps (snapshots répétés) |
| **V2-9** | Stabilité | 10 min d'opération continue, pas de crash |

---

## 6. Estimation du code

| Fichier | Lignes modifiées/ajoutées |
|---------|--------------------------|
| `include/shared.h` | ~25 lignes |
| `cokernel/cokernel.c` | ~80 lignes |
| `module/parasite_main.c` | ~10 lignes |
| `module/ck_reader.c` | ~40 lignes |
| `scripts/build_rootfs.sh` | ~15 lignes |
| **Total** | **~170 lignes** |
