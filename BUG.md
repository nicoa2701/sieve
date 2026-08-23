# Bugs

Tous les défauts trouvés dans `main12.c`, corrigés ou non. Chaque entrée dit
le symptôme, la cause, le correctif, et comment il a été vérifié.

Toute reproduction chiffrée porte ses trois références : commit, date et
heure, CPU. Le récit chronologique est dans `HISTORIQUE.md`, les campagnes de
mesure dans `MESURES.md`.

| | Défaut | État | Correctif | Non-régression |
|---|---|---|---|---|
| B1 | SIGFPE sur `HAUT = 0` | corrigé | `2124de7` | `make check` |
| B2 | `malloc(0)` traité comme un échec | corrigé | `2124de7` | `make check`, par interposition |
| B3 | Messages de borne trompeurs | corrigé | `2124de7` | `make check` |
| B4 | Débordement de `bucket_entry_t.at` | corrigé | `bdce01b` | `make check` |
| B5 | Courses de données sur les drapeaux d'allocation | corrigé | `1a4180e` | **aucune** |

`make check` a été validé par réinjection : chacun des quatre défauts remis
dans le code fait échouer la suite, et B1 en fait tomber cinq contrôles à lui
seul. B5 reste sans garde-fou, pour la raison exposée dans son entrée.

---

## B1 — SIGFPE sur `HAUT = 0`

**État** : corrigé · commit `2124de7` · 2026-08-23 12:25:54

**Symptôme.** `./roue12 0` et `./roue12 0 0` terminaient par
`Floating point exception (core dumped)`, code retour 136.

**Cause.** `parse_bound` accepte `0` par sa voie entière — seule la voie
flottante rejette `d <= 0.0`. Avec `limit = 0`, `sqrt(0.0)` donne `root = 0`,
et la seconde boucle d'ajustement de la racine divise par cette valeur :

```c
root = (uint64_t)sqrt((double)limit);
while ((root + 1) <= limit / (root + 1))  /* root + 1 >= 1 : sans danger */
    root++;
while (root > limit / root)               /* root == 0 : division par zero */
    root--;
```

**Correctif.**

```c
-    while (root > limit / root)
+    while (root && root > limit / root)
```

Cette boucle ne rattrape qu'une *sur*estimation de la racine par le calcul
flottant. `root = 0` ne peut pas en être une, donc la sortir du chemin
préserve exactement son intention. Aucun changement de comportement pour
`HAUT >= 1`.

Le reste du programme traverse `limit = 0` sans autre incident : tous les
étages s'éteignent d'eux-mêmes, `total_candidates` vaut 0, et aucun segment
n'est parcouru.

**Détection.** Balayage systématique des cas limites, pas un test existant.

**Vérification.** `./roue12 0`, `0 0`, `0 -d 0` → `Found 0 primes up to 0`,
code retour 0. `-v` montre tous les étages éteints et `Chunks: 0`.

---

## B2 — `malloc(0)` traité comme un échec

**État** : corrigé · commit `2124de7` · 2026-08-23 12:25:54

**Symptôme.** Sur une libc où `malloc(0)` renvoie `NULL` — comportement
conforme au standard — `./roue12 0` sortait `allocation failed` avec le code
retour 1, sur une entrée parfaitement valide. Invisible sous glibc, qui
renvoie un pointeur non nul.

**Cause.** Dans `generate_base_primes`, `root < 2` donne `n == 0`, et le
`NULL` légitime est interprété comme un échec d'allocation.

**Correctif.**

```c
-        malloc(n * sizeof(uint32_t));
+        malloc((n ? n : 1) * sizeof(uint32_t));
```

Reprend l'idiome `(x ? x : 1)` déjà employé dans le fichier pour `sorted`,
`turn` et `cursor`.

**Portée.** C'est le seul site exposé. Les six autres allocations ont un
minimum structurel : `calloc(limit + 1, 1)`, `span >= 128`, 1 MiB constant,
`cap >= 8`, `segment_bytes >= 32 KiB`, `ring_slots >= 1`.

**Détection.** Revue des allocations, à partir de la trajectoire `limit = 0`
ouverte par B1.

