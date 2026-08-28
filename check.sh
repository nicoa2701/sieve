#!/bin/sh
#
# Protocole de validation de roue12. Sort non nul au premier ecart.
#
# 1) pi(10^n) contre les valeurs connues
# 2) cas limites et bornes
# 3) intervalles contre une reference independante
# 4) coherence entre configurations d'etages
# 5) regressions des bugs corriges, numerotes comme dans BUG.md
#
# B5 n'a pas de test : la course ne s'ouvre qu'a un echec d'allocation, que
# ni ThreadSanitizer ni une interposition n'atteignent proprement.

ROUE=${ROUE:-./roue12}

pass=0
fail=0

ko() { fail=$((fail + 1)); printf 'ECHEC  %s\n' "$1"; }

# Compte attendu.
expect()
{
    want=$1
    shift
    got=`$ROUE "$@" 2>/dev/null | sed -n 's/^Found \([0-9]*\) .*/\1/p'`
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        ko "$* : attendu $want, obtenu ${got:-<rien>}"
    fi
}

# Code de retour attendu.
expect_rc()
{
    want=$1
    shift
    $ROUE "$@" >/dev/null 2>&1
    got=$?
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        ko "$* : code retour attendu $want, obtenu $got"
    fi
}

# Motif attendu sur la sortie, erreur comprise.
expect_out()
{
    want=$1
    shift
    if $ROUE "$@" 2>&1 | grep -qF "$want"; then
        pass=$((pass + 1))
    else
        ko "$* : sortie sans \"$want\""
    fi
}

[ -x "$ROUE" ] || { printf 'introuvable : %s\n' "$ROUE"; exit 1; }

printf 'pi(10^n)\n'
expect 78498      1e6
expect 664579     1e7
expect 5761455    1e8
expect 50847534   1e9
expect 455052511  1e10

printf 'cas limites\n'
expect 0   0
expect 0   1
expect 1   2
expect 4   10
expect 10  30
expect 0   1 1
expect 3   5 11
expect 25  0 100
expect 1   0 2

printf 'intervalles hauts\n'
while read -r lo hi want; do
    [ -n "$lo" ] && expect "$want" "$lo" "$hi"
done <<'FIN'
1000000000000 1000000100000 3614
999999999999 1000000001000 37
10000000000000 10000000010000 354
100000000000000 100000000100000 3045
1000000000000000 1000000000100000 2805
999999999999999 1000000000000000 0
9999999999999999 10000000000000000 0
FIN

printf 'intervalles aleatoires, reference independante\n'
while read -r lo hi want; do
    [ -n "$lo" ] && expect "$want" "$lo" "$hi"
done <<'FIN'
679126 995479 23182
828004 929267 7395
151909 1874246 126221
1123826 1222528 7083
766905 1989100 86660
121632 1185801 80473
450254 528888 6012
180244 1089664 68599
876970 1023467 10632
504706 694944 14287
1155629 1600769 31475
123963 1857997 127464
1185842 1315657 9201
1986946 1990603 256
1322518 1980429 45943
1222633 1287500 4620
1210272 1824256 43111
831899 935895 7591
463642 561332 7456
1167410 1307053 9940
607354 1486352 63532
302524 1436424 83480
247028 1444320 88425
646933 1821877 83993
1711541 1806293 6565
216123 1435826 90337
1197902 1867851 47026
393994 1174968 57719
204326 1353029 85442
1493404 1526323 2324
1183566 1246062 4413
1298157 1514120 15286
1041056 1754507 50451
1115098 1563461 31778
1629966 1794669 11443
976437 1590443 43660
1936596 1966295 2046
758293 1386949 45331
520988 897986 28003
1465897 1721850 17887
171662 1376315 89771
629668 1731084 78974
1038334 1955982 64624
720320 1661593 67383
603849 1880928 91318
153513 401114 19788
1073600 1512033 31232
345950 1933789 114725
717343 1036077 23286
1957209 1989253 2175
884365 966588 5963
1401350 1482740 5730
1603421 1896013 20367
1201722 1530710 23271
713288 1447665 52918
1246483 1767284 36571
1216128 1694493 33678
144206 1905746 129061
196285 762388 43479
994256 1725157 51794
FIN

printf "coherence entre configurations d'etages\n"
for cfg in "-t 1" "-t 3" "-s 32" "-c 1" "-b 0" "-B 0" "-S 0" "-K 0" "-p 0" \
           "-p 7" "-Q 0" "-K 32" "-b 0 -B 0 -S 0" "-s 32 -c 1"
do
    expect 50847534 1e9 $cfg
    expect 3614     1000000000000 1000000100000 $cfg
done

printf 'regressions\n'

# B1 : SIGFPE sur HAUT = 0.
expect_rc 0 0
expect_rc 0 0 0
expect_rc 0 0 -d 0

# B2 : un malloc(0) renvoyant NULL, conforme, ne doit pas passer pour un echec.
# Teste par interposition ; ignore si elle ne se construit pas.
TMP=`mktemp -d 2>/dev/null` || TMP=
if [ -n "$TMP" ] && command -v cc >/dev/null 2>&1; then
    cat > "$TMP/null0.c" <<'FIN'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
void *malloc(size_t n)
{
    static void *(*real)(size_t);
    if (!real) real = dlsym(RTLD_NEXT, "malloc");
    if (n == 0) return NULL;
    return real(n);
}
FIN
    if cc -shared -fPIC -O0 -o "$TMP/null0.so" "$TMP/null0.c" -ldl 2>/dev/null
    then
        got=`LD_PRELOAD=$TMP/null0.so $ROUE 0 2>/dev/null \
             | sed -n 's/^Found \([0-9]*\) .*/\1/p'`
        if [ "$got" = "0" ]; then
            pass=$((pass + 1))
        else
            ko "B2 sous malloc(0) = NULL : attendu 0, obtenu ${got:-<rien>}"
        fi
    else
        printf '  (B2 non couvert : interposeur non construit)\n'
    fi
    rm -rf "$TMP"
else
    printf '  (B2 non couvert : pas de repertoire temporaire ou de cc)\n'
fi

# B3 : les messages de borne donnent les valeurs exactes, pas un arrondi.
expect_rc  1 20000000000000000
expect_out "Limit 20000000000000000 exceeds the maximum 10000000000000000" \
           20000000000000000
expect_rc  1 1e16 -d 1e10
expect_out "Interval end 10000000000000000 + 10000000000 exceeds" 1e16 -d 1e10
expect     0 1e16 -d 0

# B4 : la fenetre de seau reste sous le plafond, quoi qu'on demande.
expect_out "fenetre 8192 KiB" 1e12 -d 1e8 -s 65536 -K 65536 -v
expect 29997546 300000000000000 -d 1e9 -s 65536 -K 16384 -J 1
expect 29997546 300000000000000 -d 1e9 -s 65536 -K 65536 -J 1

# B6 : l'anneau de seaux doit couvrir l'activation d'un premier dont le carre
# tombe dans le segment courant. Il faut donc un segment large et une borne
# au-dela du carre du seuil des seaux. Les deux intervalles sont comptes par
# primesieve 12.10, le dernier controle est pi(10^10).
expect 21201526   17180000000 -d 5e8 -s 1024 -J 1
expect 22484495   4300000000 -d 5e8 -s 512 -J 1
expect 455052511  1e10 -s 1024 -J 1

printf '\n%d reussite(s), %d echec(s)\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
