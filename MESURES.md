# Mesures

Ce fichier est réécrit à chaque campagne : il décrit l'état mesuré le plus
récent, pas l'évolution dans le temps. L'évolution est dans `HISTORIQUE.md`.
Les défauts sont dans `BUG.md`.

**Règle** — aucun chiffre sans ses trois références : le commit mesuré (source
exacte), la date et l'heure, le CPU. Chaque section ci-dessous les porte, pour
rester lisible isolément.

---

## Campagne C4 — 2026-08-30

Remplace C3, périmée par deux changements du criblage : `bdccb6f`, qui élargit
le découpage en chunks quand la repose des seaux le paie, et `9484bbc`, qui
fusionne le réempilement dans la boucle de vidage.

Deux ajouts au protocole, tous deux rendus nécessaires par la campagne
précédente :

- **un point sur fenêtre de 10¹¹**, seul endroit où la bande de
  reproductibilité descend à 1 %. C'est là que se juge désormais tout
  changement sur le criblage ;
- **une bande de reproductibilité mesurée en série indépendante** plutôt qu'un
  contrôle ponctuel — parce qu'un gain de 3,5 % annoncé pendant la session
  s'est révélé n'être qu'une dérive de la référence entre deux séries.

### Provenance

| | |
|---|---|
| **Commit** | `d51c054` — `main12.c` inchangé depuis `9484bbc` |
| **Date** | 2026-08-30, 04:24:19 → 04:44:37 (UTC) — validation 04:45:24 → 04:46:19 |
| **CPU** | AMD Ryzen 7 9700X — 8 cœurs / 16 threads, 5,58 GHz max |

### Plateforme

| | |
|---|---|
| L1d | 48 KiB par cœur (384 KiB, 8 instances) |
| L2 | 1 MiB par cœur (8 MiB, 8 instances) |
| L3 | 32 MiB, partagé |
| SIMD | AVX-512 F/BW/CD/DQ/VL/VBMI/IFMA — le pré-crible passe par les intrinsèques |
| Noyau | 7.0.0-30-generic |
| Compilateur | gcc 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| Build | `make` → `-O3 -g -Wall -Wextra -march=native -fopenmp -DSINK_TAIL=0` |
| Gouverneur | `powersave`, non fixé |
| Référence | primesieve 12.15 |

**Équité de la référence.** `primesieve` est comparé dans son build ordinaire,
sans `-march=native`, alors que `roue12` en profite. Le doute a été levé en
C3 : un build `-march=native` de primesieve donne 269 ms à `[10¹⁵, +10¹⁰]`
contre 267 ms pour le build ordinaire, soit rien.

### Méthode

**Refroidissement entre chaque passage : 10 × la durée de la mesure, borné à
[3 s, 30 s].** Meilleur temps retenu, sur 3 passages pour la comparaison et 5
pour l'ablation, `roue12` et `primesieve` entrelacés point par point.
`primesieve` est appelé avec `--no-status`. Chronomètre interne des deux
programmes.

**Témoin de dérive.** Trois passages de `primesieve` sur `[10¹⁵, +10¹⁰]` en fin
de campagne : **276, 269, 271 ms**, contre 271 ms relevés au début. La machine
n'a pas bougé de plus de 2,6 % pendant les vingt minutes.

**Résolution — et la distinction qui compte.** Elle est de deux sortes, et les
confondre a produit un faux gain pendant la session qui a précédé cette
campagne :

| | |
|---|---|
| **Dans une même série entrelacée** | π(10¹⁰) 61,3 puis 61,8 ms · `[10¹⁵,+10¹⁰]` 310,0 puis 314,1 · `[10¹⁵,+10¹¹]` 2234 puis 2228 → **0,3 % à 1,3 %** |
| **Entre séries indépendantes** | le même binaire sur `[10¹⁵,+10¹⁰]` a donné 303,6 · 304,2 · 308,5 · 314,6 · 331,7 · 335,9 ms au fil de la session → **10,6 %** |

**Un binaire ne se compare donc qu'à une référence mesurée dans la même série
entrelacée**, jamais à un chiffre relevé plus tôt. Sur la fenêtre de 10¹¹
l'écart entre séries tombe à 0,3 %, ce qui en fait le point de décision pour
tout changement inférieur à 5 %.

**Bornes.** Le comptage complet s'arrête à 10¹² : π(10¹²) coûte déjà 22 s aux
deux programmes réunis, par passage. Au-delà, seules des fenêtres sont visitées.

### Comparaison à primesieve 12.15, 16 threads

Commit `d51c054` · 2026-08-30 04:24–04:38 · Ryzen 9700X

Comptage complet :

