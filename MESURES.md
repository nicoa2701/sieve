# Mesures

Ce fichier est réécrit à chaque campagne : il décrit l'état mesuré le plus
récent, pas l'évolution dans le temps. L'évolution est dans `HISTORIQUE.md`.
Les défauts sont dans `BUG.md`.

**Règle** — aucun chiffre sans ses trois références : le commit mesuré (source
exacte), la date et l'heure, le CPU. Chaque section ci-dessous les porte, pour
rester lisible isolément.

---

## Campagne C3 — 2026-08-29

Remplace C2, que deux changements avaient périmée d'un coup : les deux paliers
du 2026-08-29 sur `generate_base_primes`, qui divisent le coût fixe par
dix-sept, et le passage de l'i5-9300HF au Ryzen 9700X. Aucun chiffre de C2 ne
décrivait plus le programme ni la machine.

C3 comble aussi le manque que C2 déclarait comme le sien : **le coût de la
plaque et des seaux, mesuré à une borne où ils travaillent.** L'ablation est
faite à trois bornes plutôt qu'une, choisies pour qu'à chacune un étage de
plus soit en service.

### Provenance

| | |
|---|---|
| **Commit** | `6e0ed6e30cf110e3ab2b12599f81a3f54384f802` |
| **Date** | 2026-08-29, 15:28:22 → 15:44:48 (UTC) — validation 15:45:33 → 15:52 |
| **CPU** | AMD Ryzen 7 9700X — 8 cœurs / 16 threads, 5,58 GHz max |

### Plateforme

| | |
|---|---|
| L1d | 48 KiB par cœur (384 KiB, 8 instances) |
| L2 | 1 MiB par cœur (8 MiB, 8 instances) |
| L3 | 32 MiB, partagé |
| SIMD | AVX-512 F/BW/CD/DQ/VL/VBMI/IFMA — le pré-crible passe par les **intrinsèques**, pas par le C vectorisable de l'i5 |
| Noyau | 7.0.0-30-generic |
| Compilateur | gcc 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| Build | `make` → `-O3 -g -Wall -Wextra -march=native -fopenmp -DSINK_TAIL=0` |
| Gouverneur | `powersave`, non fixé |
| Référence | primesieve 12.15 |

**Équité de la référence.** `primesieve` est comparé dans son build ordinaire,
sans `-march=native`, alors que `roue12` en profite. Le doute a été levé :
un build `-march=native` de primesieve donne 269 ms à `[10¹⁵, +10¹⁰]` contre
267 ms pour le build ordinaire, soit rien. La comparaison n'est pas biaisée.

### Méthode

Reprise inchangée de C2, à quoi s'ajoute un témoin de dérive.

**Refroidissement entre chaque passage : 10 × la durée de la mesure, borné à
[3 s, 30 s].** Meilleur temps retenu, sur 3 passages pour la comparaison et 5
pour l'ablation, `roue12` et `primesieve` entrelacés point par point.
`primesieve` est appelé avec `--no-status` : son affichage de progression
fausse la mesure. Chronomètre interne des deux programmes.

**Témoin de dérive.** Trois passages de `primesieve` sur `[10¹⁵, +10¹⁰]` en
fin de campagne : **275, 275, 269 ms**, contre 272 ms relevés au début. La
machine n'a pas bougé de plus de 2,2 % pendant les seize minutes. Ce témoin
est né d'une campagne jetée le jour même, où la même commande primesieve était
passée de 1,22 à 1,93 s sur une machine dérivant de 50 %.

**Bornes.** Le comptage complet s'arrête à 10¹² ; au-delà, seules des fenêtres
`-d 10¹⁰` sont visitées. Ce n'est pas une limite du programme mais du temps de
banc : π(10¹²) coûte déjà 22 s aux deux programmes réunis, par passage.

**Résolution.** Elle dépend de la borne, et il faut le savoir avant de lire
l'ablation :

| Borne | Contrôle | Bande |
|---|---|---|
| π(10¹⁰) | deux séries indépendantes, 61,2 et 61,2 ms | < 1 % |
| `[10¹¹, +10¹⁰]` | 85,2 puis 86,0 ms | ~1 % |
| `[10¹⁵, +10¹⁰]` | 333,3 (meilleur de 3), 328,8 (meilleur de 5), 351,1 (passage isolé) | ~5 % |

**À 10¹⁵, rien en dessous de 5 % n'est interprétable** — la borne haute est la
moins reproductible, et c'est précisément là que se joue la comparaison.

### Comparaison à primesieve 12.15, 16 threads

Commit `6e0ed6e` · 2026-08-29 15:28–15:38 · Ryzen 9700X

Comptage complet :

| Borne | roue12 | primesieve | rapport |
|---|---|---|---|
| π(10¹⁰) | **61,2 ms** | 88,0 ms | 1,44× |
| π(10¹¹) | **716,8 ms** | 1 071 ms | 1,49× |
| π(10¹²) | **8,92 s** | 12,69 s | 1,42× |

Intervalle de largeur 10¹⁰ :

