# Historique

Fichier en ajout seul : les entrées s'empilent, aucune n'est réécrite. Les
mesures ne sont pas ici — elles sont dans `MESURES.md`, qui est réécrit à
chaque campagne. Une entrée peut renvoyer à une campagne par son nom (`C1`…).

Les défauts ont leur propre recensement dans `BUG.md`, organisé par bug et non
par date : symptôme, cause, correctif, vérification.

---

## 2026-08-30 — Bande du milieu · trois leviers fermés, et le motif d'erreur

L'entrée du profil des seaux désignait un second chantier : `sweep_exact_calc`
porte 82,8 % des défauts L3, avec un segment de 2048 KiB par thread contre
1 MiB de L2 partagé entre deux threads SMT. Trois leviers essayés, tous mesurés,
aucun retenu.

```
                        effet sur la cause              effet sur le temps
  -L / -S / -s          ---                    -L perdant de +4 a +44 %,
                                               -S plat, -s optimal au defaut
  grandes pages         TLB L1 702 M -> 11,9 M          -0,2 %
                        (facteur 59)
  prechargement         defauts L3 335 M -> 311 M       +3,5 %
  de la bande           (-7 %), +3,3 % d'instructions
```

Temps mesurés à `[10¹⁵, +10¹¹]`, meilleur de 5 entrelacé, où la bande de
reproductibilité vaut ~1 %.

**Ce que la bande fait réellement.** Compteurs exacts, en monothread pour éviter
les atomiques : 44,9 M de visites de premier, dont **6,77 M de tours complets
seulement**. La queue partielle porte 48,9 M des 103,7 M de marques, la boucle
déroulée par 8 en porte 54,2 M, la tête 0,5 M. Soit **une marque par visite en
moyenne** : avec un pas de `p` octets — au moins 512 Ko dans cette bande — la
plupart des premiers n'ont pas la place d'un tour complet dans un segment de
2 MiB. C'est ce qui a décidé du placement du préchargement, sur l'adresse exacte
de la première marque du premier suivant, et non dans la boucle déroulée où
j'allais le mettre.

**Pourquoi les trois échouent, et c'est la même raison.** Les 2,93·10⁸ défauts
L3 attribués à cette fonction coûteraient 9,2·10⁸ cycles par thread s'ils
étaient exposés ; elle n'en consomme que 2,4·10⁸. **Ils sont donc déjà
recouverts à un parallélisme d'environ 4**, que le déroulage par 8 fournit sans
rien demander. Il n'y avait pas de latence à récupérer — seulement du débit que
la machine absorbe déjà. Supprimer 59 fois les défauts de TLB ou 7 % des défauts
L3 ne pouvait rien donner ; y ajouter de l'arithmétique d'adresse coûte
exactement ce qu'elle pèse.

**Le motif d'erreur, nommé pour la troisième fois.** J'ai désigné cette fonction
comme « le chantier de la pression mémoire » parce qu'elle porte 82,8 % des
défauts L3. Porter les défauts n'est pas être bloqué par eux. C'est la même
faute que les deux précédentes, sous une forme de plus :

- « ce n'est pas l'arithmétique, donc c'est la mémoire » — corrigé par le profil
  des seaux ;
- « ce chemin ne rate pas le cache, donc le préchargement ne sert à rien » —
  corrigé en le retirant, ce qui a coûté 5 % ;
- « cette fonction porte les défauts, donc elle est bloquée dessus » — corrigé
  ici.

À chaque fois une cause déduite d'un compteur au lieu d'être mesurée. Les deux
seuls gains de la série, `bdccb6f` et `9484bbc`, ne reposaient pas sur une telle
déduction : l'un compte des répétitions de travail, l'autre des instructions,
tous deux déterministes.

