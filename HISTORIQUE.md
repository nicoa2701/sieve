# Historique

Fichier en ajout seul : les entrées s'empilent, aucune n'est réécrite. Les
mesures ne sont pas ici — elles sont dans `MESURES.md`, qui est réécrit à
chaque campagne. Une entrée peut renvoyer à une campagne par son nom (`C1`…).

Les défauts ont leur propre recensement dans `BUG.md`, organisé par bug et non
par date : symptôme, cause, correctif, vérification.

---

## 2026-08-29 — C3 · Campagne de mesure sur le 9700X

`MESURES.md` décrivait encore C2 : l'i5-9300HF, le commit `491cd40`,
primesieve 12.10. Deux changements l'avaient périmée d'un coup — les deux
paliers du jour sur `generate_base_primes`, et le passage sur Ryzen 9700X.
Plus un seul chiffre du fichier ne décrivait le programme ni la machine.

**Le point de croisement recule de deux décades.** Sur des fenêtres de 10¹⁰ :

```
  debut      roue12   primesieve   rapport    C2 (i5)
  10^12     108,2 ms    138,0 ms    1,28x      1,10x
  10^13     145,8 ms    168,0 ms    1,15x      0,90x
  10^14     208,7 ms    210,0 ms    1,01x      0,82x
  10^15     333,3 ms    272,0 ms    0,82x      0,66x
```

En C2 le programme était distancé sur quatre points sur six et perdait jusqu'à
1,5× ; ici sur un seul, et de 18 %. Comptage complet : 1,42× à 1,49× d'avance
de 10¹⁰ à 10¹². Ce déplacement vient du coût fixe, pas du criblage — le retard
restant à 10¹⁵, lui, est bien dans le criblage.

**Le manque déclaré de C2 est comblé.** C2 se reprochait de n'avoir pu mesurer
ni la plaque ni les seaux, faute d'une borne où ils travaillent. L'ablation est
donc faite à trois bornes plutôt qu'une, le choix découlant d'un seul nombre —
le seuil des seaux, 5 242 880, qu'un premier ne franchit que si √N le dépasse :
π(10¹⁰) où plaque et seaux sont hors service, `[10¹³, +10¹⁰]` où la plaque seule
tourne, `[10¹⁵, +10¹⁰]` où tout tourne. À cette dernière, couper la plaque coûte
14 % et couper les seaux 21 % : les deux étages gagnent leur place, et le chemin
des seaux est le plus rentable des cinq.

Deux choses qu'une borne unique aurait cachées :

- **`-c 1` coûte 28 % à 10¹⁵ contre 1,3 % à 10¹⁰.** Le vol de travail entre
  threads ne se paie qu'en haut, où les chunks sont chers et déséquilibrés. C2,
  qui ne mesurait qu'à 10¹⁰, le classait comme négligeable.
- **`-Q 0` n'est résolu à aucune des trois bornes**, y compris celle où les cinq
  étages tournent. Une non-résolution n'est pas un zéro, mais après trois
  tentatives dont une dans le régime le plus favorable, la campagne n'établit
  pas l'utilité du préchargement. C'est le seul étage dans ce cas.

Le témoin de dérive adopté après la campagne jetée a servi : 275, 275, 269 ms
en fin de campagne contre 272 ms au début, soit 2,2 % sur seize minutes. La
bande de reproductibilité est écrite avant les tables parce qu'elle décide de
leur lecture : moins de 1 % à 10¹⁰, mais **~5 % à 10¹⁵**, c'est-à-dire là même
où se joue la comparaison. Deux lignes d'ablation sont marquées non résolues à
ce titre. Vérifié aussi que la référence n'est pas désavantagée : primesieve
compilé `-march=native` donne 269 ms contre 267 ms sans, alors que `roue12` en
profite.

La validation est désormais jouée en entier sous les deux compilateurs, et non
plus seulement déléguée à la CI : `make check` (127 contrôles), `make sanitize`
et les trois variantes `-Werror` passent sous gcc 15.2 comme sous clang 21.1.

Reste à faire : le profil à 10¹⁵. L'ablation dit ce que coûte *couper* un
étage, pas où passent les 18 % de retard. C'est le manque de C3, et il porte,
comme celui de C2, sur le seul régime où le programme est distancé.

---

## 2026-08-29 — `ea7124b` · Amorces criblées par la roue 30 du programme

