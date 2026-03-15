# V3 — Lecture du page cache (fichiers)

### Navigation VFS · xarray traversal · Lecture de fichiers depuis le page cache · x86-64 · Linux

---

## 1. Objectif

V2 a démontré que le co-kernel peut naviguer les structures de données du
noyau (liste chaînée `task_struct`) et rapporter un instantané structuré.

V3 passe à la **lecture de fichiers** : le co-kernel navigue les structures
VFS pour accéder au **page cache** d'un fichier cible, lit son contenu, et
le rapporte via la comm page. C'est la première opération de **lecture de
données applicatives** — pas juste des métadonnées noyau, mais le contenu
réel d'un fichier.

Le chemin complet est :
```
inode → i_mapping (address_space *) → i_pages (xarray) → folio * → pfn → phys → direct map → lecture
```

---

## 2. Changements par rapport à V2

| Composant | V2 | V3 |
|-----------|----|----|
| Co-kernel behavior | Parcours `task_struct.tasks` | + lecture xarray page cache d'un fichier cible |
| Bootstrap data | 13 champs (104 octets) | + 8 champs VFS/page cache (168 octets total) |
| Loader module | Résout offsets task_struct | + résout le chemin fichier → inode, offsets VFS |
| Comm page `data_buf` | 4000 octets → snapshot processus | Split : 2000 octets processus + 2000 octets contenu fichier |
| `ck_reader.ko` | Décode snapshot processus | + affiche le contenu du fichier lu depuis le page cache |
| Scripts de test | Compare snapshot vs `ps` | + vérifie que le contenu CANARY est retrouvé |
| Capacité processus | 124 max | 62 max (dû au split `data_buf`) |

---

## 3. Design technique

### 3.1 Résolution du fichier cible (loader)

Le module loader résout un chemin fichier (par défaut `/tmp/test`) vers
un `struct inode *` au moment du chargement :

```c
kern_path("/tmp/test", LOOKUP_FOLLOW, &path);
inode = d_inode(path.dentry);
inode_dm = LINUX_DIRECT_MAP_BASE + virt_to_phys(inode);
```

L'adresse direct-map de l'inode est passée au co-kernel via `bootstrap_data`.

Si le fichier n'existe pas au moment du chargement, `target_inode_dm = 0`
et le co-kernel ne tente aucune lecture.

### 3.2 Navigation VFS depuis le co-kernel

Le co-kernel navigue la chaîne VFS en lecture seule :

```
target_inode_dm           ← adresse DM de l'inode (fournie par loader)
  └── inode + offset_i_mapping    → struct address_space *mapping
        └── mapping + offset_a_i_pages  → struct xarray (embedded)
              └── xarray.xa_head        → void *entry
                    └── Si leaf : folio *
                    └── Si xa_node : descendre slots[0] récursivement
```

Tous les pointeurs intermédiaires sont des adresses noyau dans le direct
map (allocations slab) — accessibles depuis le co-kernel via CR3.

### 3.3 Traversée de l'xarray

L'xarray (`address_space->i_pages`) stocke les pages du cache indexées
par le numéro de page dans le fichier. Pour un fichier de ≤ 4 Ko (1 page),
l'entrée 0 est stockée directement dans `xa_head`.

**Cas simple (1 entrée, index 0)** :
```
xa_head = folio *   (bits 0-1 à zéro → pointeur régulier)
```

**Cas multi-niveaux** :
```
xa_head = xa_node * | 2    (xa_is_internal — bit 1 posé)
  └── xa_node.slots[0] = folio *   ou   xa_node * | 2 (niveau suivant)
```

L'algorithme descend les niveaux via `slots[0]` jusqu'à trouver un
pointeur folio (non-interne).

### 3.4 Conversion folio → adresse physique

Les `struct folio *` / `struct page *` sont des adresses dans le **vmemmap**
(`0xFFFFEA0000000000`). Le vmemmap N'EST PAS mappé dans le CR3 du co-kernel,
mais on n'a pas besoin de le déréférencer — on utilise le pointeur comme
un **index** :

```
pfn = (folio_ptr - vmemmap_base) / sizeof_struct_page
phys = pfn * PAGE_SIZE
data_va = LINUX_DIRECT_MAP_BASE + phys
```

La donnée (contenu du fichier) est ensuite lue via le direct map.

### 3.5 Layout du `data_buf` (V3)

Le buffer de 4000 octets est **divisé en deux zones** :

```
data_buf[4000]:

  ── Zone 1 : Snapshot processus (0..1999) ──
  [0x000..0x003]  uint32_t nr_tasks
  [0x004..0x007]  uint32_t reserved
  [0x008..0x7CF]  ck_process_entry[0..61]   (62 max × 32 octets)

  ── Zone 2 : Contenu fichier (2000..3999) ──
  [0x7D0..0x7D3]  uint32_t file_bytes       (octets valides lus)
  [0x7D4..0x7D7]  uint32_t file_status      (0=ok, 1=no_inode, 2=no_page, etc.)
  [0x7D8..0xF9F]  uint8_t  file_content[1992]
```

- Zone processus : identique à V2 mais réduite de 124 → 62 entrées max
- Zone fichier : 1992 octets de contenu (suffisant pour tout fichier < 2 Ko)

### 3.6 Garde-fous

| Garde-fou | Détail |
|-----------|--------|
| Validation pointeur inode | `in_direct_map()` avant chaque déréférencement |
| Validation mapping | `in_direct_map(mapping_ptr)` |
| Validation xarray entries | `in_direct_map(node_ptr)` pour chaque xa_node |
| Folio dans vmemmap | `folio_ptr ∈ [vmemmap_base, vmemmap_base + ram_pages * sizeof_page)` |
| Donnée dans direct map | `data_va ∈ [direct_map_base, direct_map_base + ram_size)` |
| Limite de profondeur xarray | Maximum 4 niveaux de descente (xarray max depth pour 64-bit index) |
| Budget temps | Lecture page cache < 5 μs (quelques déréférencements) |
| Lecture seule | Aucune écriture dans les structures VFS |

---

## 4. Validation

### Critère d'acceptation principal

```bash
# Dans l'init script, AVANT le chargement du module :
echo "CANARY_V3_TEST" > /tmp/test

# Après chargement :
insmod /lib/modules/ck_reader.ko comm_phys=0x...
# → dmesg montre : "File content (N bytes): CANARY_V3_TEST"
```

### Checklist

| # | Critère | Vérification |
|---|---------|--------------|
| 1 | Invariants V0/V1/V2 | 6 checks invisibilité PASS |
| 2 | Heartbeat | `tick_count` croissant |
| 3 | Snapshot processus | `nr_tasks > 0`, PID 0 et PID 1 présents |
| 4 | Fichier lu | `file_bytes > 0`, `file_status = 0` |
| 5 | Contenu correct | `file_content` contient `"CANARY_V3_TEST"` |
| 6 | Cohérence taille | `file_bytes` = taille réelle du fichier |
| 7 | Pas de crash | 10+ secondes d'opération stable |

---

## 5. Plan de fichiers (delta depuis V2)

```
Modifiés :
  include/shared.h            ← 8 champs bootstrap V3 + constantes file data + split data_buf
  cokernel/cokernel.c         ← read_target_file() + xarray traversal
  module/parasite_main.c      ← kern_path → inode, offsets VFS, module param target_path
  module/ck_reader.c          ← décodage + affichage contenu fichier
  scripts/build_rootfs.sh     ← création CANARY file, vérification contenu

Aucun nouveau fichier.
```