**Note de méthode.** Un premier balayage de la distance de préchargement donnait
`BAND_PF=32` à −3,5 % sur la fenêtre de 10¹⁰. Refait contre une base fraîche, le
même binaire donne +0,7 % : la base avait dérivé de 314,6 à 304,2 ms entre les
deux séries. Le gain annoncé n'était qu'une dérive machine, et il aurait été
publié sans le contrôle. Sur cette fenêtre, un binaire ne se compare qu'à une
référence mesurée dans la même série entrelacée — jamais à un chiffre relevé
plus tôt.

**Ce que ça laisse.** Cinq résultats négatifs consécutifs après deux gains. Les
12 % de retard restants à 10¹⁵ ne cèdent à aucun levier essayé, et le profil ne
désigne plus de cible dont on puisse dire qu'elle est exposée plutôt que
recouverte. La suite demanderait de changer d'algorithme, pas de réglage.

---

## 2026-08-30 — Empaquetage de `bucket_entry_t` · piste nommée, tentée, fermée

L'entrée ci-dessous désignait cette piste comme la suivante, et la marquait
« non tentée ». Elle l'est désormais, sous deux formes, et les deux sont
perdantes.

```
                                        instructions      temps
  actuel  struct { uint32_t k; at; }     28,120e9        300,7 ms
  V6      mot unique, k masque           29,215e9 +3,9 %  316,0 ms +5,1 %
  V7      mot unique, k reconstruit      28,712e9 +2,1 %  313,9 ms +4,4 %
```

Le compte d'instructions est déterministe, le temps va dans le même sens :
inutile d'aller chercher la bande de reproductibilité, la piste est morte.

**Le mécanisme.** La forme actuelle est déjà meilleure que celle que je
proposais. gcc tient l'entrée entière dans un registre XMM et n'en réécrit que
la moitié `at` par un seul `vpinsrd`, suivi d'un `vmovq` pour ranger les huit
octets. Le mot unique l'oblige à assembler à la main — `shl`, `or`, `or` — et
en V6 s'y ajoutent un `movabs` pour matérialiser `0xffffffff00000000`, qui
n'est pas encodable en immédiat, et un **respill de `cur`** : garder le mot
vivant pendant la boucle de marquage coûte un registre de trop, et le gain du
commit précédent est repayé sur place. Le bloc de réempilement remonte de 12 à
18 instructions.

**L'erreur était dans la formulation de la piste elle-même.** Elle disait :
« `idx` sur 23 bits, `wi` sur 9, `k` sur 32 tiennent exactement dans 64 bits ».
C'est un argument de *place* — or la structure occupait déjà exactement 64
bits. Il n'y avait aucune place à gagner. L'empaquetage ne changeait que la
manière d'accéder aux champs, et il remplaçait une insertion de voie 32 bits
par de l'arithmétique 64 bits. La contrainte invoquée pour justifier la piste
était déjà satisfaite avant de commencer.

**Le motif se répète.** Troisième tentative de suite où « moins d'instructions
apparentes dans le C » produit plus d'instructions machine : la marche de roue
en un mot, le préchargement retiré, et maintenant l'entrée empaquetée. À chaque
fois gcc avait déjà choisi la forme économique — quatre `movzbl` servis à
trois par cycle, un préchargement qui convertit 209 M de défauts, une
insertion de voie — et la réécriture manuelle lui retirait cette liberté. Le
seul gain retenu sur ce chemin, `9484bbc`, ne va pas contre le compilateur : il
lui rend un registre.

---

## 2026-08-30 — `9484bbc` · Réempilement fusionné, et trois variantes écartées

Suite du profil ci-dessous, qui donnait la cible : le chemin des seaux est
limité par le débit d'instructions. Le désassemblage a dit où était le gras du
réempilement — deux relectures de `r` puis `r->cur` depuis la pile à chaque
entrée, un spill du slot, un `jmp` de retour. La base de l'anneau est
maintenant tenue en registre pour toute la fenêtre et le push est écrit sur
place ; le bloc passe de **17 à 12 instructions**.

