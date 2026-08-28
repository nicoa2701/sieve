# Mesures

Ce fichier est réécrit à chaque campagne : il décrit l'état mesuré le plus
récent, pas l'évolution dans le temps. L'évolution est dans `HISTORIQUE.md`.
Les défauts sont dans `BUG.md`.

**Règle** — aucun chiffre sans ses trois références : le commit mesuré (source
exacte), la date et l'heure, le CPU. Chaque section ci-dessous les porte, pour
rester lisible isolément.

---

## Campagne C2 — 2026-08-23

Remplace C1, dont la méthode ne refroidissait pas entre les passages et
mesurait donc une machine froide au premier point de chaque série.

### Provenance

| | |
|---|---|
| **Commit** | `491cd40a010b37be2924b9809e7994d1352bea3b` |
| **Date** | 2026-08-23, 13:22:48 → 14:06:43 (UTC+02:00) |
| **CPU** | Intel Core i5-9300HF @ 2,40 GHz — 4 cœurs / 8 threads |

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

### Méthode

**Refroidissement entre chaque passage : 10 × la durée de la mesure, borné à
[3 s, 30 s].** C'est le seul changement de méthode entre C1 et C2, et il
suffit à déplacer les chiffres de 20 %.

Meilleur temps retenu, sur 3 passages pour la comparaison et 5 pour
l'ablation. `primesieve` est appelé avec `-q` : son affichage de progression
fausse la mesure. Machine sous WSL2, sans gouverneur de fréquence fixé.

**Résolution.** Deux mesures indépendantes du même point donnent une bande de
reproductibilité de ±1,5 % à 10¹⁰ au chronomètre interne (249,3 puis 252,7 ms
pour le défaut). Rien en dessous de ~3 % n'est interprétable. L'ablation est
faite à 10¹⁰ et non à 10⁹ : à 10⁹ la bande atteint ±8 %, ce qui noie tous les
étages sauf les plus gros. `/usr/bin/time` plafonne à 10 ms, soit 4 % à cette
borne, d'où le recours au chronomètre interne du programme.

### Comparaison à primesieve 12.10, 8 threads

Commit `491cd40` · 2026-08-23 13:22:48–13:35:55 · i5-9300HF

Comptage complet :

| Borne | roue12 | primesieve | rapport |
|---|---|---|---|
| π(10¹⁰) | **0,25 s** | 0,32 s | 1,28× |
| π(10¹¹) | **3,31 s** | 4,26 s | 1,29× |

Intervalle de largeur 10¹⁰ :

| Début | roue12 | primesieve | rapport |
|---|---|---|---|
| 10¹² | **0,49 s** | 0,54 s | 1,10× |
| 10¹³ | 0,72 s | 0,65 s | 0,90× |
| 10¹⁴ | 1,02 s | 0,84 s | 0,82× |
| 10¹⁵ | 1,66 s | 1,10 s | 0,66× |

**Point de croisement entre 10¹² et 10¹³.** En deçà, roue12 devance la
référence de 10 à 30 %. Au-delà, quand la quasi-totalité des premiers passe
par les seaux, il perd jusqu'à 1,5×. C'est le seul régime où le programme est
distancé, et donc la seule marge de progrès identifiée.

### Empreinte mémoire

Commit `491cd40` · 2026-08-23 13:33 · i5-9300HF

| Intervalle | roue12 | primesieve |
|---|---|---|
| [10¹⁵, +10¹⁰] | 158 404 KiB | 139 392 KiB |

L'empreinte n'explique donc pas le retard à 10¹⁵ : elle est du même ordre. Le
coût est par entrée de seau, pas en volume.

### Coût des étages

Commit `491cd40` · 2026-08-23 14:01:55–14:05:15 · i5-9300HF

Comptage de 10¹⁰, 8 threads, chronomètre interne, meilleur de 5, chaque étage
désactivé isolément. Toutes les variantes donnent le bon résultat.

| Configuration | Temps | Écart |
|---|---|---|
| **défaut** | **249,3 ms** | — |
| `-S 0` (plaque coupée) | 248,0 ms | *non résolu* |
| `-K 0` (seaux coupés) | 248,4 ms | *non résolu* |
| `-Q 0` (préchargement neutralisé) | 249,1 ms | *non résolu* |
| `-B 0` (tranche L2 coupée) | 258,5 ms | +3,7 % |
| `-c 1` | 259,7 ms | +4,2 % |
| `-b 0` (bloc L1 coupé) | 308,3 ms | +24 % |
| `-p 0` (pré-crible coupé) | 367,6 ms | +47 % |
| `-s 32` | 397,1 ms | +59 % |

Contrôles de reproductibilité dans la même série : défaut à 252,7 ms au second
passage, `-S 0` à 250,9 ms.

**Les trois lignes « non résolu » ne sont pas des coûts nuls, ce sont des
non-opérations.** `-v` le confirme à cette borne : la plaque s'éteint
d'elle-même (`sqrt(N) <= plaque : elle vide la bande directe`) et le seuil des
seaux vaut 737 280, très au-dessus de √10¹⁰ = 100 000, donc aucun premier n'y
entre. Couper ce qui ne tourne pas ne change rien — c'est le comportement
attendu, pas une contre-performance de ces deux étages.

À cette borne le pré-crible et le dimensionnement du segment dominent.
Accélération sur 8 threads, mesurée à 10⁹ : 3,8× pour 4 cœurs physiques.

**Non mesuré** : le coût réel de la plaque et des seaux, qui demande une borne
où ils travaillent, au-delà de 10¹³. C'est le trou principal de cette
campagne, et il porte précisément sur le régime où le programme est distancé.

### Validation

Commit `491cd40` · 2026-08-23 14:05:59–14:06:43 · i5-9300HF

Le protocole est figé dans `check.sh` et rejouable :

- `make check` — 121 contrôles, 0 échec, 9 s. π(10ⁿ), cas limites, intervalles
  hauts, 60 intervalles aléatoires contre une référence indépendante,
  cohérence sous quatorze configurations d'étages, une régression par bug
  corrigé sauf B5.
- `make sanitize` — 6 passes ASan + UBSan sans trouvaille, 37 s, sur les deux
  variantes `SINK_TAIL`.
- Compilation `-Wall -Wextra` sans un seul avertissement, y compris sans
  `-fopenmp` et avec `-DRECOMPUTE_TURN=1`.

### Défauts connus

Recensés dans `BUG.md`, qui fait foi. Les six sont corrigés.

Hors défauts : B5 n'a pas de test de non-régression, aucun outil n'atteignant
proprement son chemin d'échec.

**B6 est postérieur à cette campagne** : le débordement de l'anneau de seaux
a été trouvé le 2026-08-28, après la mesure. Son correctif agrandit l'anneau,
donc le `memmove` d'un cran par fenêtre. Les chiffres ci-dessus n'ont pas été
rejoués ; un contrôle A/B entrelacé sur la configuration par défaut à 10¹¹
figure dans l'entrée B6 de `BUG.md`.