| Borne | roue12 | primesieve | rapport | C3 |
|---|---|---|---|---|
| π(10¹⁰) | **61,3 ms** | 89,0 ms | 1,45× | 1,44× |
| π(10¹¹) | **714,4 ms** | 1 071 ms | 1,50× | 1,49× |
| π(10¹²) | **8,94 s** | 12,67 s | 1,42× | 1,42× |

Intervalle de largeur 10¹⁰ :

| Début | roue12 | primesieve | rapport | C3 |
|---|---|---|---|---|
| 10¹¹ | **86,7 ms** | 118,0 ms | 1,36× | 1,38× |
| 10¹² | **108,8 ms** | 138,0 ms | 1,27× | 1,28× |
| 10¹³ | **148,5 ms** | 165,0 ms | 1,11× | 1,15× |
| 10¹⁴ | 215,5 ms | 213,0 ms | 0,99× | 1,01× |
| 10¹⁵ | 310,0 ms | 271,0 ms | **0,87×** | 0,82× |

Intervalle de largeur 10¹¹ :

| Début | roue12 | primesieve | rapport |
|---|---|---|---|
| 10¹⁵ | **2 234 ms** | 2 369 ms | **1,06×** |

**Le retard à 10¹⁵ est un effet de fenêtre étroite, pas de vitesse de
criblage.** C'est le résultat le plus important de cette campagne, et C3 ne
pouvait pas le voir faute d'avoir mesuré une fenêtre large à cette borne : sur
10¹¹ de large, à la même borne, le programme **devance** la référence de 6 %.
Sur 10¹⁰ il la suit de 13 %. Ce qui reste à payer est donc le coût amorti par
fenêtre — génération des amorces, tables de tours, repose des seaux à chaque
chunk — et non le marquage lui-même.

Sur la fenêtre de 10¹⁰, le retard tombe tout de même de 18 % à 13 % depuis C3,
et le point de croisement reste entre 10¹⁴ et 10¹⁵.

### Empreinte mémoire

Commit `d51c054` · 2026-08-30 04:43 · Ryzen 9700X — maxRSS, `/usr/bin/time`

| Intervalle | roue12 | primesieve | rapport | C3 |
|---|---|---|---|---|
| `[10¹³, +10¹⁰]` | 65 532 KiB | 46 232 KiB | 1,42× | 1,44× |
| `[10¹⁵, +10¹⁰]` | 308 016 KiB | 264 844 KiB | 1,16× | 1,09× |

**L'empreinte à 10¹⁵ monte de 290 à 308 Mo, et c'est le prix de `bdccb6f`** :
des chunks de 4 segments au lieu de 2 doublent le nombre de fenêtres par chunk,
donc l'anneau de seaux passe de 64 à 128 emplacements. 18 Mo pour 6,6 % de
temps, sur un programme dont l'empreinte reste à 1,16× celle de la référence.

### Coût des étages

Commit `d51c054` · 2026-08-30 04:38–04:43 · Ryzen 9700X

Chronomètre interne, meilleur de 5, chaque étage désactivé isolément. **Les
trois bornes donnent le même compte dans les neuf configurations.**

Le choix des trois bornes tient au seuil des seaux, 5 242 880 : un premier n'y
entre que si √N le dépasse, soit N > 2,7·10¹³.

#### `[10¹⁵, +10¹⁰]` — tous les étages en service, 1 587 772 premiers à seau

| Configuration | Temps | Écart | C3 |
|---|---|---|---|
| **défaut** | **301,1 ms** | — | — |
| `-S 0` (plaque coupée) | 316,2 ms | +5,0 % | +14 % |
| `-Q 0` (préchargement neutralisé) | 316,3 ms | **+5,0 %** | *non résolu* |
| `-b 0` (bloc L1 coupé) | 341,8 ms | +13 % | +12 % |
| `-K 0` (seaux coupés) | 411,4 ms | **+37 %** | +21 % |
| `-c 1` | 420,5 ms | **+40 %** | +28 % |
| `-p 0` (pré-crible coupé) | 436,7 ms | +45 % | +35 % |
| `-B 0` (tranche L2 coupée) | 524,9 ms | +74 % | +64 % |
| `-s 32` | 576,8 ms | +92 % | +82 % |

**`-Q` se résout enfin.** C3 le donnait non résolu à ses trois bornes et n'en
tirait aucune conclusion ; il vaut ici 5,0 %, largement au-dessus de la bande
intra-série de 1,3 %. Un essai mené entre les deux campagnes le confirme par un
autre chemin : retirer le préchargement à la compilation coûte 5,1 % et fait
réapparaître 203 M de défauts L2, les remplissages logiciels tombant de 209 M à
78 000. Le chemin des seaux ne rate pas le cache **parce que** le préchargement
le sert.