| Début | roue12 | primesieve | rapport |
|---|---|---|---|
| 10¹¹ | **85,2 ms** | 118,0 ms | 1,38× |
| 10¹² | **108,2 ms** | 138,0 ms | 1,28× |
| 10¹³ | **145,8 ms** | 168,0 ms | 1,15× |
| 10¹⁴ | **208,7 ms** | 210,0 ms | 1,01× |
| 10¹⁵ | 333,3 ms | 272,0 ms | 0,82× |

**Le point de croisement est passé d'entre 10¹² et 10¹³ à entre 10¹⁴ et
10¹⁵.** En C2, le programme était distancé sur quatre points sur six et perdait
jusqu'à 1,5× ; ici il devance la référence partout sauf à 10¹⁵, où il reste
18 % en retard. À 10¹⁴ c'est l'égalité à la bande de mesure près.

Ce déplacement ne vient pas du criblage mais du coût fixe : la génération des
premiers de base est passée de 435 à 25,5 ms à 10¹⁵ (voir `HISTORIQUE.md`).
Le retard restant à 10¹⁵, lui, est bien dans le criblage.

### Empreinte mémoire

Commit `6e0ed6e` · 2026-08-29 15:44 · Ryzen 9700X — maxRSS, `/usr/bin/time`

| Intervalle | roue12 | primesieve | rapport |
|---|---|---|---|
| `[10¹³, +10¹⁰]` | 65 892 KiB | 45 896 KiB | 1,44× |
| `[10¹⁵, +10¹⁰]` | 290 024 KiB | 265 096 KiB | 1,09× |

Conclusion inchangée depuis C2, et maintenant vérifiée aux deux bouts :
l'empreinte n'explique pas le retard à 10¹⁵, où elle est à 9 % de la
référence. L'écart relatif est plus large à 10¹³, mais sur un volume où il ne
coûte rien. Le prix est par entrée de seau, pas en volume.

### Coût des étages

Commit `6e0ed6e` · 2026-08-29 15:38–15:44 · Ryzen 9700X

Chronomètre interne, meilleur de 5, chaque étage désactivé isolément.
**Les trois bornes donnent le même compte dans les neuf configurations** —
aucune ablation ne change le résultat, seulement le temps.

Le choix des trois bornes tient à un seul nombre : le seuil des seaux vaut
5 242 880. Un premier n'y entre que si √N le dépasse, soit N > 2,7·10¹³.
D'où π(10¹⁰), où la plaque **et** les seaux sont hors service ;
`[10¹³, +10¹⁰]` (√N = 3,16·10⁶), où la plaque travaille et pas les seaux ;
`[10¹⁵, +10¹⁰]` (√N = 3,16·10⁷), où tout tourne.

#### `[10¹⁵, +10¹⁰]` — tous les étages en service

Bande de reproductibilité ~5 % : les deux premières lignes ne sont pas résolues.

| Configuration | Temps | Écart |
|---|---|---|
| **défaut** | **328,8 ms** | — |
| `-Q 0` (préchargement neutralisé) | 336,1 ms | *non résolu* |
| `-b 0` (bloc L1 coupé) | 369,9 ms | +12 % |
| `-S 0` (plaque coupée) | 375,6 ms | +14 % |
| `-K 0` (seaux coupés) | 398,9 ms | +21 % |
| `-c 1` | 422,0 ms | +28 % |
| `-p 0` (pré-crible coupé) | 442,9 ms | +35 % |
| `-B 0` (tranche L2 coupée) | 538,1 ms | +64 % |
| `-s 32` | 598,9 ms | +82 % |

**C'est le trou de C2 qui se referme.** La plaque et les seaux, que C2 ne
pouvait que déclarer non mesurés, valent respectivement 14 % et 21 % du temps
à la borne où ils servent. Les deux étages gagnent leur place, et le chemin
des seaux est le plus rentable des cinq.

La tranche L2 et le dimensionnement du segment dominent tout le reste. `-c 1`
coûte 28 % ici contre 1 % à 10¹⁰ : le vol de travail entre threads ne se
paie qu'à cette borne, où les chunks sont chers et déséquilibrés.

#### `[10¹³, +10¹⁰]` — plaque en service, seaux hors service

Bande ~2 %.

| Configuration | Temps | Écart |
|---|---|---|
| **défaut** | **147,7 ms** | — |
| `-K 0` (seaux coupés) | 148,0 ms | *non résolu* |
| `-Q 0` (préchargement neutralisé) | 149,4 ms | *non résolu* |
| `-c 1` | 152,8 ms | +3,5 % |
| `-S 0` (plaque coupée) | 158,1 ms | +7,0 % |
| `-B 0` (tranche L2 coupée) | 166,3 ms | +13 % |
| `-p 0` (pré-crible coupé) | 171,0 ms | +16 % |
| `-b 0` (bloc L1 coupé) | 190,6 ms | +29 % |
| `-s 32` | 372,5 ms | +152 % |

