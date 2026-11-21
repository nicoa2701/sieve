# Compilateur : le premier de CC_PREFERRED qui est installe, sinon le cc du
# systeme. Un CC explicite — ligne de commande ou environnement — est toujours
# honore et court-circuite la detection ; la CI en depend pour sa matrice
# gcc x clang, et check.sh comme sanitize en heritent.
CC_PREFERRED ?= gcc-16 gcc-15 gcc-13

CC_FOUND := $(shell for c in $(CC_PREFERRED); do \
                command -v $$c >/dev/null 2>&1 && { echo $$c; break; }; \
            done)

ifeq ($(filter-out default undefined,$(origin CC)),)
CC := $(if $(CC_FOUND),$(CC_FOUND),cc)
endif
CFLAGS  ?= -O3 -g -Wall -Wextra
ARCH    ?= -march=native

OPENMP  := -fopenmp
# pthread_once garde les tables construites une seule fois ; -fopenmp l'amene
# deja, la variante sans OpenMP en a besoin sur les libc qui le separent.
PTHREAD := -pthread
SINK    ?= 0

# Bibliotheque de reference de `make check-crible`.
CRIBLE_LIB ?= sieve/build/libcrible.a

CC_MACROS := $(shell $(CC) $(ARCH) -dM -E - < /dev/null 2>/dev/null)

ifneq (,$(findstring __AVX512F__,$(CC_MACROS)))
SIMD_TIER := intrinseques AVX-512
else ifneq (,$(findstring __AVX2__,$(CC_MACROS)))
SIMD_TIER := C portable, cible AVX2
else ifneq (,$(findstring __SSE2__,$(CC_MACROS)))
SIMD_TIER := C portable, cible SSE2
else
SIMD_TIER := C portable, cible scalaire
endif

.PHONY: all clean run12 simd check sanitize force lib check-api check-crible check-arith check-haut

# compte, quand son depot est range a cote (count/, et sieve/ pour son
# en-tete) : make le relie a libroue12.a et pose le binaire ici. Absent, make
# ne construit que le programme et la bibliotheque.
ifneq ($(wildcard count/Makefile),)
COMPTE := compte
endif

# Les sources de compte gardent leur cible portable ; ses noyaux s'aiguillent
# a l'execution.
COMPTE_ARCH ?= x86-64-v2

all: roue12 libroue12.a $(COMPTE)
	@echo "Pre-crible : $(SIMD_TIER)   [$(CC) $(ARCH)]"

simd:
	@echo "CC              : $(CC)"
	@echo "ARCH            : $(ARCH)"
	@echo "Palier detecte  : $(SIMD_TIER)"
	@echo "__AVX512F__     : $(if $(findstring __AVX512F__,$(CC_MACROS)),oui,non)"
	@echo "__AVX2__        : $(if $(findstring __AVX2__,$(CC_MACROS)),oui,non)"
	@echo "__SSE2__        : $(if $(findstring __SSE2__,$(CC_MACROS)),oui,non)"

roue12: main12.o
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -o $@ $^ -lm

# Temoin des drapeaux : un changement de CC, CFLAGS, ARCH, OPENMP ou SINK
# recompile, y compris passe en ligne de commande.
BUILD_FLAGS := $(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -DSINK_TAIL=$(SINK)

.build-flags: force
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@

main12.o: main12.c roue12.h .build-flags
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -DSINK_TAIL=$(SINK) -c -o $@ $<

# La bibliotheque : le meme fichier sans la ligne de commande. Elle se lie
# avec -fopenmp -lm et offre les fonctions de roue12.h.
lib: libroue12.a

main12-lib.o: main12.c roue12.h .build-flags
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -DSINK_TAIL=$(SINK) -DROUE12_LIB -c -o $@ $<

libroue12.a: main12-lib.o
	rm -f $@
	ar rcs $@ $^

# Le Makefile de compte recoit le compilateur retenu ici et une bibliotheque
# deja construite ; MAKEFLAGS est vide pour qu'un CFLAGS ou un ARCH passe a ce
# make-ci, qui n'ont pas le meme sens chez lui, ne le traversent pas. La copie
# n'a lieu que si le binaire a change.
compte: libroue12.a force
	MAKEFLAGS= $(MAKE) -C count SIEVE=roue12 ROUE12_PREBUILT=1 CC='$(CC)' ARCH='$(COMPTE_ARCH)' build-roue12/compte
	@cmp -s count/build-roue12/compte $@ || cp count/build-roue12/compte $@

test_api: test_api.c roue12.h libroue12.a
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -o $@ test_api.c libroue12.a -lm

# L'API contre ses references independantes.
check-api: test_api
	./test_api

# Les calculs de l'activation qui frolent 2^64, contre une reference sur 128
# bits : aucun premier de base, une seconde.
test_arith: test_arith.c main12.c roue12.h
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) $(PTHREAD) -DSINK_TAIL=$(SINK) -o $@ test_arith.c -lm

check-arith: test_arith
	./test_arith

# Les bornes hautes, 10^19 et le voisinage de 2^64 : 813 Mo de premiers de
# base, 1,6 Go le temps de les construire. A lancer sur une machine qui les a.
check-haut: test_api roue12
	./test_api haut
	HAUT=1 sh check.sh

# Le meme programme lie a crible : les deux bibliotheques doivent sortir les
# memes resumes. Demande $(CRIBLE_LIB), que ce Makefile ne construit pas.
check-crible: test_api
	$(CC) $(CFLAGS) $(ARCH) $(PTHREAD) -o test_api-crible test_api.c $(CRIBLE_LIB) -lm
	./test_api $(NIVEAU) | grep '^D ' > .resume-roue12
	./test_api-crible ref $(NIVEAU) | grep '^D ' > .resume-crible
	@if cmp -s .resume-roue12 .resume-crible; then \
	    echo "roue12 et crible : `wc -l < .resume-roue12` resumes identiques"; \
	    rm -f .resume-roue12 .resume-crible; \
	else \
	    diff .resume-roue12 .resume-crible; \
	    rm -f .resume-roue12 .resume-crible; \
	    exit 1; \
	fi

run12: roue12
	./roue12 $(LOW) $(LIMIT) $(ARGS)

SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

check: roue12
	sh check.sh

sanitize: main12.c
	$(CC) $(SAN) $(ARCH) $(OPENMP) -DSINK_TAIL=0 -o roue12-asan main12.c -lm
	$(CC) $(SAN) $(ARCH) $(OPENMP) -DSINK_TAIL=1 -o roue12-asan-sink main12.c -lm
	@for b in roue12-asan roue12-asan-sink; do \
	    for t in "0" "1e8" "1e12 -d 1e8"; do \
	        printf '%-18s %-12s ' "$$b" "$$t"; \
	        ./$$b $$t > /dev/null || exit 1; \
	        echo ok; \
	    done; \
	done

clean:
	rm -f roue12 main12.o roue12-asan roue12-asan-sink .build-flags
	rm -f main12-lib.o libroue12.a test_api test_api-crible compte test_arith
