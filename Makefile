CC      ?= cc
CFLAGS  ?= -O3 -g -Wall -Wextra
ARCH    ?= -march=native

OPENMP  := -fopenmp

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

.PHONY: all clean run12 simd check sanitize

all: roue12
	@echo "Pre-crible : $(SIMD_TIER)   [$(CC) $(ARCH)]"

simd:
	@echo "CC              : $(CC)"
	@echo "ARCH            : $(ARCH)"
	@echo "Palier detecte  : $(SIMD_TIER)"
	@echo "__AVX512F__     : $(if $(findstring __AVX512F__,$(CC_MACROS)),oui,non)"
	@echo "__AVX2__        : $(if $(findstring __AVX2__,$(CC_MACROS)),oui,non)"
	@echo "__SSE2__        : $(if $(findstring __SSE2__,$(CC_MACROS)),oui,non)"

roue12: main12.o
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) -o $@ $^ -lm

main12.o: main12.c
	$(CC) $(CFLAGS) $(ARCH) $(OPENMP) -DSINK_TAIL=0 -c -o $@ $<

run12: roue12
	./roue12 $(LOW) $(LIMIT) $(ARGS)

SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

check: roue12
	sh check.sh

sanitize: main12.c
	$(CC) $(SAN) $(OPENMP) -DSINK_TAIL=0 -o roue12-asan main12.c -lm
	$(CC) $(SAN) $(OPENMP) -DSINK_TAIL=1 -o roue12-asan-sink main12.c -lm
	@for b in roue12-asan roue12-asan-sink; do \
	    for t in "0" "1e8" "1e12 -d 1e8"; do \
	        printf '%-18s %-12s ' "$$b" "$$t"; \
	        ./$$b $$t > /dev/null || exit 1; \
	        echo ok; \
	    done; \
	done

clean:
	rm -f roue12 main12.o roue12-asan roue12-asan-sink