`-K 0` non résolu est ici une **prédiction vérifiée**, pas une surprise :
√10¹³ = 3 162 278 est sous le seuil de 5 242 880, donc aucun premier ne passe
par les seaux et les couper ne peut rien coûter. La plaque, elle, prend
31 139 premiers à cette borne et se résout à 7 %.

#### π(10¹⁰) — plaque et seaux hors service (comparable à C2)

Bande < 1 %. Même mesure que C2, sur l'autre machine.

| Configuration | Temps | Écart | C2 (i5) |
|---|---|---|---|
| **défaut** | **61,2 ms** | — | 249,3 ms |
| `-Q 0` | 60,5 ms | *non résolu* | *non résolu* |
| `-S 0` (plaque, éteinte d'elle-même) | 61,1 ms | *non résolu* | *non résolu* |
| `-K 0` (seaux, aucun premier) | 61,4 ms | *non résolu* | *non résolu* |
| `-c 1` | 62,0 ms | +1,3 % | +4,2 % |
| `-B 0` (tranche L2 coupée) | 67,1 ms | +9,6 % | +3,7 % |
| `-p 0` (pré-crible coupé) | 85,4 ms | +40 % | +47 % |
| `-b 0` (bloc L1 coupé) | 106,4 ms | +74 % | +24 % |
| `-s 32` | 110,4 ms | +80 % | +59 % |

La hiérarchie est la même sur les deux machines, les poids non. **`-b 0` passe
de +24 % à +74 %** : le blocage L1 rapporte trois fois plus sur Zen 5, dont le
L1d fait 48 KiB par cœur contre 32 sur l'i5 — un bloc une fois et demie plus
grand, pour deux threads par cœur des deux côtés. Le pré-crible, lui, rapporte
un peu moins (+40 % contre
+47 %) alors même qu'il passe ici par les intrinsèques AVX-512 : le reste du
programme a plus accéléré que lui.

**Accélération sur 16 threads : 7,91×** pour 8 cœurs physiques, mesurée à
π(10¹⁰), `-t 1` (492,0 ms) contre le défaut (62,2 ms), meilleur de 3.
C2 relevait 3,8× pour 4 cœurs — à 10⁹, borne trop basse pour que le parallèle
soit strictement comparable.

#### Ce que l'ablation dit du préchargement

`-Q 0` n'est résolu à **aucune** des trois bornes, y compris celle où les cinq
étages travaillent. Ce n'est pas la preuve qu'il ne sert à rien — une
non-résolution n'est pas un zéro — mais après trois tentatives dont une dans
le régime le plus favorable, cette campagne n'établit pas son utilité. C'est le
seul étage dans ce cas. À reprendre par un A/B entrelacé dédié, seule méthode
capable de descendre sous la bande.

### Validation

Commit `6e0ed6e` · 2026-08-29 15:45:33 → 15:52:10 · Ryzen 9700X

Le protocole est figé dans `check.sh` et rejouable. **Il est joué en entier
sous les deux compilateurs**, comme la CI le fait à chaque `push` :

| | gcc 15.2.0 | clang 21.1.8 |
|---|---|---|
| `make check` | 127 contrôles, 0 échec, 1,1 s | 127 contrôles, 0 échec, 2,9 s |
| `make sanitize` | 6 passes, 0 trouvaille, 20 s | 6 passes, 0 trouvaille, 13 s |
| `-Werror`, avec OpenMP | 0 avertissement | 0 avertissement |
| `-Werror`, sans OpenMP | 0 avertissement | 0 avertissement |
| `-Werror`, `-DRECOMPUTE_TURN=1` | 0 avertissement | 0 avertissement |

`make check` couvre π(10ⁿ), les cas limites, les intervalles hauts, des
intervalles aléatoires contre une référence indépendante, la cohérence entre
configurations d'étages, et une régression par bug corrigé sauf B5.
`make sanitize` passe ASan + UBSan sur trois bornes et les deux variantes
`SINK_TAIL`.

Les trois variantes `-Werror` sont celles que ce fichier annonce sans
avertissement : le travail *Zéro avertissement* de la CI transforme cette
promesse écrite en test qui casse le jour où elle cesse d'être vraie.

### Défauts connus

Recensés dans `BUG.md`, qui fait foi. **Les sept sont corrigés, et cette
campagne leur est postérieure** — contrairement à C2, dont les chiffres
précédaient B6 et B7 et n'avaient pas été rejoués.

Hors défauts : B5 n'a toujours pas de test de non-régression, aucun outil
n'atteignant proprement son chemin d'échec.

### Ce que cette campagne ne mesure pas

- **Le comptage complet au-delà de 10¹²**, écarté pour le temps de banc. Les
  bornes hautes ne sont vues qu'en fenêtre de 10¹⁰.
- **Le profil interne à 10¹⁵**, qui dirait *où* passent les 18 % de retard
  restants. L'ablation dit ce que coûte couper un étage, pas ce que coûte le
  garder. C'est le manque principal de C3, et il porte, comme celui de C2, sur
  le seul régime où le programme est distancé.
- **L'utilité du préchargement**, non établie faute de résolution (voir
  ci-dessus).