```
  instructions, [10^15, +10^10]   28,805e9 -> 28,127e9   -2,4 %
  temps,        [10^15, +10^11]     2291 ms -> 2226 ms   -2,8 %
```

**Le point de mesure est la fenêtre de 10¹¹, et c'est le fond de l'affaire.**
Sa bande de reproductibilité vaut ~1 % contre ~5 % sur la fenêtre de 10¹⁰, où
le gain n'est pas résolu : trois passages y ont donné +0,9 %, −2,5 % et −8,4 %.
Sans le point long, cette mesure aurait pu être lue comme n'importe quoi entre
« rien » et « −8 % ». Le compte d'instructions, lui, est déterministe, et c'est
lui qui a servi à trier les variantes avant de chronométrer.

**Trois variantes écartées.** Elles valent l'entrée : chacune démolit une
intuition que le profil semblait appuyer.

- **Marche de roue empaquetée dans un mot de 64 bits.** Le corps de boucle
  faisait quatre `movzbl` sur une structure de 8 octets ; un seul chargement
  paraissait évident. Résultat : **+0,6 % d'instructions**. Un chargement plus
  cinq extractions ALU en chaîne dépendante coûte plus que quatre accès L1, que
  Zen 5 sert à trois ou quatre par cycle. Les quatre `movzbl` étaient le bon
  choix du compilateur.

- **Préchargement retiré.** L'entrée précédente concluait qu'« il n'y a rien à
  précharger, ce chemin ne rate pas le cache ». **C'est une lecture à
  l'envers**, et l'expérience le montre : −5,2 % d'instructions, mais +5,1 % de
  temps et 203 M de défauts L2 qui réapparaissent. Les 209 M de remplissages
  logiciels tombent à 78 000 quand on l'enlève. Le chemin ne rate pas le cache
  *parce que* le préchargement le sert. Ce qui ne se résout pas, c'est la
  distance — `-Q` de 8 à 32 — jamais le préchargement lui-même.

- **Base de l'anneau passée en paramètre** à `bucket_push`, pour ne pas
  dupliquer son corps : −1,5 % d'instructions, −0,7 % de temps, non résolu. La
  duplication paye le tiers restant, et c'est le seul motif de l'avoir écrite ;
  le commentaire du code le dit, sans quoi la première relecture la
  refactorisera.

`bucket_push` reste la forme employée à l'activation, où elle n'est appelée
qu'une fois par premier et par chunk.

**Ce que ça laisse.** Le budget par entrée reste d'environ 45 instructions, dont
le préchargement (5, nécessaire), le dépaquetage de l'entrée (9), le marquage
(11) et le réempilement (12). La piste suivante serait l'empaquetage de
`bucket_entry_t` — `idx` sur 23 bits, `wi` sur 9, `k` sur 32 tiennent
exactement dans 64 bits — pour supprimer la reconstruction par `vpinsrd` à
chaque réempilement. Non tentée.

---

## 2026-08-30 — Profil des seaux · 43,6 % des instructions pour 8,7 % du marquage

Après `bdccb6f` il restait 12 % de retard à 10¹⁵, et le profil par ligne les
plaçait dans `sweep_bucketed` (18,8 % des cycles) et `bucket_push` (10,6 %).
Restait à savoir par quoi ces deux-là sont limités.

**Correction de deux entrées ci-dessous.** L'entrée `bdccb6f` ouvre sur « le
coût de l'activation est en trafic mémoire », et l'entrée du profil à 10¹⁵ dit
que « la boucle d'activation attend la mémoire ». **C'est faux**, et l'erreur
tenait à la même paresse que celle qu'elle prétendait corriger : n'ayant pas
mesuré la mémoire, j'avais pris « ce n'est pas l'arithmétique » pour « c'est la
mémoire ». En échantillonnant cette fois sur les compteurs de remplissage :

```
                        % cycles   % remplissages L2   % remplissages L3
  sweep_bucketed          18,8            1,7                0,4
  bucket_push             10,6            1,8                0,4
  sieve_segment           12,0            0,8                1,9
  sweep_over              24,1           70,1               12,2
  sweep_exact_calc        22,0           14,1               82,8
```

