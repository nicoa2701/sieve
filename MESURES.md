# Mesures

Ce fichier est réécrit à chaque campagne : il décrit l'état mesuré le plus
récent, pas l'évolution dans le temps. L'évolution est dans `HISTORIQUE.md`.

Les défauts sont dans `BUG.md`, le récit chronologique dans `HISTORIQUE.md`.

**Règle** — aucun chiffre sans ses trois références : le commit mesuré (source
exacte), la date et l'heure, le CPU. Chaque section ci-dessous les porte, pour
rester lisible isolément.

---

## Campagne C1 — 2026-08-23

### Provenance

| | |
|---|---|
| **Commit** | `9d2b5764ef51f62273ef03932f95e7a29027d1c7` |
| **Date** | 2026-08-23, 12:31:41 → 12:35:06 (UTC+02:00) |
| **CPU** | Intel Core i5-9300HF @ 2,40 GHz — 4 cœurs / 8 threads |

Arbre propre au moment du build (`git status` vide), binaire reconstruit par
`make clean && make` à 12:31:41 depuis ce commit exact.

**Le code a évolué depuis.** Les commits postérieurs n'ont touché au chemin
chaud d'aucune manière : commentaires, puis une comparaison ajoutée dans
`main`, hors de toute boucle. Contrôle daté — commit `bdce01b` ·
2026-08-23 12:51:14 · i5-9300HF : 10⁹ en 21,9 ms (C1 : 22,8), 10¹⁰ en 0,28 s
(C1 : 0,26), [10¹², +10¹⁰] en 0,59 s (C1 : 0,84). Les chiffres de C1 tiennent,
dans la dispersion annoncée.

### Plateforme

| | |
|---|---|
| L1d | 32 KiB par cœur (128 KiB, 4 instances) |
| L2 | 256 KiB par cœur (1 MiB, 4 instances) |
| L3 | 8 MiB, partagé |
| SIMD | AVX2, **pas** d'AVX-512 — le pré-crible passe par le C vectorisable |
| Noyau | 6.6.87.2-microsoft-standard-WSL2 |
| Compilateur | gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| Build | `make` → `-O3 -g -Wall -Wextra -march=native -fopenmp -DSINK_TAIL=0` |
| Référence | primesieve 12.10 |

Paramètres retenus automatiquement à 10¹² (`-v`) : segment 1024 KiB, bloc L1
16 KiB, tranche L2 32 KiB, plaque 128 KiB, fenêtre de seau 32 KiB sur
64 anneaux, pré-crible 27 premiers en 12 tables / 3 passes, 67 KiB.

### Méthode

Trois exécutions par point, **meilleur temps retenu**. `primesieve` est appelé
avec `-q` : son affichage de progression fausse la mesure. Machine sous WSL2,
sans gouverneur de fréquence fixé — la dispersion résiduelle reste de l'ordre
de 10 %, donc un écart inférieur à ~15 % n'est pas concluant.

### Comparaison à primesieve 12.10, 8 threads

Commit `9d2b576` · 2026-08-23 12:31:54–12:33:11 · i5-9300HF

Comptage complet :

| Borne | roue12 | primesieve | rapport |
|---|---|---|---|
| π(10¹⁰) | **0,26 s** | 0,35 s | 1,35× |
| π(10¹¹) | **4,27 s** | 4,97 s | 1,16× |

Intervalle de largeur 10¹⁰ :

| Début | roue12 | primesieve | rapport |
|---|---|---|---|
| 10¹² | **0,84 s** | 1,01 s | 1,20× |
| 10¹³ | 1,26 s | 1,20 s | 0,95× |
| 10¹⁴ | 1,65 s | 1,33 s | 0,81× |
| 10¹⁵ | 2,44 s | 1,61 s | 0,66× |

**Point de croisement entre 10¹² et 10¹³.** En deçà, roue12 devance la
référence de 15 à 35 %. Au-delà, quand la quasi-totalité des premiers passe
par les seaux, il perd jusqu'à 1,5×. C'est le seul régime où le programme est
distancé, et donc la seule marge de progrès identifiée.

### Empreinte mémoire

Commit `9d2b576` · 2026-08-23 12:33:27–12:33:33 · i5-9300HF

| Intervalle | roue12 | primesieve |
|---|---|---|
| [10¹⁵, +10¹⁰] | 158 404 KiB | 139 648 KiB |

L'empreinte n'explique donc pas le retard à 10¹⁵ : elle est du même ordre. Le
coût est par entrée de seau, pas en volume.

### Coût des étages

Commit `9d2b576` · 2026-08-23 12:33:46–12:33:49 · i5-9300HF

Comptage de 10⁹, 8 threads, chronomètre interne du programme, meilleur de
cinq. Chaque étage désactivé isolément ; toutes les variantes donnent le bon
résultat.

| Configuration | Temps | Écart |
|---|---|---|
| défaut | 22,8 ms | — |
| `-S 0` (plaque coupée) | 23,2 ms | +2 % |
| `-Q 0` (préchargement neutralisé) | 23,3 ms | +2 % |
| `-B 0` (tranche L2 coupée) | 24,0 ms | +5 % |
| `-K 0` (seaux coupés) | 24,2 ms | +6 % |
| `-c 1` | 24,8 ms | +9 % |
| `-b 0` (bloc L1 coupé) | 28,8 ms | +26 % |
| `-s 32` | 30,8 ms | +35 % |
| `-p 0` (pré-crible coupé) | 36,4 ms | +60 % |
| `-t 1` | 88,8 ms | ×3,9 |

À cette borne le pré-crible est de loin le poste le plus rentable, et les
seaux ne rapportent que 6 % — attendu : ils ne payent qu'au-delà de quelques
segments de portée. Accélération sur 8 threads : 3,9×, pour 4 cœurs physiques.

### Validation

Commit `9d2b576` · 2026-08-23 12:34:01–12:35:06 · i5-9300HF

Depuis, ce protocole est figé dans `check.sh` : `make check` rejoue
121 contrôles en une dizaine de secondes, `make sanitize` ajoute les passes
ASan et UBSan sur les deux variantes `SINK_TAIL`.

- π(10⁶) à π(10¹¹) exacts : 78 498 · 664 579 · 5 761 455 · 50 847 534 ·
  455 052 511 · 4 118 054 813.
- 60 intervalles aléatoires dans [0, 2·10⁶], croisés contre un crible de
  référence indépendant : **0 écart**. Rejoués sous `-t 1`, `-s 32 -c 1`,
  `-p 0`, `-K 32` et `-b 0 -B 0 -S 0` : **0 écart** dans les six
  configurations.
- Cas limites, tous corrects : `0`→0, `1`→0, `2`→1, `30`→10, `[1,1]`→0,
  `[5,11]`→3, `[0,100]`→25, `[10¹⁶−1, 10¹⁶]`→0.
- ASan + UBSan : 0 erreur sur `0`, 10⁸ et `[10¹², +10⁸]`, dans les deux
  variantes `SINK_TAIL=0` et `SINK_TAIL=1`.
- Compilation `-Wall -Wextra` sans un seul avertissement, y compris la
  variante `-DRECOMPUTE_TURN=1`, qui donne le même résultat à 10⁹.

### Défauts connus

Recensés dans `BUG.md`, qui fait foi. Les cinq sont corrigés.

Hors défauts : B5 n'a pas de test de non-régression, aucun outil n'atteignant
proprement son chemin d'échec.