Second palier sur `generate_base_primes`, après les impairs segmentés du même
jour. La fonction n'a plus de crible à elle : elle emprunte celui du programme
— `sweep_exact_calc` pour le marquage, `activate_prime` pour poser chaque
amorce, `index_to_number` pour relire. Un octet porte 30 entiers au lieu de 2,
et l'extraction lit le bitset par mots de 64 bits, un `ctz` par premier trouvé.
Elle a dû être déplacée après `sweep_exact_calc`, dont elle dépend désormais.

C'est le principe de `SievingPrimes` dans primesieve : le crible se sert
lui-même, au lieu d'entretenir une routine séparée et moins bonne.

Générateur mesuré isolément sur i5 : 45 → 10,8 ms à racine(10¹⁵),
142 → 35,3 ms à racine(10¹⁶). Sortie identique à une référence indépendante,
bornes 0 à 5000 exhaustivement et sous les deux variantes `SINK_TAIL`.

Mesures d'ensemble sur Ryzen 7 9700X, 16 threads, machine dédiée, meilleur de
3, refroidissement proportionnel — la campagne porte un témoin de dérive, trois
passages de primesieve à la fin, ici stables à 1,8 % près :

```
                 cout fixe (-d 1)      intervalle 1e10, vs primesieve 12.15
  10^13        7,6 -> 5,3 ms          1,11x -> 1,13x
  10^14       18,4 -> 11,0 ms         0,94x -> 0,97x
  10^15       48,2 -> 25,5 ms         0,77x -> 0,81x
```

Cumulé avec le palier précédent, le rapport à 10¹⁵ passe de 0,66× à 0,81× : le
retard tombe de 34 % à 19 %, et 10¹⁴ revient à l'égalité.

**Cette piste est épuisée.** Il reste 25,5 ms de coût fixe sur 333 ms à 10¹⁵,
et ce résidu n'est plus la génération des amorces mais le pré-crible, les
tables de tours et les allocations. Les 19 % de retard restants sont dans le
criblage lui-même.

Une première campagne de mesure, menée sur le portable en fin de session, avait
été jetée : `primesieve` y passait de 1,22 à 1,93 s sur une commande inchangée,
la machine ayant dérivé de 50 %. D'où le témoin de dérive, désormais systématique.

---

## 2026-08-29 — `90314e1` · Premiers de base, impairs seuls et par segments

`generate_base_primes` tenait un octet par entier et criblait d'un bloc. À
racine(10¹⁵) il parcourait 31 Mo en accès dispersés, à racine(10¹⁶) 100 Mo, en
série. Invisible sur un comptage complet ; dominant sur les intervalles étroits
à borne haute.

Le crible ne porte plus que sur les impairs, par blocs de 32 KiB : la fenêtre
active tient en cache. Les amorces jusqu'à racine(racine(limit)) sortent d'un
crible simple, négligeable à cette taille.

Le coût fixe, mesuré à `-d 1`, tombe de 435 à 76 ms à 10¹⁵. Le générateur seul
passe de 373 à 45 ms à racine(10¹⁵). Sortie identique à l'ancienne
implémentation, vérifiée exhaustivement de 0 à 3000 et sur six bornes jusqu'à
10⁸. Chiffres en A/B entrelacé dans le message de commit.

**Deux pistes fausses avant celle-là**, toutes deux issues d'une lecture du
code et démenties par `perf` :

- le `memmove` de rotation de l'anneau de seaux — **0,00 %** du profil, aucun
  échantillon ;
- le chemin des seaux lui-même — `sweep_bucketed` tombe de 22 % à 16 % du temps
  CPU en passant de l'i5 au Zen 5, il profite donc bien du matériel.

La bonne piste n'est pas venue d'une intuition sur le source mais d'une ligne
du profil qu'il fallait convertir de temps CPU en temps mural :
`generate_base_primes` pesait 3,7 % du CPU, mais sur huit threads dont sept
inactifs pendant cette phase, cela faisait près de 30 % du temps mural.

Reste à faire : la roue 30 bit-packée avec extraction `popcnt`/`ctz`, comme
`SievingPrimes` dans primesieve, qui n'alloue aucune liste et sert les amorces
au fil de l'eau. Encore un facteur 3 à 4 sur cette phase.

---

## 2026-08-29 — `0268245` · Dépôt public, vitrine bilingue, intégration continue

Le code est passé sur GitHub. `README.md` devient la vitrine du projet et
`LISEZMOI.md` en donne la version française, les deux liées par un sélecteur de
langue : démarrage rapide, principes de fonctionnement, jeu complet des
options, validation, renvois vers les trois fichiers de suivi.