Le chemin des seaux fait 29,4 % des cycles pour 3,5 % des défauts L2 et 0,8 %
des défauts L3. L'activation, corrigée elle aussi, n'est pas davantage un
problème de mémoire. Et le programme entier ne prend que 10,9 M de lignes
depuis la DRAM contre 939 M depuis le L2 et 354 M depuis le L3 : il n'est
limité par la DRAM nulle part.

L'argument d'amortissement de `bdccb6f` tient malgré tout — il portait sur le
*nombre de fois* que le travail d'activation est refait, ce que la nature de ce
travail ne change pas. Seule l'étiquette « trafic mémoire » était non mesurée.

**Ce qui limite vraiment ces deux fonctions : le débit d'instructions.**

```
                        % cycles   % instructions    IPC
  bucket_push             10,6           23,2        3,46
  sweep_bucketed          18,8           20,4        1,72
  sweep_over              24,1           22,5        1,48
  sweep_exact_calc        22,0           16,0        1,16
                                     (programme entier : 1,59)
```

`bucket_push` est le code le mieux exécuté du programme. Il ne bloque sur rien :
il exécute simplement beaucoup. À eux deux ils retirent **43,6 % de toutes les
instructions**.

**Le mécanisme, chiffré.** Une fenêtre de seau de 128 KiB couvre 3 932 160
entiers et le seuil des seaux vaut 5 242 880 : tout premier à seau a donc un pas
plus grand que la fenêtre. Chaque entrée raye **exactement une fois**, puis paie
la totalité du dépilement, du calcul de position et du réempilement.

```
                     premiers      marques    instructions   par bit raye
  balayage direct     364 165     3,07e9         41,0 %          3,85
  seaux             1 587 762     2,93e8         43,6 %         42,9
```

Les seaux font **8,7 % du marquage pour 43,6 % des instructions**, soit 11 fois
plus d'instructions par bit rayé que le balayage direct. C'est la comptabilité
qui coûte, pas le marquage. Les deux sommes de 1/p sont calculées
indépendamment du programme ; le compte de premiers à seau qui en sort,
1 587 762, recoupe à dix près celui qu'annonce `-v`.

**Deux réglages que ça met en cause.**

- `-Q`, le préchargement : 322,4 / 311,7 / 317,3 ms pour 0, 8 et 32. Toujours
  non résolu — mais **on sait enfin pourquoi**. C3 constatait le fait à ses
  trois bornes sans pouvoir l'expliquer : il n'y a rien à précharger, ce chemin
  ne rate pas le cache.
- `-K`, la fenêtre : 320,9 / 317,0 / 312,4 / 308,1 / 314,0 ms de 32 à 512 KiB.
  Tout dans la bande de 5 %. Élargir la fenêtre pour qu'une entrée raye
  plusieurs fois — la sortie évidente au 42,9 — ne donne rien de résolu.

**Ce que ça laisse.** Sur les seaux, le levier est le nombre d'instructions par
marque : fusionner dépilement, marquage et réempilement, non pas précharger ni
réagencer la mémoire. Et la pression mémoire du programme, elle, est un autre
chantier : `sweep_exact_calc` porte 82,8 % des défauts L3, avec un segment de
2048 KiB par thread contre 1 MiB de L2 partagé par deux threads SMT.

---

## 2026-08-29 — `bdccb6f` · Le découpage en chunks suit le régime

Suite directe du profil : le coût de l'activation est en trafic mémoire, et il
est **par chunk**, pas par segment. Un chunk peut démarrer n'importe où, donc
il repose toutes les entrées de seau avant de cribler — 1 587 772 entrées,
80 fois, à `[10¹⁵, +10¹⁰]`.