**Vérification.** `LD_PRELOAD` d'un `malloc` interposé renvoyant `NULL` en
taille nulle : avant, `allocation failed` et code retour 1 ; après, code
retour 0, et `1e9` reste correct sous le même preload.

---

## B3 — Messages de borne trompeurs

**État** : corrigé · commit `2124de7` · 2026-08-23 12:25:54

**Symptôme.** `./roue12 1e16 -d 1e10` répondait
`Maximum limit is 1.00e+16`, alors que `./roue12 1e16` seul est accepté. Le
message affirmait donc que la valeur refusée était la valeur autorisée.

**Cause.** Pas la formulation, mais le `%.2e` : il arrondissait la demande
*et* le plafond à la même chaîne. `MAX_LIMIT` vaut exactement 10¹⁶ et le test
est `limit > MAX_LIMIT` ; c'est la somme `DEBUT + DIST` qui dépassait, ce que
l'affichage effaçait.

**Correctif.** Passage en `%llu` exact aux deux sites, et le site `-d` nomme
ses deux opérandes :

```
Interval end 10000000000000000 + 10000000000 exceeds the maximum 10000000000000000
Limit 20000000000000000 exceeds the maximum 10000000000000000
```

La somme n'est pas affichée : `dist` peut monter à ~1,8·10¹⁹ via
`parse_bound`, donc `low_limit + dist` déborderait dans le message d'erreur
lui-même.

**Détection.** Test des bornes hautes.

**Vérification.** Les deux messages ci-dessus, et les bornes acceptées restent
inchangées : `1e16 -d 0` et `[10¹⁶−1, 10¹⁶]` passent toujours.

---

## B4 — Débordement de `bucket_entry_t.at`

**État** : corrigé · commit `bdce01b` · 2026-08-23 12:51:39

**Sévérité.** Le seul des cinq à produire un **résultat faux** sans rien
signaler.

**Symptôme.** Compte de premiers erroné dès que la fenêtre de seau demandée
par `-K` dépasse 8 MiB.

**Cause.** `bucket_entry_t.at` empaquette deux valeurs dans un `uint32_t` :

```c
at = (decalage_dans_la_fenetre << 9) | (rc * 48 + j)
```

Les 9 bits bas portent l'indice de marche de la roue 210, qui va jusqu'à
`7 * 48 + 47 = 383 < 512`. Il reste 23 bits pour le décalage, qui doit donc
rester sous 2²³ = 8 MiB. Rien ne le garantissait : `bucket_bytes` n'était
plafonné qu'au segment, lui-même plafonné à `MAX_SEGMENT_KB` = 64 MiB.

**Correctif.** Une constante nommée d'après son origine, posée sous
`BUCKET_BLOCK_BYTES` :

```c
/* Plafond de la fenetre de seau : bucket_entry_t.at range le decalage a
   partir du bit 9, donc il doit tenir sur les 23 bits restants. */
#define BUCKET_WINDOW_MAX_BYTES (1ULL << 23)
```

et le plafonnement dans `main`, à côté de celui du segment :

```c
     if (bucket_bytes > segment_bytes)
         bucket_bytes = segment_bytes;

+    if (bucket_bytes > BUCKET_WINDOW_MAX_BYTES)
+        bucket_bytes = BUCKET_WINDOW_MAX_BYTES;
```

Placé **avant** la réduction en puissance de deux, de sorte que la fenêtre
reste un diviseur du segment — invariant que le reste du code suppose. Coût
nul : une comparaison dans `main`, hors de toute boucle.

**Atteignabilité.** Jamais par défaut : la fenêtre est dérivée de la tranche
L2, très loin de 8 MiB. Il fallait réunir un `-K` explicite au-delà de 8 MiB,
un segment large, et une borne assez haute pour que des premiers atteignent
la bande des seaux.

**Détection.** Trouvé en dérivant l'invariant d'empaquetage pour le
commenter, pas par un test — les 60 intervalles de validation tournaient tous
en configuration par défaut, hors d'atteinte du défaut.