**`-c 1` monte de 28 % à 40 %** parce que le défaut vaut désormais 4 segments
par chunk et non 2 : `-c 1` dégrade d'un facteur 4 au lieu de 2. Même chose pour
`-K 0`, les seaux ayant plus à faire par chunk.

#### `[10¹³, +10¹⁰]` — plaque en service, seaux hors service

| Configuration | Temps | Écart |
|---|---|---|
| **défaut** | **148,7 ms** | — |
| `-Q 0` | 149,4 ms | *non résolu* |
| `-K 0` (seaux coupés) | 150,2 ms | *non résolu* |
| `-c 1` | 152,4 ms | +2,5 % |
| `-S 0` (plaque coupée) | 157,1 ms | +5,6 % |
| `-B 0` (tranche L2 coupée) | 165,2 ms | +11 % |
| `-p 0` (pré-crible coupé) | 171,8 ms | +16 % |
| `-b 0` (bloc L1 coupé) | 188,5 ms | +27 % |
| `-s 32` | 362,0 ms | +143 % |

`-K 0` non résolu est une prédiction vérifiée : √10¹³ = 3 162 278 est sous le
seuil de 5 242 880, `-v` annonce **0 premier à seau**, et couper ce qui ne
tourne pas ne peut rien coûter. `-Q 0` suit, faute de seaux à précharger.

#### π(10¹⁰) — plaque et seaux hors service

| Configuration | Temps | Écart | C3 |
|---|---|---|---|
| **défaut** | **61,7 ms** | — | — |
| `-S 0`, `-K 0`, `-Q 0`, `-c 1` | 61,0 à 61,5 ms | *non résolus* | idem |
| `-B 0` (tranche L2 coupée) | 67,3 ms | +9,1 % | +9,6 % |
| `-p 0` (pré-crible coupé) | 86,6 ms | +40 % | +40 % |
| `-b 0` (bloc L1 coupé) | 106,3 ms | +72 % | +74 % |
| `-s 32` | 109,7 ms | +78 % | +80 % |

À cette borne rien n'a bougé depuis C3, ce qui est attendu : les deux
changements de la session ne touchent que le régime des seaux.

**Accélération sur 16 threads : 8,03×** pour 8 cœurs physiques, mesurée à
π(10¹⁰), `-t 1` (493,9 ms) contre le défaut (61,5 ms).

### Validation

Commit `d51c054` · 2026-08-30 04:45:24 → 04:46:19 · Ryzen 9700X

Le protocole est figé dans `check.sh` et joué en entier sous les deux
compilateurs, comme la CI le fait à chaque `push` :

| | gcc 15.2.0 | clang 21.1.8 |
|---|---|---|
| `make check` | 127 contrôles, 0 échec, 4 s | 127 contrôles, 0 échec, 3 s |
| `make sanitize` | 6 passes, 0 trouvaille, 20 s | 6 passes, 0 trouvaille, 13 s |
| `-Werror`, avec OpenMP | 0 avertissement | 0 avertissement |
| `-Werror`, sans OpenMP | 0 avertissement | 0 avertissement |
| `-Werror`, `-DRECOMPUTE_TURN=1` | 0 avertissement | 0 avertissement |

`make check` couvre π(10ⁿ), les cas limites, les intervalles hauts, des
intervalles aléatoires contre une référence indépendante, la cohérence entre
configurations d'étages, et une régression par bug corrigé sauf B5.

### Défauts connus

Recensés dans `BUG.md`, qui fait foi. **Les sept sont corrigés, et cette
campagne leur est postérieure.**

Hors défauts : B5 n'a toujours pas de test de non-régression, aucun outil
n'atteignant proprement son chemin d'échec.

### Ce que cette campagne ne mesure pas

- **Le comptage complet au-delà de 10¹²**, écarté pour le temps de banc.
- **Les fenêtres larges ailleurs qu'à 10¹⁵.** Le point `[10¹⁵, +10¹¹]` renverse
  la lecture du retard ; la même mesure à 10¹³ et 10¹⁴ dirait si le croisement
  se déplace aussi sur cet axe. C'est le manque principal de C4.
- **Ce qui reste des 13 % à 10¹⁵ sur fenêtre étroite.** Cinq pistes ont été
  fermées entre C3 et C4 — réciproque entière, marche de roue empaquetée,
  préchargement retiré, entrée de seau empaquetée, grandes pages et
  préchargement de la bande du milieu. Elles sont dans `HISTORIQUE.md` avec
  leurs mécanismes ; aucune n'a laissé de cible dont on puisse dire qu'elle est
  exposée plutôt que recouverte.