Intégration continue sur `push` et sur chaque PR, trois travaux en matrice
gcc × clang : compilation et `make check`, `make sanitize`, et `-Werror` sur
les trois variantes que la documentation annonce sans avertissement. Ce dernier
transforme une promesse écrite en test qui casse le jour où elle cesse d'être
vraie.

Avant publication, l'historique a été réécrit deux fois : purge d'un fichier
d'échange `nano` et d'une adresse personnelle héritée du commit initial de
GitHub, puis retrait de mentions d'outillage dans deux messages de commit.
La branche `main` est désormais protégée contre le `push --force`, ce qui rend
ces réécritures délibérées plutôt qu'accessibles d'une ligne de commande.

---

## 2026-08-29 — B7 · Débordement de `chunk_segments * segment_bits` sur `-c`

`-c` alimentait `chunk_segments` sans plafond. À 2⁴⁴ segments par chunk avec un
bitset de 128 KiB, le produit vaut exactement 2⁶⁴ : `chunk_candidates` tombe à
zéro, aucun segment n'est criblé, et le programme annonce 30 premiers jusqu'à
10⁹ avec un code de retour 0.

Premier défaut du recensement à rendre un compte faux **silencieusement** — les
six autres plantaient ou refusaient. Rien n'est écrit hors bornes et aucun
sanitizer ne s'en émeut : c'est une arithmétique modulaire parfaitement
définie, qui produit simplement un domaine plus petit que celui demandé.

Trouvé en passant les douze options numériques à `0`, `1`, `2³²`, `2⁴⁴` et
`UINT64_MAX`, soit soixante combinaisons comparées à π(10⁹) : `-c` était la
seule touchée. Le détail est dans `BUG.md`, entrée B7. Trois contrôles ajoutés
à `check.sh`, la suite passe de 124 à 127.

Au même moment, le texte de `-K` cessait de renvoyer à `roue11`, programme
absent de ce dépôt : il décrit désormais ce que fait le programme quand l'étage
est éteint.

---

## 2026-08-28 — B6 · Débordement de l'anneau de seaux à l'activation

`./roue12 1e11 -s 2048 -J 4 -v` plantait par `SIGSEGV`. ASan pointe une
lecture hors bornes dans `bucket_push`, sur `ring.cur` : l'anneau était
dimensionné pour la seule portée du balayage — l'écart entre deux marques d'un
même premier, `p / 3` octets, que `2p` majore — alors que l'activation pose la
première marque en `p * p`, donc n'importe où dans le segment. Le terme
`segment_bytes` manquait : `need = (segment_bytes + 2 p) / fenetre + 4`.

Le détail est dans `BUG.md`, entrée B6. Trois contrôles ajoutés à `check.sh`,
choisis courts : 0,35 s à eux trois, la suite passe de 121 contrôles en 9,5 s
à 124 en 9,8 s.
Un garde-fou temporaire `d >= r->slots` posé dans `bucket_push` a servi à
séparer le débordement effectif du plantage observable, et à balayer 232
configurations. Une première rédaction du correctif majorait l'arrondi du
cofacteur par un tour de roue entier ; l'écart maximal des résidus vaut 10, ce
que `2p` couvrait déjà.

Le défaut était hors d'atteinte de `check.sh` : ses quatorze configurations
d'étages ne tournent qu'à `10⁹` et sur un intervalle de `10⁵`, deux régimes où
aucun premier n'atteint les seaux. Les nouveaux contrôles ferment ce trou.

## 2026-08-28 — Makefile : témoin des drapeaux, `ARCH` sous sanitizer, `SINK`

Trois corrections, aucune sur le code :

- `sanitize` ne passait pas `$(ARCH)`. Sur une machine AVX-512, les deux
  binaires ASan compilaient donc la voie C portable, et le bloc
  `_mm512_*` — le seul code à charger et ranger à la main — n'était jamais
  sanitizé.
- `main12.o` ne dépendait que de `main12.c` : changer `CFLAGS`, `OPENMP` ou
  `SINK_TAIL` laissait l'objet périmé en place, sans le dire. Un témoin
  `.build-flags` enregistre la ligne de drapeaux et force la recompilation
  quand elle change, y compris passée en ligne de commande. Une dépendance sur
  `Makefile` seule ne suffisait pas : `make SINK=1` répondait « up to date ».
