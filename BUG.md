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
| B6 | Débordement de l'anneau de seaux à l'activation | corrigé | `e29ec95` | `make check` |
| B7 | Débordement de `chunk_segments * segment_bits` sur `-c` | corrigé | non commité | `make check` |

`make check` a été validé par réinjection : chacun des cinq défauts testés
remis dans le code fait échouer la suite, et B1 en fait tomber cinq contrôles à
lui seul. B5 reste sans garde-fou, pour la raison exposée dans son entrée.

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

---

## B6 — Débordement de l'anneau de seaux à l'activation

**État** : corrigé · non commité · 2026-08-28

**Sévérité.** Écriture hors bornes du tas, donc corruption ; en pratique
`SIGSEGV` immédiat sur les configurations essayées.

**Symptôme.** `./roue12 1e11 -s 2048 -J 4 -v` se termine par
`Segmentation fault (core dumped)`, code retour 139, avant la moindre sortie.
Sous ASan :

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8
    #0 bucket_push main12.c:827
    #1 sieve_segment main12.c:1802
0x51100001a060 is located 32 bytes after 256-byte region
allocated by ... calloc ... main12.c:3302
```

soit `ring.cur`, dimensionné à 32 pointeurs, indexé au-delà.

**Cause.** `ring.cur` est indexé par le décalage en fenêtres depuis le début
du segment courant, qui est sa fente 0. `bucket_push` fait `&r->cur[d]` sans
borne, et les deux sites d'appel ne vérifient que l'appartenance au chunk :
`gw0 + skip < chunk_windows` à l'activation, `skip < left` au balayage. C'est
donc le dimensionnement de `ring_slots` qui doit majorer `skip`, et il ne
couvrait qu'une des deux portées :

```c
/* L'anneau doit couvrir le plus grand saut d'un premier entre deux
   marques : l'ecart maximal des residus de la roue 210 vaut 10, soit
   p / 3 octets, que 2p majore largement. */
