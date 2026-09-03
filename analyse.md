# Analyse

Ce fichier porte la **méthode de mesure et la lecture des compteurs**, plus le
jeu de données de comparaison à primesieve sur cinq décades. Il est écrit pour
être lu isolément, sans le contexte de la session qui l'a produit.

Sa place parmi les autres :

| fichier | rôle | cycle de vie |
|---|---|---|
| `MESURES.md` | chiffres de la campagne courante | réécrit à chaque campagne |
| `HISTORIQUE.md` | évolution, décisions, pistes fermées | ajout seul |
| `BUG.md` | défauts, par bug | ajout par bug |
| `analyse.md` | méthode, lecture des compteurs, mise à l'échelle | mis à jour quand la méthode change |

Le jeu de données ci-dessous n'est pas une campagne : il décrit un **comportement
asymptotique** sur quatre ordres de grandeur, ce qui survit à un changement de
commit tant que la structure du crible ne bouge pas. Le revérifier coûte 12 h.

---

## Provenance

Tous les chiffres de ce fichier viennent de cette série. Aucun autre.

| | |
|---|---|
| **Commit** | `91ef930` — `main12.c` inchangé depuis `9484bbc` |
| **Date** | 2026-09-02 → 2026-09-03 |
| **CPU** | AMD Ryzen 7 9700X (Zen 5) — 8 cœurs / 16 threads, 5,58 GHz max |
| **Caches** | L1d 48 KiB par cœur · L2 1 MiB **partagée par 2 threads** · L3 32 MiB partagé par 16 |
| **Référence** | primesieve 12.15 |
| **Compilateur** | gcc 15.2.0, `make` par défaut |
| **Gouverneur** | `powersave`, non fixé |

π(n) vérifié identique entre les deux binaires aux cinq décades, jusqu'à
π(10¹⁵) = 29 844 570 422 669.

---

## Protocole

Trois règles, chacune apprise à ses dépens.

**Neutraliser debuginfod.** Sans cela `perf report` se bloque plusieurs minutes
sur des requêtes réseau.

```console
$ export DEBUGINFOD_URLS=
```

**Ne jamais lancer les deux binaires en parallèle.** Ils se disputent la L3 et
la bande passante ; les deux mesures sont alors fausses, et faussées dans des
proportions différentes. Toujours en série, machine au repos.

**Compter les compteurs.** Zen 5 offre 6 compteurs programmables. Au-delà, `perf`
multiplexe et l'indique en pourcentage à droite de chaque ligne : 6 événements
donnent 83 %, 8 en donnent 62,5 %. À 62,5 % l'erreur reste sous le pourcent,
négligeable devant les écarts mesurés ici, mais il faut la connaître.

Le jeu de huit utilisé pour toute la série :

```console
$ perf stat -e cycles,instructions,branches,branch-misses,\
L1-dcache-loads,L1-dcache-load-misses,cache-misses,\
ls_any_fills_from_sys.dram_io_all ./roue12 1e14
```

Aux décades courtes (10¹¹, 10¹²), prendre la médiane de trois répétitions : la
dispersion y est inférieure à 0,5 %. Au-delà, un run unique suffit.

---

## Lire les compteurs sur Zen

**C'est le piège le plus coûteux de cette plateforme.** Les événements génériques
de `perf` ne pointent pas les mêmes unités que sur Intel.

| événement générique | ce qu'il compte réellement sur Zen |
|---|---|
| `cache-references` | requêtes **L2**, préchargement matériel inclus |
| `cache-misses` | défauts **L2** — *pas* le dernier niveau |
| `L1-dcache-load-misses` | défauts L1d, cohérent avec `ls_any_fills_from_sys.all` |
| `LLC-loads`, `LLC-load-misses` | `<not supported>` |

Il n'existe **aucun événement générique pour le trafic DRAM**. Il faut le nommer :

```console
$ perf stat -e ls_any_fills_from_sys.dram_io_all ./roue12 1e13
```

Vérification qui établit le mapping, mesurée à 10¹¹ :

