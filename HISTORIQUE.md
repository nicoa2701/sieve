# Historique

Fichier en ajout seul : les entrées s'empilent, aucune n'est réécrite. Les
mesures ne sont pas ici — elles sont dans `MESURES.md`, qui est réécrit à
chaque campagne. Une entrée peut renvoyer à une campagne par son nom (`C1`…).

Les défauts ont leur propre recensement dans `BUG.md`, organisé par bug et non
par date : symptôme, cause, correctif, vérification.

---

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