`-c 4` le confirmait à −8,8 %, mais on ne pouvait pas en faire un défaut : la
même valeur perd 12 % à 10¹². Le défaut devient donc conditionnel plutôt que
constant — **un chunk doit couvrir au moins 40 candidats par entrée reposée**,
sans jamais descendre sous 2 chunks par thread. Sans premier à seau la
contrainte est vide et le découpage ne bouge pas : c'est ce qui la rend sûre en
bas, où il n'y a rien à amortir.

```
  borne        seaux      chunks         avant     apres
  10^11            0   80x2 -> 80x2      85,6      84,7    -1,1 %
  10^12            0   80x2 -> 80x2     107,3     107,7    +0,4 %
  10^13            0   80x2 -> 80x2     144,7     146,6    +1,3 %
  10^14       300409   80x2 -> 80x2     215,7     214,7    -0,5 %
  5.10^14    1046258   80x2 -> 53x3     282,4     283,4    +0,4 %
  7.10^14    1287094   80x2 -> 40x4     310,8     292,7    -5,8 %
  10^15      1587772   80x2 -> 40x4     332,3     310,3    -6,6 %
```

A/B entrelacé, meilleur de 7, fenêtres de 10¹⁰. La règle ne mord qu'à partir de
5·10¹⁴ et ne gagne qu'à partir de 7·10¹⁴ ; ailleurs le découpage est identique.
Vérifiée inchangée aussi sur les comptages complets, sur une fenêtre étroite
(`-d 1e9`, où le plancher de chunks par thread protège), sur deux fenêtres
larges (`-d 1e11`, `-d 1e12`, où le défaut est déjà assez gros) et à `-t 4`.

Le rapport 40 est **mesuré, pas dérivé**, et l'entrée le dit pour que personne
ne le prenne pour une constante physique : balayage de `-c 1` à `-c 8` à 10¹⁴
et 10¹⁵, optimum à 4 segments par chunk à 10¹⁵, soit 42 candidats par entrée.
À 10¹⁴ l'optimum est plat de 2 à 6, ce qui explique que la règle puisse y
rester inactive sans rien coûter.

Le rapport à primesieve 12.15 passe de 0,82× à 0,89× à 10¹⁵ : le retard tombe
de 18 % à 12 %. C'est le premier gain pris sur le criblage lui-même, les deux
paliers précédents portant sur le coût fixe.

`-v` annonce désormais le nombre de premiers à seau, sans quoi le découpage
retenu n'est pas relisible.

---

## 2026-08-29 — Profil à 10¹⁵ · La division de la ligne 1927 n'était pas le coût

C3 se reprochait de ne pas dire *où* passent les 18 % de retard à 10¹⁵ :
l'ablation dit ce que coûte couper un étage, pas ce que coûte le garder. Ce
profil répond à la question, et se trompe une fois en chemin.

**Le profil, en regard d'une borne où les seaux sont hors service.** Temps CPU,
16 threads, attribution par ligne source — le rapport par symbole ne montre que
`main._omp_fn.0` à 48 %, tout étant inliné.

```
                                    [1e15,+1e10]   [1e13,+1e10]
  sweep_over                            22,5 %         49,6 %
  sweep_exact_calc                      20,5 %         30,5 %
  sieve_segment, dont activation        20,2 %          2,3 %
      (le seul bloc d'activation)      (17,6 %)        (0,00 %)
  sweep_bucketed                        16,3 %            —
  bucket_push                            9,1 %            —
  libgomp                                2,6 %          3,9 %
```

**43 % du temps CPU à 10¹⁵ est la machinerie des seaux**, dont rien n'existe à
10¹³. C'est là qu'est le régime distancé, et c'est cohérent avec C3 : couper
les seaux coûtait 21 %, la plaque 14 %.

**La piste fausse.** La ligne la plus chaude de tout le programme était
`main12.c:1927`, à 4,04 % — `m = (low + p - 1) / p`, une division 64 bits par
une variable, les deux autres divisions du bloc étant par des constantes et
déjà transformées par le compilateur. Les compteurs matériels semblaient
confirmer :