```
  cache-references                 14,21 G
  l2_request_g1.all_no_prefetch     8,05 G   -> l'ecart, 6,15 G, est le prefetch
  L1-dcache-loads                  37,37 G   -> cache-references n'est pas la L1
  L1-dcache-load-misses             8,03 G   -> egal aux requetes L2 hors prefetch
```

**Conséquence directe.** Lire `cache-misses` comme un compteur de dernier niveau
donne l'image d'un programme qui martèle la DRAM, et conduit à conclure que
roue12 est limité par la mémoire. Il ne l'est pas, à aucune des cinq décades :
son trafic DRAM culmine à **5,8 Go/s** à 10¹⁵, sur une machine qui en offre
plusieurs dizaines. Voir l'entrée du 2026-09-03 dans `HISTORIQUE.md`.

Multiplier un compte de défauts par 64 octets donne le trafic de l'étage
correspondant. Utile : à 10¹⁵ roue12 pousse 128 Go/s à travers la L3, contre
61 Go/s pour primesieve, sans que cela lui coûte de CPI.

---

## Le jeu de données

### Temps et avance

| n | roue12 | primesieve | avance |
|---|---|---|---|
| 10¹¹ | 0,721 s | 1,077 s | ×1,494 |
| 10¹² | 8,981 s | 12,762 s | ×1,421 |
| 10¹³ | 115,22 s | 155,15 s | ×1,347 |
| 10¹⁴ | 1 552,5 s | 1 900,9 s | ×1,224 |
| 10¹⁵ | 19 459,5 s | 22 772,3 s | ×1,170 |

roue12 est devant partout, mais **l'avance perd deux tiers de son excédent** sur
la série. L'érosion est monotone ; sa vitesse ne l'est pas (−4,9 %, −5,2 %,
−9,1 %, −4,4 % par décade).

### Compteurs — roue12

| | 10¹¹ | 10¹² | 10¹³ | 10¹⁴ | 10¹⁵ |
|---|---|---|---|---|---|
| cycles (10¹²) | 0,0485 | 0,600 | 7,587 | 100,4 | 1 268,2 |
| instructions (10¹²) | 0,0662 | 0,784 | 9,758 | 139,0 | 2 099,5 |
| **CPI** | 0,732 | 0,765 | 0,778 | 0,722 | **0,604** |
| charges L1d (10¹²) | 0,0374 | 0,445 | 5,071 | 62,24 | 792,3 |
| défauts L1d | 21,5 % | 23,9 % | 25,4 % | 24,0 % | 21,4 % |
| défauts L2 / 100 instr. | 0,56 | 1,36 | 2,61 | 2,63 | 1,85 |
| DRAM | 0,02 Go/s | 2,1 | 2,4 | 3,4 | 5,8 |
| threads utiles / 16 | — | 15,69 | — | 15,65 | 15,76 |
| segment | 1024 KiB | 2048 | 2048 | 2048 | 2048 |

### Compteurs — primesieve 12.15

| | 10¹¹ | 10¹² | 10¹³ | 10¹⁴ | 10¹⁵ |
|---|---|---|---|---|---|
| cycles (10¹²) | 0,0729 | 0,848 | 10,20 | 123,9 | 1 483,7 |
| instructions (10¹²) | 0,0718 | 0,874 | 12,07 | 171,7 | 2 215,9 |
| **CPI** | 1,017 | 0,970 | 0,845 | 0,7215 | 0,670 |
| charges L1d (10¹²) | 0,0308 | 0,352 | 4,530 | 62,46 | 791,9 |
| défauts L1d | 59,4 % | 59,6 % | 50,9 % | 39,9 % | 33,7 % |
| défauts L2 / 100 instr. | 0,32 | 1,11 | 1,23 | 0,95 | 0,98 |
| DRAM | 0,01 Go/s | 0,04 | 0,26 | 1,8 | 4,0 |
| threads utiles / 16 | — | 15,94 | — | 15,99 | 15,96 |
| segment | 512 KiB | 512 | 512 | 512 | 512 |