uint64_t need = 2 * (uint64_t)primes[prime_count - 1] / bucket_bytes + 4;
```

Le raisonnement est juste, mais il ne couvre que la **réinsertion** du
balayage, où un premier ressort au plus loin d'un écart de résidus. Il ne dit
rien de l'**activation**, qui pose la première marque de `p` en `p * p` : pour
un premier dont le carré tombe dans le segment courant, ce point est
n'importe où dans le segment, sans rapport avec `p`. Le terme manquant est
donc `segment_bytes`, et le débordement s'ouvre dès que le segment dépasse
l'anneau — soit, l'arrondi à la puissance de deux près, dès qu'il approche
`2 p_max` octets.

L'autre entrée de l'activation, elle, était bien couverte : quand `p * p`
précède le segment, le cofacteur `ceil(low / p)` est arrondi au résidu suivant
de la roue 210, ce qui ajoute au plus 10 unités, soit `p / 3` octets — le même
majorant que le balayage.

Instrumenté au site d'appel, `1e11 -s 2048 -J 4` donne `p = 131671`,
`skip = 36` pour `slots = 32`, avec une fenêtre de 32 KiB et un segment de
2048 KiB — soit 64 fenêtres par segment, déjà le double de l'anneau.

**Correctif.** Le dimensionnement majore les deux portées :

```c
-        uint64_t need =
-            2 * (uint64_t)primes[prime_count - 1] / bucket_bytes + 4;
+        uint64_t need =
+            (segment_bytes + 2 * (uint64_t)primes[prime_count - 1])
+            / bucket_bytes + 4;
```

Le majorant `2p` d'origine est conservé — il couvre les deux termes en `p`,
le saut du balayage et l'arrondi du cofacteur — et `segment_bytes` ajoute la
portée qui manquait. Le plafonnement existant par `cap_windows` reste en
place : il est licite, les deux sites refusant déjà tout `skip` au-delà du
chunk.

Coût : quelques pointeurs de plus par thread, et autant de `memmove` d'un cran
par fenêtre. À `10¹¹` avec `-s 2048`, l'anneau passe de 32 à 128 fentes, soit
1 KiB par thread. Mesuré en A/B entrelacé, meilleur de sept passages,
refroidissement de 10 s entre chaque, sur i5-9300HF le 2026-08-28 :

| | Avant | Après |
|---|---|---|
| `1e11` (défaut, seaux inactifs) | 3,68 s | 3,60 s |
| `1e11 -s 2048` (anneau 32 → 128) | 6,60 s | 6,83 s |

Soit environ 3 % sur la configuration touchée, dans la bande de bruit de la
machine.

**Atteignabilité.** Trois conditions à réunir. Des premiers de la bande des
seaux doivent être *activés* dans un segment, donc une borne au-delà de
`p_bucket²`. Le segment doit être large devant `2 p_max`, ce qui demande un
`-s` explicite : le segment par défaut est dérivé du L3 par thread, et
`p_bucket` en découle, si bien que les deux grandeurs restent liées. Enfin les
intervalles étroits sont hors d'atteinte, `cap_windows` y ramenant l'anneau à
la largeur du chunk, que les deux sites refusent déjà de dépasser.

Vérifié sur la configuration par défaut : `./roue12 7e12`, première borne
au-delà de `p_bucket²` = 6,9·10¹², passe sans un débordement sous garde-fou
(245 277 688 804 premiers, 1173 s).

**Détection.** Essai manuel d'une configuration d'étages que `check.sh` ne
couvrait pas : les quatorze configurations de la section « cohérence » ne
tournent qu'à `10⁹` et sur un intervalle de `10⁵`, deux régimes où aucun
premier n'atteint les seaux.

**Vérification.** π(10¹¹) = 4 118 054 813, et les comptes d'intervalles donnés
par primesieve 12.10. Colonne « avant » relevée avec un garde-fou
`d >= r->slots` posé dans `bucket_push`, qui distingue le débordement effectif
du plantage observable.

| Configuration à 10¹¹ | Avant | Après |
|---|---|---|
| défaut | aucun débordement ✅ | ✅ |
| `-s 512` | aucun débordement ✅ | ✅ |
| `-s 2048` | aucun débordement ✅ | ✅ |
| `-J 4` | aucun débordement ✅ | ✅ |
| `-s 2048 -J 4` | `d=36 slots=32` → SIGSEGV ❌ | ✅ |
| `-s 2048 -J 16` | aucun débordement ✅ | ✅ |
| `-s 16384 -J 1` | `d=33 slots=32` → SIGSEGV ❌ | ✅ |
| `-s 65536 -J 1` | `d=1100 slots=32` → SIGSEGV ❌ | ✅ |
| `-s 2048 -K 32 -J 4` | `d=36 slots=32` → SIGSEGV ❌ | ✅ |

Le garde-fou a ensuite tourné sur 232 configurations — bornes `10⁹`, `10¹⁰`,
`10¹¹`, intervalles hauts jusqu'à `3·10¹⁴`, `-s` de 32 à 65536 KiB, `-K` de 32
à 65536 KiB, `-J` 1, 4, 16 — sans un débordement et avec le compte attendu
partout. `make sanitize` repasse sans trouvaille, et la commande d'origine
sous ASan donne le compte juste.

**Non-régression.** Trois contrôles ajoutés à `check.sh`, 0,35 s à eux trois —
la suite passe de 121 contrôles en 9,5 s à 124 en 9,8 s :

```sh
expect 21201526   17180000000 -d 5e8 -s 1024 -J 1
expect 22484495   4300000000 -d 5e8 -s 512 -J 1
expect 455052511  1e10 -s 1024 -J 1
```

Les trois plantent la version d'avant correctif.

---

## B7 — Débordement de `chunk_segments * segment_bits` sur `-c`

**État** : corrigé · non commité · 2026-08-29

**Sévérité.** Compte faux, silencieux, avec code de retour 0. C'est le seul
défaut recensé ici qui rende un résultat erroné sans le signaler.

**Symptôme.** À segment de 128 KiB, donc `segment_bits = 2²⁰` :

```
$ ./roue12 1e9 -s 128 -c 17592186044416
Found 30 primes up to 1000000000 using 8 threads, segment 128 KiB in 1.4 ms
$ echo $?
0
```

30 au lieu de 50 847 534. Avec `-c 17592186044417` on obtient 278 737, avec
`-c 18446744073709551615` de nouveau 30.

**Cause.** `-c` alimente `chunk_override` sans plafond, et `chunk_segments`
le reprend tel quel :

```c
if (chunk_override)
{
    chunk_segments = chunk_override;
}
...
uint64_t chunk_candidates = chunk_segments * segment_bits;
```

`2⁴⁴ × 2²⁰ = 2⁶⁴` : le produit est nul modulo 2⁶⁴, donc `chunk_candidates`
vaut 0, `chunk_end == chunk_first`, et la boucle des segments ne tourne pas.
Les 30 restants sont 2, 3, 5 et les 27 premiers du pré-crible, comptés hors
crible. Avec `2⁴⁴ + 1`, le produit retombe à `2²⁰` : un seul segment est
criblé, d'où 278 737.

Rien n'est écrit hors bornes et aucun sanitizer ne s'en émeut — c'est une
arithmétique modulaire parfaitement définie, qui produit simplement un domaine
plus petit que celui demandé. Le débordement s'ouvre dès
`-c ≥ 2⁶⁴ / segment_bits`, soit 2⁴⁴ au segment par défaut de ce test.

**Correctif.** Le nombre de segments par chunk n'a aucun sens au-delà du
nombre total de segments : le plafonner y suffit, et ne change rien aux
valeurs utiles.

```c
if (chunk_override)
{
    /* Plafonne a la plage entiere : au-dela, chunk_segments *
       segment_bits deborde et chunk_candidates retombe sous la
       taille reelle, ne criblant qu'une partie du domaine. */
    chunk_segments =
        chunk_override < total_segments ? chunk_override
                                        : total_segments;

    if (chunk_segments == 0)
        chunk_segments = 1;
}
```

Le second garde-fou couvre `total_segments == 0`, qui survient sur un
intervalle vide et donnerait une division par zéro au calcul de `chunk_count`.

**Vérification.** Les douze options numériques ont été passées à `0`, `1`,
`2³²`, `2⁴⁴` et `UINT64_MAX`, soit 60 combinaisons comparées à π(10⁹) : `-c`
était la seule touchée, et plus aucune ne diverge après correctif.
`make sanitize` repasse sans trouvaille, et la compilation reste sans
avertissement sous `-Wall -Wextra`, y compris sans `-fopenmp` et avec
`-DRECOMPUTE_TURN=1`.

**Non-régression.** Trois contrôles ajoutés à `check.sh` — la suite passe de
124 contrôles à 127, de 9,8 s à 11,2 s :

```sh
expect 50847534 1e9 -s 128 -c 17592186044416
expect 50847534 1e9 -s 128 -c 17592186044417
expect 50847534 1e9 -s 128 -c 18446744073709551615
```

Les trois donnent 30, 278 737 et 30 sur la version d'avant correctif.