- `SINK_TAIL` était figé à 0 dans la règle. `SINK ?= 0` rend la variante
  mesurable en `-O3`, alors qu'elle n'existait qu'en `-O1` sous sanitizer.

## 2026-08-23 — Campagne C2, refroidissement entre passages

`MESURES.md` réécrit : C2 remplace C1. Seul changement de méthode, un
refroidissement de 10 × la durée de la mesure borné à [3 s, 30 s] entre
chaque passage. Il déplace les chiffres de 20 % — C1 mesurait une machine
froide au premier point de chaque série, et sous-estimait les deux programmes,
roue12 plus que primesieve.

Les conclusions de C1 tiennent : croisement entre 10¹² et 10¹³, retard de
×1,5 à 10¹⁵, empreinte mémoire comparable.

L'ablation des étages passe de 10⁹ à 10¹⁰, où la bande de reproductibilité
tombe de ±8 % à ±1,5 %, et du `/usr/bin/time` au chronomètre interne, dont la
résolution est de 0,1 ms au lieu de 10 ms. Une lecture intermédiaire de C2
faisait apparaître la plaque et les seaux comme coûteux à 10⁹ ; c'était du
bruit. À ces bornes la plaque s'éteint d'elle-même et aucun premier n'atteint
les seaux : couper l'un ou l'autre est une non-opération.

## 2026-08-23 — `check.sh` · Cibles `make check` et `make sanitize`

Le protocole de validation, jusque-là une suite de commandes tapées à la main,
est figé dans `check.sh` : 121 contrôles en une dizaine de secondes — π(10ⁿ),
cas limites, intervalles hauts, 60 intervalles aléatoires contre une référence
indépendante, cohérence sous quatorze configurations d'étages, et une
régression par bug corrigé sauf B5.

`make sanitize` construit et exerce les deux variantes `SINK_TAIL` sous ASan
et UBSan, avec `-fno-sanitize-recover=all` pour qu'une trouvaille fasse
échouer la cible.

La suite a été validée par réinjection : B1, B2, B3 et B4 remis dans le code
la font échouer à chaque fois.

## 2026-08-23 — `1a4180e` · Accès atomiques aux drapeaux d'allocation

B5 corrigé : les six accès à `alloc_failed` et `bucket_alloc_failed` situés
dans la région `omp parallel` passent par `atomic read` ou `atomic write`.
Sans coût mesurable — le seul ajout au chemin chaud est une lecture atomique
par chunk. Vérifié par inspection et non-régression, pas par ThreadSanitizer :
voir la limite exposée dans `BUG.md`.

## 2026-08-23 — `BUG.md`

Recensement des cinq défauts trouvés depuis l'ouverture du suivi : B1 SIGFPE
sur `HAUT = 0`, B2 `malloc(0)`, B3 messages de borne, B4 débordement de
`bucket_entry_t.at`, B5 courses de données sur les drapeaux d'allocation.
Quatre corrigés, B5 ouvert avec son correctif proposé.

## 2026-08-23 — `bdce01b` · Débordement du champ `at` des seaux

`bucket_entry_t.at` range le décalage dans la fenêtre à partir du bit 9, les
9 bits bas portant l'indice de marche `rc * 48 + j` (384 valeurs). Le décalage
doit donc tenir sur 23 bits, soit une fenêtre de 8 MiB au plus. Rien ne le
garantissait : `bucket_bytes` n'était plafonné qu'au segment, lui-même
plafonné à `MAX_SEGMENT_KB` = 64 MiB.

```c
+    if (bucket_bytes > BUCKET_WINDOW_MAX_BYTES)
+        bucket_bytes = BUCKET_WINDOW_MAX_BYTES;
```

Le plafond rejoint celui du segment, juste avant la réduction en puissance de
deux, et porte un nom qui dit d'où vient la borne. Coût nul : une comparaison
dans `main`, hors de toute boucle.

Défaut trouvé en dérivant l'invariant pour le commenter, non par un test.
Il n'était pas atteignable par défaut — la fenêtre est dérivée de la tranche
L2, très loin de 8 MiB — mais un `-K` explicite suffisait à le déclencher.

Intervalle `[9999999000000000, +10⁹]`, dont primesieve 12.10 donne 27 147 369.
Avant, commit `4c643b7` · 2026-08-23 12:48 · i5-9300HF ; après, commit
`bdce01b` · 2026-08-23 12:51:49 · i5-9300HF :