Deux faits à retenir de ces deux tables. À 10¹⁴ et 10¹⁵ les deux programmes
exécutent **le même nombre de chargements mémoire à 0,05 % près** — tout l'écart
de coût tient au taux de défauts sur des accès identiques en nombre. Et roue12
encaisse en permanence 1,6 à 2,2× plus de défauts L2 que primesieve tout en
gardant un meilleur CPI : ses défauts sont mieux recouverts, et un raisonnement
fondé sur les taux de défauts prédit un écart de coût qui n'existe pas.

---

## La décomposition quantité × coût

Le temps se factorise exactement, sans résidu :

```
  cycles = instructions × CPI
           ----------     ---
           quantite       cout unitaire
```

Appliquée au rapport des deux programmes, décade par décade :

```
                    1e11    1e12    1e13    1e14    1e15
  total (cycles)   1,505   1,412   1,345   1,235   1,170
  quantite         1,083   1,114   1,237   1,235   1,056
  cout unitaire    1,389   1,267   1,087   0,999   1,108
```

**L'outil est descriptif, pas prédictif.** Il factorise juste à chaque décade et
il rend lisible ce que le ratio global masque — ici, un régime dominé par le coût
à 10¹¹–10¹², par la quantité à 10¹³–10¹⁴, puis de nouveau par le coût à 10¹⁵.
Mais ses deux termes ne sont pas des propriétés stables du programme : ce sont
deux monnaies dans lesquelles un même levier physique s'encaisse alternativement
selon le régime arithmétique. Extrapoler l'un des deux termes a échoué deux fois
sur cette seule série ; voir `HISTORIQUE.md`, 2026-09-03.

### Mise à l'échelle propre à roue12

| transition | temps | = instructions | × facteur CPI | attendu *n·lnln n* |
|---|---|---|---|---|
| 10¹¹ → 10¹² | ×12,46 | ×11,84 | ×1,045 | ×10,27 |
| 10¹² → 10¹³ | ×12,83 | ×12,44 | ×1,017 | ×10,24 |
| 10¹³ → 10¹⁴ | ×13,48 | ×14,25 | ×0,928 | ×10,22 |
| 10¹⁴ → 10¹⁵ | ×12,53 | ×15,10 | ×0,836 | ×10,20 |

Le volume de travail **accélère de façon monotone** pendant que le temps reste
entre ×12,5 et ×13,5 : le coût unitaire absorbe l'écart. Le régime des seaux
gonfle le nombre d'instructions et les rend simultanément prédictibles et
séquentielles.

primesieve croît plus lentement à chaque palier — ×11,85, ×12,16, ×12,25,
×11,98 — et c'est là toute l'érosion du ratio, vue de l'intérieur. Les deux
paient une surcharge sur l'asymptote théorique : +21 à +32 % pour roue12, +15 à
+20 % pour primesieve, le supplément venant de la comptabilité des seaux qui
croît en √n.

---

## Le régime des seaux

C'est le mécanisme unique derrière tout ce qui précède. **Plus la borne monte,
plus l'étage des seaux pèse dans le budget d'instructions.**

Le nombre de radiations à effectuer croît comme *n·Σ1/p* sur les premiers
jusqu'à √n, soit ×10,23 à ×10,31 par décade — presque exactement le facteur
théorique. Tout ce qui est dépensé au-delà est donc un changement de **mélange**,
pas de volume :

| instructions par radiation | 10¹¹ | 10¹² | 10¹³ | 10¹⁴ | 10¹⁵ |
|---|---|---|---|---|---|
| roue12 | 0,237 | 0,272 | 0,329 | 0,457 | **0,675** |
| primesieve | 0,256 | 0,303 | 0,407 | 0,565 | 0,713 |

Le coût par radiation de roue12 **triple** sur la série, et sa croissance
s'accélère : ×1,15, ×1,21, ×1,39, ×1,48 par décade. Le travail élémentaire n'a
pas changé de nature ; c'est un étage plus cher qui prend la place.

**Le moteur arithmétique.** Les premiers cribleurs vont jusqu'à √n.

