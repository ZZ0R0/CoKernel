# CoKernel — Roadmap

---

## Vision

**CoKernel** est un **noyau parallèle** qui coexiste avec Linux sur la même
machine physique. Il s'exécute uniquement via interruption PMI hardware (pas de
thread, pas de structure noyau), et opère sur les **structures de données que
Linux a déjà construites en mémoire** — page cache, réseau, VFS, drivers — sans
jamais appeler une seule fonction du noyau hôte.

Linux a initialisé le matériel et construit toute l'infrastructure. Le co-kernel
la **réutilise silencieusement** en lisant et écrivant directement dans ces
structures via le direct map. C'est un consommateur invisible de l'état du noyau
hôte.

```
┌──────────────────────────────────────────────────┐
│           Matériel (CPU, RAM, NIC, disque)        │
├──────────────────────────────────────────────────┤
│  Linux Kernel                                     │
│    Initialise tout : drivers, VFS, réseau, MM     │
│    Structures de données vivantes en mémoire      │
│    task_struct, sk_buff, inode, page cache, ...   │
├──────────────────────────────────────────────────┤
│  CoKernel (PMI, direct map, zéro structure Linux) │
│    Lit et écrit dans les structures de Linux       │
│    Réutilise l'infrastructure déjà en place        │
│    100% invisible — pas de thread, pas de module   │
│    Cross-compatible : travaille sur les structures  │
│    génériques, pas sur les drivers spécifiques     │
└──────────────────────────────────────────────────┘
```

**Invisibilité non-négociable** (acquis V0) :

- Zéro `task_struct`, zéro thread, zéro timer, zéro workqueue
- Exécution purement hardware (PMI) — aucune fonction noyau hookée
- Mémoire hors direct map — Linux ne peut ni lire ni écrire nos pages
- Exécution séquentielle bornée (<50 μs par tick)
- `ENDBR64` + `iretq` — CET/IBT/Shadow Stack intacts

**Cross-compatibilité** : en opérant sur les structures génériques du noyau
(pas sur les registres matériels des drivers), le co-kernel fonctionne avec
n'importe quel driver NIC, n'importe quel filesystem, n'importe quelle
configuration — tant que c'est le même noyau Linux.

---

## Principe de développement

Chaque version ajoute UN sous-système de Linux au répertoire du co-kernel.
Chaque version est petite, testable, et nécessaire à la suivante.
VN+1 ne commence que lorsque VN passe tous ses VERIFY.

---

## V0 — Foundation PoC ✅

**Goal**: invisible execution component coexisting with Linux.

| Deliverable | Status |
|-------------|:------:|
| Invisible memory (`alloc_pages` + `set_direct_map_invalid`) | ✅ |
| Standalone page tables (own CR3) | ✅ |
| Hardware execution via PMI (IDT 0x42 + PMU + LAPIC) | ✅ |
| Dual-CR3 trampoline (same VA in both address spaces) | ✅ |
| Module self-erasure (`list_del`, `kobject_del`, symbol scrub) | ✅ |
| Bypass CET/IBT (`ENDBR64`), CR pinning (raw asm), KASLR (kprobes) | ✅ |
| Full build system (cokernel binary + module + rootfs + QEMU) | ✅ |
| 6/6 invisibility checks passed in QEMU KVM | ✅ |

**Result**: co-kernel runs, increments a counter every ~0.5 ms via PMI,
undetectable by Linux.

---

## V1 — Communication & Verification 🔄

**Sous-système** : direct map + canal de vérification.

Le co-kernel obtient un accès en lecture à **toute la RAM physique** et dispose
d'un canal pour prouver qu'il fonctionne (comm page).

| Task | Description |
|------|-------------|
| Map physical RAM into co-kernel CR3 | Mirror Linux direct map (`0xFFFF888000000000`) via 2 MB huge pages |
| Shared comm page | 1 page restée dans le direct map — co-kernel écrit, userspace lit |
| Read `init_task.comm` | Preuve que le direct map fonctionne |
| `bootstrap_data` | Loader passe `init_task`, `ram_size`, `comm_page_phys` |
| `ck_verify` userspace tool | Lit la comm page via `/dev/mem`, affiche heartbeat + init_comm |

**VERIFY**: `ck_verify` montre heartbeat croissant, `init_comm = "swapper/0"`,
écriture fichier valide. Les 6 checks V0 passent toujours.

~150 lignes de nouveau code.

---

## V2 — Lecture de l'état du noyau (task_struct)

**Sous-système** : structures de processus.

Le co-kernel parcourt la liste des processus Linux et rapporte les résultats.
C'est la première lecture structurée de données noyau — le fondement de tout
le reste.

| Task | Description |
|------|-------------|
| Walk `task_struct` linked list | `init_task` → `.tasks.next` → PID, comm, UID |
| `pahole`-based offset extraction | Script qui génère les offsets au build time |
| Structured output in comm page | Snapshots processus dans `data_buf` |
| `ck_verify` update | Affiche la liste des processus, compare avec `ps` |

**VERIFY**: `ck_verify` affiche une liste de processus identique à `ps`.

---

## V3 — Lecture du page cache (fichiers)

**Sous-système** : VFS / page cache (lecture).

Le co-kernel navigue les structures VFS pour lire des fichiers depuis le
page cache — sans aucun appel VFS, uniquement via manipulation directe des
structures `super_block` → `inode` → `address_space` → xarray → `struct page`.

| Task | Description |
|------|-------------|
| Traverse superblocks | `super_blocks` → `s_list` → `inode` |
| Read page cache pages | `inode->i_mapping->i_pages` (xarray) → `struct page *` → phys → direct map |
| Report via comm page | Contenu de fichiers cibles affiché par `ck_verify` |