| Réglage | Avant | Après |
|---|---|---|
| `-s 65536 -K 4096 -J 1` | 27 147 369 ✅ | — |
| `-s 65536 -K 8192 -J 1` | 27 147 369 ✅ | 27 147 369 ✅ |
| `-s 65536 -K 16384 -J 1` | 27 423 941 ❌ | 27 147 369 ✅ |
| `-s 65536 -K 32768 -J 1` | 27 396 895 ❌ | — |
| `-s 65536 -K 65536 -J 1` | 27 309 075 ❌ | 27 147 369 ✅ |

Le seuil tombait exactement sur 2²³ octets, comme l'empaquetage le prédit.

## 2026-08-23 — `4c643b7` · Commentaires minimaux

Vingt-cinq blocs dans `main12.c`, aux seuls endroits où le code ne peut pas se
dire lui-même : les dérivations (masques de roue, décalages de tour, marche de
la roue 210, décalages recalculés, amortissement du segment) et les invariants
de sûreté (copie de queue du pré-crible, empaquetage de `at`, blocs de seau
auto-localisés, marge exigée par `sweep_over`, octet de garde de
`mark_partial`). Section `.text` compilée sans `-g` identique à celle de
`9d2b576`.

## 2026-08-23 — `b750dfa` · Fichier d'échange retiré du suivi

Un `git add -A` avait ramassé un `.MESURES.md.swp`. Retiré du suivi, motifs
d'échange vim ajoutés au `.gitignore`. Le blob reste dans l'historique de
`73c6c1c`.

## 2026-08-23 — `9d2b576` · En-tête minimal

En-tête de `main12.c` listant les cinq points clés : roue mod 30 et conversion
index/entier, les cinq étages de balayage et leur critère de choix, le
principe des seaux, le pré-crible fusionné, le dimensionnement déduit des
caches. Aucun changement de code.

## 2026-08-23 — `2124de7` · Commit initial, et trois correctifs

Mise sous git du crible (`main12.c`, 3 513 lignes, aucun commentaire, aucun
historique préalable) et de son `Makefile`. Ajout d'un `.gitignore` limité aux
deux cibles de `make clean` (`roue12`, `main12.o`).

Le commit contient déjà trois correctifs par rapport à l'état trouvé :

**1. SIGFPE sur `HAUT = 0`** — `main12.c`, calcul de la racine.

```c
-    while (root > limit / root)
+    while (root && root > limit / root)
```

`sqrt(0.0)` donne `root = 0`, et la boucle qui rattrape l'arrondi flottant
divisait alors par zéro : `./roue12 0` et `./roue12 0 0` faisaient un core
dump. Cette boucle ne corrige qu'une *sur*estimation de la racine, ce que
`root = 0` ne peut pas être ; la garder hors du chemin préserve exactement son
intention. Aucun changement de comportement pour `HAUT ≥ 1`.

**2. `malloc(0)` traité comme un échec** — `generate_base_primes`.

```c
-        malloc(n * sizeof(uint32_t));
+        malloc((n ? n : 1) * sizeof(uint32_t));
```

Quand `root < 2`, `n` vaut 0 ; une libc qui renvoie `NULL` pour `malloc(0)` —
comportement conforme — déclenchait « allocation failed » sur un cas
parfaitement valide. Reprend l'idiome `(x ? x : 1)` déjà employé pour `sorted`,
`turn` et `cursor`. Vérifié par `LD_PRELOAD` d'un `malloc` forçant `NULL` en
taille nulle : avant, rc=1 et message d'erreur ; après, rc=0. C'est le seul
site exposé, les six autres allocations ayant un minimum structurel.

**3. Messages de borne trompeurs** — deux sites.

Le défaut n'était pas la formulation mais le `%.2e` : il arrondissait la valeur
refusée *et* le plafond à la même chaîne `1.00e+16`, si bien que l'erreur
affirmait que la valeur refusée était la valeur autorisée. Passage en `%llu`
exact ; le site `-d` nomme désormais ses deux opérandes plutôt que leur somme,
qui peut déborder (`dist` monte à ~1,8·10¹⁹ via `parse_bound`).

```
Interval end 10000000000000000 + 10000000000 exceeds the maximum 10000000000000000
Limit 20000000000000000 exceeds the maximum 10000000000000000
```

---

## État à l'ouverture du suivi

`main12.c` existait hors de tout dépôt, sans historique récupérable. Le point
de départ du suivi est donc `2124de7`, qui contient déjà les trois correctifs
ci-dessus : l'état strictement antérieur n'a jamais été commité.