```
  n        racine(n)      pi(racine(n))
  1e11      3,16e5              27 300
  1e12      1,00e6              78 498
  1e13      3,16e6            ~227 600
  1e14      1,00e7             664 579
  1e15      3,16e7          ~1 946 000
```

Soixante-et-onze fois plus de premiers à placer sur la série, et les plus grands
sont cent fois plus grands — donc de plus en plus nombreux à ne pouvoir barrer
qu'une fois par segment, ce qui est le critère d'entrée dans les seaux.

**Le tarif.** Le profil du 2026-08-30 dans `HISTORIQUE.md`, mesuré à 10¹⁵ :
les seaux prennent **43,6 % des instructions pour 8,7 % du marquage**, soit
environ cinq fois le coût moyen par radiation, et 29,4 % des cycles. Une part de
marquage qui grandit lentement suffit donc à déplacer beaucoup d'instructions.

**Attention à la formulation.** Les seaux dominent le budget d'instructions, pas
le travail : à 10¹⁵ ils portent moins d'un neuvième des radiations. Dire que le
travail « est presque entièrement dans les seaux » est faux et conduit à
surestimer ce qu'un changement sur cet étage peut rapporter.

À l'autre bout, la même arithmétique éclaire le point de départ : à 10¹¹, √n vaut
3,16·10⁵, un ordre de grandeur sous le seuil qu'implique ce profil.
**L'étage des seaux y est quasi vide** — et c'est exactement la décade où l'avance
de roue12 est la plus microarchitecturale de la série, terme de coût à son
maximum (×1,389).

**Ce que ce seul mécanisme explique.** La convergence des CPI, puisque les seaux
sont le même algorithme des deux côtés et n'offrent aucune localité à paver.
L'accélération du volume d'instructions de roue12. L'amélioration continue du
taux de défauts L1 de primesieve. Et l'effondrement du terme de quantité à 10¹⁵,
quand le segment plafonne pendant que la population de seaux triple.

**Ce qui n'est pas mesuré.** La part des seaux décade par décade. Il faudrait des
compteurs par étage, non relevés pendant cette campagne ; seul le point à 10¹⁵
existe. La courbe intermédiaire est une inférence, pas une mesure — c'est le
premier relevé à faire si cette piste doit être travaillée.

---

## Pistes fermées

À ne pas rouvrir sans élément nouveau. Chacune a été mesurée, pas raisonnée.

**La taille de segment, dans les deux directions.** Le plafond par défaut n'est
pas un réglage à affiner : c'est un bord de capacité, et les deux versants sont
mesurés.

*Réduire* — perdant à 10¹². À 512 KiB, les défauts L2 chutent de 71 %, le trafic
DRAM d'un facteur 525, le CPI descend à 0,710 — le meilleur de toutes les
configurations essayées — et le programme est **10,7 % plus lent**, le volume
d'instructions montant de 18,8 %. Le débordement de la L2 est le prix délibéré de
l'amortissement par segment, pas un défaut.

```
  -s KiB       256     512    1024    2048    4096      (temps a 1e12)
  roue12     11,84    9,91    9,13    8,95    9,07
  primesieve 13,16   12,70   13,47   13,98   13,98
```

*Agrandir* — catastrophique à 10¹⁴, `-s 4096` contre le défaut de 2048 :

```
                    2048 KiB    4096 KiB
  temps              1 552 s     4 487 s     x2,89 plus lent
  instructions     139 009 T   110 921 T     -20,2 %
  CPI                  0,722       3,335     x4,62
  DRAM                  83 G     1 680 G     x20,2
  debit DRAM        3,4 Go/s   24,0 Go/s
```

L'amortissement fonctionne exactement comme prévu — **−20,2 % d'instructions** —
et le coût unitaire l'emporte d'un facteur 4,6. Cause : 16 threads × 4096 KiB =
64 Mio contre 32 Mio de L3. Le jeu de travail ne tient plus et le trafic DRAM,
resté sous 6 Go/s sur les cinq décades, bondit à 24 Go/s. Le plafond par défaut
place 16 × 2 Mio = **exactement la capacité L3** ; la règle de dimensionnement
automatique tombe juste des deux côtés.

