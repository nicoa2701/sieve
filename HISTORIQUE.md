# Historique

Fichier en ajout seul : les entrées s'empilent, aucune n'est réécrite. Les
mesures ne sont pas ici — elles sont dans `MESURES.md`, qui est réécrit à
chaque campagne. Une entrée peut renvoyer à une campagne par son nom (`C1`…).

---

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