**VERIFY**: `echo "CANARY" > /tmp/test` → `ck_verify` montre `CANARY` trouvé
dans le page cache.

---

## V4 — Écriture dans le page cache (fichiers)

**Sous-système** : VFS / page cache (écriture).

Le co-kernel peut maintenant **écrire** dans le page cache. Linux voit les
pages comme dirty et les flush vers le disque via writeback. C'est la première
**action** du co-kernel (pas juste de la lecture).

| Task | Description |
|------|-------------|
| Write to cached pages | Modifier les octets d'une page déjà en cache |
| Mark page dirty | Set `PG_dirty` flag sur la `struct page` pour déclencher writeback |
| Target file strategy | Écrire dans un fichier existant ou pré-ciblé |

**VERIFY**: le co-kernel écrit `"COKERNEL_WAS_HERE"` dans le page cache d'un
fichier → `cat /tmp/target` montre le message après writeback.

C'est critique : une fois qu'on peut écrire dans des fichiers, on a un canal
de sortie qui fonctionne avec **n'importe quel filesystem, n'importe quel
disque** — totalement cross-compatible.

---

## V5 — Injection réseau via structures Linux

**Sous-système** : networking stack (structures `sk_buff`, queues).

Le co-kernel injecte des paquets dans le chemin de transmission réseau de
Linux en manipulant directement les structures du networking subsystem. Pas
de code NIC-specific — on travaille au niveau des `sk_buff` et des queues
`qdisc` / `netdev_queue`, structures identiques quel que soit le driver.

| Task | Description |
|------|-------------|
| Research injection point | Identifier le point le plus sûr : `qdisc` queue, `netdev_queue`, `sk_buff_head` |
| `sk_buff` construction in co-kernel memory | Construire un sk_buff valide pointant vers notre frame |
| Queue injection during PMI | Insérer le sk_buff dans la queue TX — le driver le transmettra |
| UDP frame builder | Ethernet + IP + UDP construit par le co-kernel |
| Cross-driver validation | Tester avec e1000 et virtio-net |

**VERIFY**: co-kernel envoie des paquets UDP → capturés sur l'hôte via
`tcpdump`. Fonctionne avec **e1000 et virtio-net** sans changement de code.

**Note** : c'est la version la plus complexe et la plus risquée. L'injection
dans les queues réseau pendant que Linux est gelé (PMI) require une
compréhension profonde des invariants du networking stack. La recherche
du bon point d'injection est un livrable en soi.

---

## V6 — Multi-CPU (SMP)

**Sous-système** : exécution parallèle.

| Task | Description |
|------|-------------|
| Per-CPU PMI handler | Installer le handler sur chaque CPU via IPI au chargement |
| Per-CPU state | Stacks, tick counters, buffers séparés par CPU |
| Synchronisation co-kernel | Atomics / spinlock léger pour l'accès aux structures partagées (comm page, ring buffer) |
| Sérialisation des écritures | Un seul CPU à la fois peut écrire dans le page cache ou les queues réseau |

**VERIFY**: stable sur `-smp 4` avec charge Linux concurrente. Heartbeat
correct sur chaque CPU.

---

## V7 — Surveillance avancée

**Sous-système** : TTY, credentials, network capture.

| Task | Description |
|------|-------------|
| Keylogging | Poll TTY / `n_tty_data.read_buf` via direct map |
| Credential harvesting | `task_struct.cred` → uid, gid, capabilities |
| Network capture | Lire `sk_buff` dans les receive queues — summaries de paquets |
| Ring buffer interne | Stockage structuré de toutes les données collectées |

**VERIFY**: keystrokes, credentials, et summaries réseau apparaissent dans
le ring buffer, lisibles via comm page ou exfiltration réseau.

---

## V8 — Stealth & Anti-Forensics

**Sous-système** : suppression de traces.

| Task | Description |
|------|-------------|
| Suppression comm page | Toute sortie passe par le réseau (V5) ou le page cache (V4) |
| `dmesg` scrubbing | Écraser les entrées `log_buf` mentionnant le loader |
| PMU counter save/restore | `perf stat` ne montre rien d'anormal |
| IDT vector rotation | Changer de vecteur périodiquement |
| TSC compensation | Masquer le temps CPU volé par PMI |

**VERIFY**: tous les checks V0 passent + `perf stat` normal + `dmesg`
propre + zéro comm page visible.

---

## Récapitulatif

| Version | Sous-système | Capacité acquise | Vérification |
|---------|-------------|------------------|-------------|
| **V0** ✅ | Exécution | Existe invisiblement | 6 checks d'invisibilité |
| **V1** | Direct map | Lit toute la RAM | `ck_verify` + heartbeat |
| **V2** | Processus | Voit les task_struct | Liste process = `ps` |
| **V3** | Page cache R | Lit des fichiers | Trouve `CANARY` |
| **V4** | Page cache W | Écrit dans des fichiers | `cat` montre le message |
| **V5** | Réseau | Envoie des paquets | `tcpdump` capture UDP |
| **V6** | SMP | Multi-CPU | Stable sur `-smp 4` |
| **V7** | Surveillance | Keylog, creds, netcap | Données dans ring buffer |
| **V8** | Stealth | Zéro trace | Tous checks + `perf` propre |

---

## Conventions

- Chaque version a son propre `markdown/VN/README.md` et `markdown/VN/SPEC.md`
- Le code vit dans `cokernel/`, `module/`, `scripts/`, `tools/`
- VN+1 ne commence que lorsque VN est fonctionnel et vérifié
- **Jamais de structure noyau Linux** côté co-kernel (pas de thread, timer, workqueue)
- **Jamais d'appel à une fonction noyau** depuis le co-kernel
- Le co-kernel manipule les **structures de données** de Linux, pas ses **fonctions**