**Un balayage de segment ne se transporte pas d'une borne à l'autre.** À 10¹²,
`-s 4096` ne coûte que +1,3 % ; à 10¹⁴ il coûte +189 %. Même réglage, même
machine, verdicts opposés — c'est ce qui avait justifié de rouvrir la piste, et
il faut refaire le balayage dans le régime où l'on veut conclure. L'explication
probable, non mesurée : à 10¹² les listes de seaux sont petites et l'empreinte
réelle reste sous la L3 malgré un bitset de 4096 KiB.

Note d'usage : le plafond L3 ne concerne que le dimensionnement automatique, une
valeur explicite est honorée telle quelle, vérifié jusqu'à `-s 8192`. Repère hors
régime de seaux, à 10¹¹ : 4096 KiB coûte +6 %, 8192 KiB coûte ×2,7.

Attention aussi au compteur de cycles sur ces configurations : à 4096 KiB les
cycles montent ×3,69 pour un temps ×2,89, parce que les cœurs bloqués sur la
mémoire consomment moins et montent en fréquence — 5,21 GHz contre 4,13 GHz. Le
compteur de cycles surestime le ralentissement dès qu'on compare deux points à
intensité mémoire très différente.

**Chasser les défauts L2.** roue12 en porte structurellement plus que primesieve,
avec un meilleur CPI. Ils sont recouverts par le désordonné. Porter les défauts
n'est pas être bloqué par eux — c'est le motif d'erreur récurrent du projet,
nommé cinq fois dans `HISTORIQUE.md`.

**La DRAM.** Jamais un facteur, à aucune décade, pour aucun des deux binaires.
5,8 Go/s au pire.

**Optimiser l'IPC comme objectif.** La configuration au meilleur IPC de toute
l'étude (1,409, à 512 KiB) est 10,7 % plus lente que celle retenue (1,307).
L'IPC mesure un coût, pas une performance ; il ne devient un objectif qu'à
quantité de travail constante, ce qu'un changement de segment ne fait jamais.

---

## Pistes ouvertes

**Une comptabilité de seaux moins chère.** C'est la piste la mieux chiffrée, et
la seule qui reste sur le criblage. Le test `-s 4096` à 10¹⁴ a mesuré la taille
du gain disponible : **−20,2 % d'instructions**, uniquement en amortissant la
comptabilité de seaux sur des segments deux fois plus longs. Le levier est donc
réel et mesuré. Il est simplement inatteignable par la taille de segment, bornée
par la capacité L3 sur cette machine — le récupérer demande de payer moins par
entrée de seau, pas d'en payer moins souvent.

C'est aussi ce qui expliquerait l'effondrement du terme de quantité à 10¹⁵ : le
segment plafonne pendant que la population de seaux triple. Inutile de relancer
`-s 4096` à 10¹⁵, la falaise de capacité y est la même et pire.

**Le parallélisme.** roue12 utilise 15,65 à 15,76 threads utiles sur 16, contre
15,96 à 15,99 pour primesieve — 1,5 à 2 % laissés sur la table, probablement en
déséquilibre de fin de balayage. Non instrumenté. C'est désormais du même ordre
que ce qui sépare les deux CPI à 10¹⁵.

---

## Coût des runs

À connaître avant de s'engager : les mesures doivent être séquentielles.

| n | roue12 | primesieve | total |
|---|---|---|---|
| 10¹¹ | 0,7 s | 1,1 s | 2 s |
| 10¹² | 9 s | 13 s | 22 s |
| 10¹³ | 1 min 55 | 2 min 35 | 4 min 30 |
| 10¹⁴ | 25 min 53 | 31 min 41 | 58 min |
| 10¹⁵ | 5 h 24 | 6 h 20 | **11 h 45** |
| 10¹⁶ | ~68 h | ~76 h | ~6 jours |

10¹⁶ est la borne du binaire et n'a pas été mesuré. Au-delà de 10¹³, lancer en
tâche de fond et ne pas sonder en boucle.

Mémoire : 10¹⁵ tient largement dans 60 Gio disponibles ; aucun échange observé.