**Vérification.** Intervalle `[9999999000000000, +10⁹]`, dont primesieve 12.10
donne 27 147 369. Avant : commit `4c643b7` · 2026-08-23 12:48 · i5-9300HF.
Après : commit `bdce01b` · 2026-08-23 12:51:49 · i5-9300HF.

| Réglage | Avant | Après |
|---|---|---|
| défaut | 27 147 369 ✅ | 27 147 369 ✅ |
| `-s 65536 -K 4096 -J 1` | 27 147 369 ✅ | — |
| `-s 65536 -K 8192 -J 1` | 27 147 369 ✅ | 27 147 369 ✅ |
| `-s 65536 -K 16384 -J 1` | 27 423 941 ❌ | 27 147 369 ✅ |
| `-s 65536 -K 32768 -J 1` | 27 396 895 ❌ | — |
| `-s 65536 -K 65536 -J 1` | 27 309 075 ❌ | 27 147 369 ✅ |

Le seuil tombait exactement sur 2²³ octets, comme l'empaquetage le prédit.
`-v` confirme le rabattement : `fenetre 8192 KiB` quand on demande 65536.

---

## B5 — Courses de données sur les drapeaux d'allocation

**État** : corrigé · commit `1a4180e` · 2026-08-23

**Symptôme.** Aucun observé. Formellement, comportement indéfini au sens du
modèle mémoire C.

**Cause.** Deux drapeaux partagés dans la région `omp parallel` :

- `bucket_alloc_failed` était écrit sans `atomic` par plusieurs threads, à
  l'échec d'allocation de `ring.cur` et à l'échec d'un seau.
- `alloc_failed` était écrit sous `#pragma omp atomic write`, mais relu sans
  `atomic` au début de chaque tour de la boucle sur les chunks.

**Pourquoi c'était bénin en pratique.** Les deux sont des `int`, tous les
écrivains écrivent la même valeur `1`, et une lecture manquée ne fait que
retarder l'abandon d'un tour de boucle. Sur les architectures visées, un
`int` aligné ne se déchire pas.

**Pourquoi le corriger quand même.** Rien dans le standard ne garantit ce qui
précède, et un compilateur reste libre de garder `alloc_failed` en registre à
travers la boucle, la rendant insensible à l'écriture d'un autre thread.

**Correctif.** Les six accès situés dans la région parallèle passent tous par
`atomic read` ou `atomic write`. Aux deux sites d'écriture :

```c
     if (!ring.cur)
+    {
+#ifdef _OPENMP
+#       pragma omp atomic write
+#endif
         bucket_alloc_failed = 1;
+    }
```

et à la lecture, en tête de la boucle sur les chunks :

```c
-    if (alloc_failed)
-        continue;
+    int stop;
+
+#ifdef _OPENMP
+#   pragma omp atomic read
+#endif
+    stop = alloc_failed;
+
+    if (stop)
+        continue;
```

Les deux lectures qui suivent la région parallèle restent des lectures
simples : la barrière implicite de fin de région les ordonne déjà.

**Coût.** L'unique ajout au chemin chaud est une lecture atomique par chunk,
soit quelques dizaines par exécution. Mesuré à 10⁹, meilleur de neuf, commit
`1a4180e` · 2026-08-23 12:59 · i5-9300HF : 23,4 ms après contre 23,6 ms avant.
Les écritures atomiques ne sont atteintes qu'en cas d'échec d'allocation.

**Limite de la vérification.** Le correctif est vérifié par inspection — les
six accès sont atomiques, `grep` à l'appui — par compilation avec et sans
`-fopenmp`, et par la non-régression fonctionnelle complète. **Il ne l'est pas
par un détecteur de courses.** ThreadSanitizer ne signale rien ni avant ni
après, parce que les écritures concurrentes n'ont lieu qu'à un échec
d'allocation : en marche normale, le chemin n'est jamais pris. Forcer cet
échec demande d'interposer `calloc`, ce qui casse soit libgomp, soit
ThreadSanitizer lui-même, incompatible avec `LD_PRELOAD`. La course reste donc
établie par lecture du code, pas par observation.