```
                          [1e15,+1e10]   [1e13,+1e10]
  ex_div_count              173,8 M         19,2 M
  ex_div_busy              1,23 G cycles   0,13 G
                          (5,6 % des cycles)
```

Et le compte de divisions est exactement linéaire en nombre de chunks —
2,66 M par chunk, de 4 chunks (12,5 M) à 159 chunks (423 M). Chacun des
~1,9 M premiers de criblage est donc réactivé **dans chaque chunk**, un chunk
pouvant démarrer n'importe où. Ce qui explique enfin le `-c 1` à +28 % de C3,
resté sans mécanisme.

**Le correctif a été écrit, et il marche — sauf sur le temps.** Réciproque
entière `M = floor(2^64 / p)` précalculée une fois par premier, puis
`ceil(low/p)` par multiplication haute 128 bits et au plus une correction
(l'écart au quotient vaut au plus `low·r/(p·2^64) < low/2^64 < 1`, quelle que
soit la borne).

```
  divisions materielles          174,2 M  ->  36,3 M     -79 %
  part du bloc d'activation       17,64 %  ->  12,85 %
  comptes sur quatre bornes      identiques, check 127/127
  temps a [1e15, +1e10]           337,2 ms ->  336,2 ms   -0,3 %
```

A/B entrelacé, meilleur de 7 ; et sur les six points de 10¹⁰ à 10¹⁵, tout
tient dans ±1,7 %. Une troisième variante a écarté l'explication paresseuse
— « les 15,7 Mo du tableau mangent le gain » : tableau alloué et rempli mais
division conservée, 335,5 ms. Les trois variantes sont indiscernables. Ni le
tableau ne coûte, ni la division ne coûtait.

**Le mécanisme de l'erreur.** Le diviseur est une unité séparée : `ex_div_busy`
mesure son **occupation**, jamais le chemin critique. À cette borne la boucle
d'activation attend la mémoire — les écritures dispersées d'entrées de seau —
et l'exécution dans le désordre recouvre entièrement les ~7 cycles de chaque
division. Les 4,04 % d'échantillons sur la ligne 1927 sont l'artefact classique
de l'attribution par ligne, qui crédite l'instruction à longue latence quand
ses voisines sont bloquées sur la mémoire. Les deux compteurs disaient la même
chose, et aucun des deux ne disait « chemin critique ». Le correctif n'est pas
retenu.

**Ce qui survit.** Le coût de l'activation est réel mais il est en trafic
mémoire, pas en arithmétique : 1,58 M d'entrées de seau reposées à chaque
chunk. D'où le seul levier mesuré, qui a maintenant son mécanisme —
`-c 4` (40 chunks au lieu de 80) donne −8,8 % à 10¹⁵, meilleur de 7 entrelacé,
ce qui ramènerait le rapport de 0,82× à 0,89×. Mais ce n'est pas un défaut à
changer :

```
  borne        defaut    -c 4
  10^12       107,0 ms  120,0 ms   +12,1 %
  10^13       149,6 ms  162,3 ms    +8,5 %
  10^14       220,8 ms  212,1 ms    -3,9 %
  10^15       333,3 ms  304,1 ms    -8,8 %
```

Sous le seuil des seaux il n'y a aucune activation à économiser, et agrandir
les chunks ne fait que dégrader l'équilibrage. Reste à faire :
`default_chunk_bytes` dépendant du régime, gros chunks quand les premiers
passent par les seaux, petits sinon.

**Note d'outil.** `DEBUGINFOD_URLS` pointe sur `debuginfod.ubuntu.com` dans cet
environnement. Toute résolution fine — `-s srcline`, `--inline`, `annotate` —
part alors en requêtes réseau et `perf report` se bloque plusieurs minutes,
sans message. `export DEBUGINFOD_URLS=` avant tout `perf report` : le même
rapport sort en une seconde.

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
