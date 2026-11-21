/*
 * Crible de premiers segmente, roue mod 30, OpenMP.
 *
 * 1) Un octet porte les 8 residus premiers a 30 ; index_to_number et
 *    wheel_count convertissent entre index de bit et entier.
 * 2) Cinq etages de balayage, choisis par la taille du tour, p octets :
 *    bloc L1, tranche L2, plaque, bande directe, seaux.
 * 3) Seaux : un premier n'est parcouru que dans la fenetre ou il
 *    marque. Marche roue 210 tabulee par classe de residu et non par
 *    premier, anneau de blocs recycles.
 * 4) Pre-crible : les premiers <= 113 en tables periodiques fusionnees
 *    quatre par passe, AVX-512 intrinseque ou C vectorisable.
 * 5) Tailles deduites a l'execution des caches detectes ; le segment
 *    est amorti sur les 1/p des premiers au-dela de la plaque, bande
 *    du milieu et seaux confondus.
 * 6) Le meme fichier donne le programme et la bibliotheque : -DROUE12_LIB
 *    retire la ligne de commande et ne laisse que l'API de roue12.h.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>

#include "roue12.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__AVX512F__) || defined(__AVX2__)
#include <immintrin.h>
#endif


#define MAX_LIMIT ROUE12_MAX_LIMIT

#define FALLBACK_SEGMENT_KB 512

#define MIN_SEGMENT_KB 32
#define MAX_SEGMENT_KB 65536

#define SEGMENT_MARKS_PER_ENTRY 40.0

#ifndef SINK_TAIL
#define SINK_TAIL 1
#endif

#define BLOCK_MIN_BYTES (16 * 1024ULL)

#define CHUNKS_PER_THREAD 8

/*
 * Amortissement de la repose des seaux. Un chunk peut demarrer n'importe ou,
 * donc il replace TOUS les premiers a seau avant de cribler : ce cout est par
 * chunk, pas par segment, et un chunk trop court le paie plein. On exige donc
 * qu'un chunk couvre au moins BUCKET_AMORT candidats par entree reposee.
 *
 * Le rapport est mesure, pas derive : a [10^15, +10^10], 1 587 772 premiers a
 * seau et 16 777 216 candidats par segment, l'optimum est a 4 segments par
 * chunk, soit 42 candidats par entree. En dessous de 10^14 la contrainte est
 * vide, faute de premiers a seau.
 *
 * MIN_CHUNKS_PER_THREAD borne l'effet : agrandir les chunks au point de
 * laisser des threads sans travail coute plus que la repose economisee.
 */
#define BUCKET_AMORT 40
#define MIN_CHUNKS_PER_THREAD 2

/*
 * Part minimale de travail par thread. Chaque thread paie une mise en place —
 * sa creation, son bitset de segment, ses curseurs, son anneau de seaux — et
 * en dessous de MIN_THREAD_SPAN entiers a cribler cette mise en place coute
 * plus que le criblage qu'elle sert.
 *
 * Mesure a 1e6, ou l'intervalle entier tient dans un seul segment : 512 us au
 * defaut contre 109 a -t 1, pour 24 us de criblage reel. Le croisement se
 * situe entre 3e7 et 1e8, ce que la constante encadre.
 *
 * Le second terme, racine(limit) / THREAD_SPAN_ROOT_DIV, prend le relais
 * au-dela de 2,5e15 : la mise en place d'un thread croit comme racine(n),
 * donc sa part doit croitre avec elle. Il est inerte sur tout le domaine
 * mesure jusqu'ici.
 */
#define MIN_THREAD_SPAN 10000000ULL
#define THREAD_SPAN_ROOT_DIV 5

#define BLOCK_MIN_TURNS 1


static const uint8_t residues[8] =
{
    1, 7, 11, 13, 17, 19, 23, 29
};


static inline uint64_t index_to_number(uint64_t index)
{
    uint64_t block = index >> 3;
    uint64_t pos   = index & 7;

    return block * 30 + residues[pos];
}

static uint64_t wheel_count(uint64_t n)
{
    uint64_t block = n / 30;
    uint64_t r = n % 30;

    uint64_t count = block * 8;

    for (int i = 0; i < 8; i++)
    {
        if (residues[i] <= r)
            count++;
    }

    return count;
}


static inline void clear_bit(uint8_t *bits, uint64_t i)
{
    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static uint64_t count_set_bits(const uint8_t *bits,
                               uint64_t nbits)
{
    uint64_t count = 0;
    uint64_t words = nbits >> 6;

    const uint64_t *p = (const uint64_t *)bits;

    for (uint64_t i = 0; i < words; i++)
        count += (uint64_t)__builtin_popcountll(p[i]);

    uint64_t remaining = nbits & 63;

    if (remaining)
    {
        uint64_t mask = (1ULL << remaining) - 1;

        uint64_t w = p[words] & mask;

        count += (uint64_t)__builtin_popcountll(w);
    }

    return count;
}


/*
 * wheel_mask[rc][j] efface le bit du residu (residues[rc] * residues[j]) % 30 :
 * la classe du multiple p * (30q + residues[j]) quand p % 30 == residues[rc].
 */
static const uint8_t wheel_mask[8][8] =
{
    { 0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F },
    { 0xFD, 0xDF, 0xEF, 0xFE, 0x7F, 0xF7, 0xFB, 0xBF },
    { 0xFB, 0xEF, 0xFE, 0xBF, 0xFD, 0x7F, 0xF7, 0xDF },
    { 0xF7, 0xFE, 0xBF, 0xDF, 0xFB, 0xFD, 0x7F, 0xEF },
    { 0xEF, 0x7F, 0xFD, 0xFB, 0xDF, 0xBF, 0xFE, 0xF7 },
    { 0xDF, 0xF7, 0x7F, 0xFD, 0xBF, 0xFE, 0xEF, 0xFB },
    { 0xBF, 0xFB, 0xF7, 0x7F, 0xFE, 0xEF, 0xDF, 0xFD },
    { 0x7F, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE },
};

static const int8_t residue_to_index[30] =
{
    -1,  0, -1, -1, -1, -1, -1,  1, -1, -1,
    -1,  2, -1,  3, -1, -1, -1,  4, -1,  5,
    -1, -1, -1,  6, -1, -1, -1, -1, -1,  7
};

#define TURN_SLOTS 8

/*
 * Un tour vaut 30p entiers, soit p octets, et porte 8 multiples de p.
 * out[k] est le decalage en octets du multiple p * (30q + residues[k]) depuis
 * le debut du tour, le curseur portant deja le terme p / 30 (activate_prime).
 * out[0] vaut donc 0, ce dont les boucles deroulees se servent.
 */
static void
build_turn_offsets(const uint32_t *primes,
                   size_t count,
                   uint32_t *out)
{
    for (size_t i = 0; i < count; i++)
    {
        uint64_t p = primes[i];

        uint64_t base = p / 30;

        for (int k = 0; k < 8; k++)
            out[i * TURN_SLOTS + k] =
                (uint32_t)(p * residues[k] / 30 - base);
    }
}

#ifndef RECOMPUTE_TURN
#define RECOMPUTE_TURN 0
#endif

#if RECOMPUTE_TURN
__attribute__((always_inline))
static inline void
turn_offsets_for(int64_t p, uint32_t *out)
{
    uint64_t up   = (uint64_t)p;
    uint64_t base = up / 30;

    for (int k = 0; k < 8; k++)
        out[k] = (uint32_t)(up * residues[k] / 30 - base);
}
#endif


#define PRESIEVE_DEFAULT_MAX   113

#define PRESIEVE_MAX_PRIME     1000

#define PRESIEVE_TABLES        16
#define PRESIEVE_PASS_TABLES   4

#define PRESIEVE_BUDGET_BYTES  (8ULL << 20)

#ifndef PRESIEVE_CACHE_BYTES
#define PRESIEVE_CACHE_BYTES   (256ULL << 10)
#endif

typedef struct
{
    const uint8_t *data;
    uint64_t       period;
} presieve_table_t;

static presieve_table_t g_pre[PRESIEVE_TABLES + PRESIEVE_PASS_TABLES];

static int      g_pre_tables;
static int      g_pre_passes;
static int      g_pre_groups;
static uint32_t g_pre_primes[176];
static int      g_pre_count;
static uint64_t g_pre_footprint;

static uint8_t  g_pre_ones[128];

/*
 * Repartition en serpentin : les premiers etant croissants, alterner le sens
 * a chaque tour apparie un petit premier avec un grand, ce qui egalise les
 * produits — donc les periodes des tables — d'un groupe a l'autre.
 */
static void presieve_partition(int ngroups, int *assign)
{
    int base  = g_pre_count / ngroups;
    int extra = g_pre_count % ngroups;

    int head = extra * (base + 1);

    for (int i = 0; i < head; i++)
    {
        int r   = i / extra;
        int pos = i % extra;

        assign[i] = (r & 1) ? (extra - 1 - pos) : pos;
    }

    int tail_groups = ngroups - extra;

    for (int i = head; i < g_pre_count; i++)
    {
        int k   = i - head;
        int r   = k / tail_groups;
        int pos = k % tail_groups;

        assign[i] = extra
                  + ((r & 1) ? (tail_groups - 1 - pos) : pos);
    }
}

/*
 * La periode d'un groupe, en octets, est le produit de ses premiers : le motif
 * se repete tous les 30 * produit entiers, et un octet porte 30 entiers.
 */
static double presieve_estimate(int ngroups, int *assign)
{
    presieve_partition(ngroups, assign);

    double estimate[PRESIEVE_TABLES];

    for (int g = 0; g < ngroups; g++)
        estimate[g] = 1.0;

    for (int i = 0; i < g_pre_count; i++)
        estimate[assign[i]] *= g_pre_primes[i];

    double total = 0.0;

    for (int g = 0; g < ngroups; g++)
    {
        total += (estimate[g] < 64.0 ? 64.0 : estimate[g]) + 64.0;
    }

    return total;
}

static int presieve_ngroup(int *assign, double *total)
{
    int cap =
        g_pre_count < PRESIEVE_TABLES ? g_pre_count : PRESIEVE_TABLES;

    int best = cap;

    for (int g = PRESIEVE_PASS_TABLES;
         g <= cap;
         g += PRESIEVE_PASS_TABLES)
    {
        if (presieve_estimate(g, assign) <= (double)PRESIEVE_CACHE_BYTES)
        {
            best = g;
            break;
        }
    }

    *total = presieve_estimate(best, assign);

    return best;
}

static void presieve_mark(uint8_t *table,
                          uint64_t period,
                          uint32_t p)
{
    uint64_t bound = 30ULL * period;

    for (uint64_t q = 0; ; q++)
    {
        int done = 0;

        for (int k = 0; k < 8; k++)
        {
            uint64_t n =
                (uint64_t)p * (30ULL * q + residues[k]);

            if (n >= bound)
            {
                done = 1;
                break;
            }

            table[n / 30] &=
                (uint8_t)~(1u << residue_to_index[n % 30]);
        }

        if (done)
            break;
    }
}

/* Construit une table par groupe et les complete jusqu'a PRESIEVE_PASS_TABLES
   pres, les tables de bourrage etant neutres pour le ET. */
static int presieve_build(uint32_t pmax, double *want)
{
    g_pre_count    = 0;
    g_pre_groups   = 0;
    g_pre_tables   = 0;
    g_pre_passes   = 0;
    g_pre_footprint = 0;

    *want = 0.0;

    for (uint32_t v = 7; v <= pmax; v++)
    {
        int prime = 1;

        for (uint32_t d = 2; d * d <= v; d++)
        {
            if (v % d == 0)
            {
                prime = 0;
                break;
            }
        }

        if (prime)
            g_pre_primes[g_pre_count++] = v;
    }

    if (g_pre_count == 0)
        return 0;

    int assign[sizeof g_pre_primes / sizeof g_pre_primes[0]];

    double total = 0.0;

    int ngroups = presieve_ngroup(assign, &total);

    if (total > (double)PRESIEVE_BUDGET_BYTES)
    {
        *want = total;
        return 1;
    }

    uint64_t period[PRESIEVE_TABLES];

    for (int g = 0; g < ngroups; g++)
        period[g] = 1;

    for (int i = 0; i < g_pre_count; i++)
        period[assign[i]] *= g_pre_primes[i];

    for (int g = 0; g < ngroups; g++)
    {
        uint64_t span = period[g];

        /* Un multiple entier de la periode, porte a 64 octets au moins. */
        if (span < 64)
            span *= (64 + span - 1) / span;

        uint8_t *table =
            aligned_alloc(64, ((span + 64 + 63) / 64) * 64);

        if (!table)
            return 2;

        memset(table, 0xFF, span);

        for (int i = 0; i < g_pre_count; i++)
        {
            if (assign[i] == g)
                presieve_mark(table, span, g_pre_primes[i]);
        }

        /* Copie de queue : presieve_fill lit 64 octets depuis n'importe quel
           decalage de [0, span). */
        memcpy(table + span, table, 64);

        g_pre[g].data   = table;
        g_pre[g].period = span;

        g_pre_footprint += span + 64;
    }

    memset(g_pre_ones, 0xFF, sizeof g_pre_ones);

    int padded =
        (ngroups + PRESIEVE_PASS_TABLES - 1)
        / PRESIEVE_PASS_TABLES * PRESIEVE_PASS_TABLES;

    for (int g = ngroups; g < padded; g++)
    {
        g_pre[g].data   = g_pre_ones;
        g_pre[g].period = 64;
    }

    g_pre_groups = ngroups;
    g_pre_tables = padded;
    g_pre_passes = padded / PRESIEVE_PASS_TABLES;

    return 0;
}

/* La bibliotheque garde ses tables jusqu'a la fin du processus. */
#ifndef ROUE12_LIB
static void presieve_free(void)
{
    for (int g = 0; g < g_pre_groups; g++)
        free((void *)g_pre[g].data);

    g_pre_groups = 0;
    g_pre_tables = 0;
    g_pre_passes = 0;
}
#endif

#if defined(__AVX512F__)
#define PRESIEVE_PATH_LABEL "(intrinseques AVX-512)"
#elif defined(__AVX2__)
#define PRESIEVE_PATH_LABEL "(C portable, cible AVX2)"
#elif defined(__SSE2__)
#define PRESIEVE_PATH_LABEL "(C portable, cible SSE2)"
#else
#define PRESIEVE_PATH_LABEL "(C portable, cible scalaire)"
#endif

/*
 * Combine les tables par ET, quatre par passe : la premiere passe ecrit, les
 * suivantes accumulent. Chaque table avance a son propre pas modulo sa periode.
 */
static void presieve_fill(uint8_t *dst,
                          uint64_t bytes,
                          uint64_t first_byte)
{
    uint64_t n = (bytes + 63) & ~(uint64_t)63;

    for (int t = 0; t < g_pre_passes; t++)
    {
        const presieve_table_t *tab =
            &g_pre[t * PRESIEVE_PASS_TABLES];

        const uint8_t *b0 = tab[0].data;
        const uint8_t *b1 = tab[1].data;
        const uint8_t *b2 = tab[2].data;
        const uint8_t *b3 = tab[3].data;

        const uint64_t P0 = tab[0].period;
        const uint64_t P1 = tab[1].period;
        const uint64_t P2 = tab[2].period;
        const uint64_t P3 = tab[3].period;

        uint64_t o0 = first_byte % P0;
        uint64_t o1 = first_byte % P1;
        uint64_t o2 = first_byte % P2;
        uint64_t o3 = first_byte % P3;

#define PRESIEVE_ADVANCE                        \
            o0 += 64; if (o0 >= P0) o0 -= P0;   \
            o1 += 64; if (o1 >= P1) o1 -= P1;   \
            o2 += 64; if (o2 >= P2) o2 -= P2;   \
            o3 += 64; if (o3 >= P3) o3 -= P3;

#if defined(__AVX512F__)

#if defined(__AVX512VL__)
        /* Les quatre decalages dans un ymm : avance et retour en periode
           en trois instructions vectorielles, extraction en scalaires pour
           former les adresses. En scalaire, gcc emet add, mov, sub, cmp,
           cmov par decalage : 20 instructions par pas contre 9. */
        __m256i vo = _mm256_set_epi64x((long long)o3, (long long)o2,
                                       (long long)o1, (long long)o0);
        const __m256i vP  = _mm256_set_epi64x((long long)P3, (long long)P2,
                                              (long long)P1, (long long)P0);
        const __m256i v64 = _mm256_set1_epi64x(64);

#define PRESIEVE_LOAD_OFFSETS                             \
        {                                                 \
            __m128i lo = _mm256_castsi256_si128(vo);      \
            __m128i hi = _mm256_extracti128_si256(vo, 1); \
            o0 = (uint64_t)_mm_cvtsi128_si64(lo);         \
            o1 = (uint64_t)_mm_extract_epi64(lo, 1);      \
            o2 = (uint64_t)_mm_cvtsi128_si64(hi);         \
            o3 = (uint64_t)_mm_extract_epi64(hi, 1);      \
        }
#define PRESIEVE_STEP                                     \
        vo = _mm256_add_epi64(vo, v64);                   \
        vo = _mm256_min_epu64(vo, _mm256_sub_epi64(vo, vP));
#else
#define PRESIEVE_LOAD_OFFSETS
#define PRESIEVE_STEP PRESIEVE_ADVANCE
#endif

        if (t == 0)
        {
            for (uint64_t i = 0; i < n; i += 64)
            {
                PRESIEVE_LOAD_OFFSETS
                __m512i v = _mm512_loadu_si512(b0 + o0);

                v = _mm512_and_si512(v, _mm512_loadu_si512(b1 + o1));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b2 + o2));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b3 + o3));

                _mm512_store_si512((__m512i *)(dst + i), v);

                PRESIEVE_STEP
            }
        }
        else
        {
            for (uint64_t i = 0; i < n; i += 64)
            {
                PRESIEVE_LOAD_OFFSETS
                __m512i v = _mm512_loadu_si512(b0 + o0);

                v = _mm512_and_si512(v, _mm512_loadu_si512(b1 + o1));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b2 + o2));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b3 + o3));

                v = _mm512_and_si512(
                        v,
                        _mm512_load_si512((const __m512i *)(dst + i)));

                _mm512_store_si512((__m512i *)(dst + i), v);

                PRESIEVE_STEP
            }
        }

#undef PRESIEVE_LOAD_OFFSETS
#undef PRESIEVE_STEP

#else

        for (uint64_t i = 0; i < n; i += 64)
        {
            for (int k = 0; k < 64; k++)
            {
                uint8_t v = (uint8_t)(b0[o0 + k] & b1[o1 + k] &
                                      b2[o2 + k] & b3[o3 + k]);

                if (t == 0)
                    dst[i + k] = v;
                else
                    dst[i + k] &= v;
            }

            PRESIEVE_ADVANCE
        }

#endif

#undef PRESIEVE_ADVANCE
    }
}


typedef struct
{
    int32_t  next;
    uint32_t j;
} wheel_cursor_t;


/*
 * Une entree de seau : k vaut p / 30, et at empaquette le decalage en octets
 * dans la fenetre (a partir du bit 9) avec l'indice de marche rc * 48 + j,
 * qui tient sur 9 bits. D'ou BUCKET_WINDOW_MAX_BYTES, qui borne la fenetre.
 */
typedef struct
{
    uint32_t k;
    uint32_t at;
} bucket_entry_t;

#ifndef BUCKET_BLOCK_BYTES
#define BUCKET_BLOCK_BYTES 16384
#endif

/* Plafond de la fenetre de seau : bucket_entry_t.at range le decalage a
   partir du bit 9, donc il doit tenir sur les 23 bits restants. */
#define BUCKET_WINDOW_MAX_BYTES (1ULL << 23)

typedef struct bucket_block
{
    struct bucket_block *next;
    bucket_entry_t      *end;
    bucket_entry_t       e[(BUCKET_BLOCK_BYTES - 16) / 8];
} bucket_block_t;

/* Les blocs sont alignes sur leur propre taille : masquer un pointeur d'entree
   retrouve son bloc. Le -1 traite le pointeur juste apres la derniere entree. */
#define BUCKET_BLOCK_OF(p)                                            \
    ((bucket_block_t *)(((uintptr_t)(p) - 1)                          \
                        & ~(uintptr_t)(BUCKET_BLOCK_BYTES - 1)))

typedef struct
{
    bucket_entry_t **cur;
    bucket_block_t  *stock;
    bucket_block_t **arena;
    size_t           arenas;
    size_t           arenas_cap;
    uint64_t  slots;
    uint64_t  win;
    unsigned  shift;
    int       one;
} bucket_ring_t;


static const uint8_t residues210[48] =
{
      1,  11,  13,  17,  19,  23,  29,  31,
     37,  41,  43,  47,  53,  59,  61,  67,
     71,  73,  79,  83,  89,  97, 101, 103,
    107, 109, 113, 121, 127, 131, 137, 139,
    143, 149, 151, 157, 163, 167, 169, 173,
    179, 181, 187, 191, 193, 197, 199, 209
};

/* Decalages de tour recalculables sans table : pour p = 30k + sr,
   o[t] = k * (residues[t] - 1) + sr * residues[t] / 30. */
static uint8_t off30_lo[8][8];
static uint8_t r30m1[8];

typedef struct
{
    uint8_t  mask;
    uint8_t  dR;
    uint8_t  dT;
    uint8_t  pad;
    uint32_t next;
} wheel_step_t;

static wheel_step_t wheel210_step[384];

static uint8_t next210[211];

static uint8_t next30[31];

/* Entier porte par le bit b d'un mot de 64 bits, relatif a l'octet 0 du mot. */
static uint8_t bit_value[64];

/* Pour chaque valeur d'un octet du bitset, les residus de ses bits a 1 dans
   l'ordre, completes par des zeros : l'extraction AVX2 les lit par huit. */
static uint32_t byte_offsets[256][8];

/*
 * Marche de la roue 210, pour les seaux. Pour p = 30k + sr et un multiple
 * m = 210q + Rj, l'octet vise vaut 210kq + 7*sr*q + k*Rj + Tj, ou Tj vaut
 * sr * Rj / 30. Passer au residu suivant ajoute donc k * dR + dT.
 * next30[r] et next210[r] donnent l'indice du premier residu >= r, pour
 * arrondir un cofacteur sur la roue.
 */
static void build_wheel210(void)
{
    {
        int j = 0;

        for (int r = 0; r <= 30; r++)
        {
            while (j < 8 && residues[j] < r)
                j++;

            next30[r] = (uint8_t)j;
        }
    }

    for (int j = 0; j < 8; j++)
    {
        r30m1[j] = (uint8_t)(residues[j] - 1);

        for (int rc = 0; rc < 8; rc++)
            off30_lo[rc][j] =
                (uint8_t)((unsigned)residues[rc] * residues[j] / 30);
    }

    for (int rc = 0; rc < 8; rc++)
    {
        const unsigned sr = residues[rc];

        for (int j = 0; j < 48; j++)
        {
            const int jn = (j + 1) % 48;

            const unsigned Rj = residues210[j];
            const unsigned Rn = (j == 47) ? 211u : residues210[jn];

            const unsigned Tj = sr * Rj / 30;
            const unsigned Tn = (j == 47) ? 7u * sr : sr * Rn / 30;

            wheel_step_t *w = &wheel210_step[rc * 48 + j];

            w->mask = wheel_mask[rc][residue_to_index[Rj % 30]];
            w->dR   = (uint8_t)(Rn - Rj);
            w->dT   = (uint8_t)(Tn - Tj);
            w->pad  = 0;
            w->next = (uint32_t)(rc * 48 + jn);
        }
    }

    int j = 0;

    for (int r = 0; r <= 210; r++)
    {
        while (j < 48 && residues210[j] < r)
            j++;

        next210[r] = (uint8_t)j;
    }

    for (int b = 0; b < 64; b++)
        bit_value[b] = (uint8_t)(30 * (b >> 3) + residues[b & 7]);

    for (unsigned v = 0; v < 256; v++)
    {
        unsigned n = 0;

        for (unsigned k = 0; k < 8; k++)
        {
            if (v >> k & 1u)
                byte_offsets[v][n++] = residues[k];
        }
    }
}

/* Tables de roue et caches de la machine, construits une seule fois : les
   appels de l'API peuvent arriver de plusieurs threads a la fois. */
static void tables_init(void);


#ifndef PREFETCH_DEFAUT
#define PREFETCH_DEFAUT 32
#endif

#define BUCKET_ARENA_BLOCKS ((1 << 20) / BUCKET_BLOCK_BYTES)

static int bucket_arena(bucket_ring_t *r)
{
    bucket_block_t *a =
        aligned_alloc(BUCKET_BLOCK_BYTES,
                      (size_t)BUCKET_ARENA_BLOCKS * BUCKET_BLOCK_BYTES);

    if (!a)
        return 0;

    if (r->arenas == r->arenas_cap)
    {
        size_t cap = r->arenas_cap ? r->arenas_cap * 2 : 8;

        bucket_block_t **v = realloc(r->arena, cap * sizeof *v);

        if (!v)
        {
            free(a);
            return 0;
        }

        r->arena     = v;
        r->arenas_cap = cap;
    }

    r->arena[r->arenas++] = a;

    for (int i = 0; i < BUCKET_ARENA_BLOCKS; i++)
    {
        a[i].next = r->stock;
        r->stock  = &a[i];
    }

    return 1;
}

/* Le tableau d'entrees remplit exactement le bloc : le pointeur d'un seau
   atteint la frontiere suivante quand le bloc est plein, et vaut NULL quand
   il n'y a pas encore de bloc — un seul test sur ses bits bas couvre les deux
   cas, et dit quand appeler bucket_open. */
static int bucket_open(bucket_ring_t *r, bucket_entry_t **slot)
{
    if (!r->stock && !bucket_arena(r))
        return 0;

    bucket_block_t *b = r->stock;

    r->stock = b->next;
    b->next  = NULL;

    if (*slot)
    {
        bucket_block_t *old = BUCKET_BLOCK_OF(*slot);

        old->end = *slot;
        b->next  = old;
    }

    *slot = b->e;

    return 1;
}

static void bucket_ring_free(bucket_ring_t *r)
{
    for (size_t i = 0; i < r->arenas; i++)
        free(r->arena[i]);

    free(r->arena);
    free(r->cur);

    r->arena = NULL;
    r->cur   = NULL;
}

static int g_prefetch = PREFETCH_DEFAUT;

/*
 * Vide les seaux fenetre par fenetre. Chaque entree reprend ou elle s'etait
 * arretee, marque tant qu'elle reste dans la fenetre, puis est reclassee dans
 * celle ou elle retombe. L'anneau tourne d'un cran par fenetre.
 */
__attribute__((always_inline))
static inline int sweep_bucketed_run(uint8_t *bits,
                                     uint64_t segment_bytes,
                                     bucket_ring_t *r,
                                     uint64_t gw0,
                                     uint64_t chunk_windows,
                                     const int one)
{
    const uint64_t win   = r->win;
    const unsigned shift = r->shift;

    const ptrdiff_t pf = g_prefetch;

    for (uint64_t kw = 0; kw * win < segment_bytes; kw++)
    {
        const uint64_t  gw   = gw0 + kw;
        uint8_t *const  w    = bits + kw * win;
        const uint64_t  rest = segment_bytes - kw * win;
        const uint64_t  end  = rest < win ? rest : win;

        /* Base de l'anneau, invariante pour toute la fenetre : la tenir en
           registre evite que le reempilement ne relise r puis r->cur depuis
           la pile a chaque entree. */
        bucket_entry_t **const cur = r->cur;

        bucket_entry_t **slot = &cur[0];

        const uint64_t left = gw < chunk_windows ? chunk_windows - gw : 0;

        bucket_entry_t *stop = *slot;

        *slot = NULL;

        bucket_block_t *blk = stop ? BUCKET_BLOCK_OF(stop) : NULL;

        while (blk)
        {
        for (const bucket_entry_t *ent = blk->e; ent < stop; ent++)
        {
            {
                const bucket_entry_t *nx = ent + pf;

                if (nx < stop)
                    __builtin_prefetch(w + (nx->at >> 9), 1, 3);
            }

            const uint32_t k = ent->k;

            uint64_t idx = ent->at >> 9;
            uint32_t wi  = ent->at & 511;

            /* Quand le plus petit pas de la roue 210 atteint la fenetre,
               une entree marque au plus une fois : un test au lieu d'une
               boucle, -15 % d'instructions dans les seaux et -6 % de leur
               temps a [10^15, +10^12]. La boucle reste pour un -J bas. */
            if (one)
            {
                if (idx < end)
                {
                    const wheel_step_t st = wheel210_step[wi];

                    w[idx] &= st.mask;

                    idx += (uint64_t)k * st.dR + st.dT;
                    wi   = st.next;
                }
            }
            else
            {
                while (idx < end)
                {
                    const wheel_step_t st = wheel210_step[wi];

                    w[idx] &= st.mask;

                    idx += (uint64_t)k * st.dR + st.dT;
                    wi   = st.next;
                }
            }

            {
                const uint64_t skip = idx >> shift;

                if (skip < left)
                {
                    /* Reempilement ecrit dans la boucle plutot que par un
                       appel. Chaque entree n'y raye qu'une fois — au
                       dimensionnement par defaut, un premier a seau a un
                       pas plus grand que la fenetre — donc la comptabilite
                       pese autant que le marquage, et les cinq instructions
                       economisees ici comptent : le bloc passe de 17 a 12
                       instructions, -2,4 % sur le programme entier, -2,8 %
                       sur [10^15, +10^11]. L'activation a sa propre forme,
                       bucket_activate. */
                    bucket_entry_t **const s = cur + skip;

                    bucket_entry_t *e = *s;

                    if (((uintptr_t)e & (BUCKET_BLOCK_BYTES - 1)) == 0)
                    {
                        if (!bucket_open(r, s))
                            return 0;

                        e = *s;
                    }

                    e->k  = k;
                    e->at = (uint32_t)(((idx & (win - 1)) << 9) | wi);

                    *s = e + 1;
                }
            }
        }

            {
                bucket_block_t *prev = blk->next;

                blk->next = r->stock;
                r->stock  = blk;

                blk = prev;

                if (blk)
                    stop = blk->end;
            }
        }

        if (r->slots > 1)
        {
            for (bucket_block_t *b = r->cur[0]
                     ? BUCKET_BLOCK_OF(r->cur[0]) : NULL; b; )
            {
                bucket_block_t *prev = b->next;

                b->next   = r->stock;
                r->stock  = b;
                b         = prev;
            }

            memmove(r->cur, r->cur + 1,
                    (size_t)(r->slots - 1) * sizeof *r->cur);

            r->cur[r->slots - 1] = NULL;
        }
    }

    return 1;
}

/* Deux copies du vidage, marque unique ou boucle : un test par entree sur
   r->one suffit a annuler le gain de la marque unique. */
__attribute__((noinline))
static int sweep_bucketed(uint8_t *bits,
                          uint64_t segment_bytes,
                          bucket_ring_t *r,
                          uint64_t gw0,
                          uint64_t chunk_windows)
{
    return r->one
        ? sweep_bucketed_run(bits, segment_bytes, r, gw0, chunk_windows, 1)
        : sweep_bucketed_run(bits, segment_bytes, r, gw0, chunk_windows, 0);
}


/*
 * Place le curseur sur le premier multiple de p qui soit a la fois >= low et
 * >= p * p, son cofacteur arrondi au residu suivant de la roue.
 */
static void
activate_prime(uint32_t p,
               uint64_t segment_first_index,
               uint64_t low,
               wheel_cursor_t *c)
{
    uint64_t m = (low + p - 1) / p;

    if (m < p)
        m = p;

    uint64_t q = m / 30;

    int j = next30[m % 30];

    if (j == 8)
    {
        q++;
        j = 0;
    }

    c->next = (int32_t)((int64_t)((uint64_t)p * q + p / 30)
                        - (int64_t)(segment_first_index >> 3));

    c->j = (uint32_t)j;
}

#if SINK_TAIL
/*
 * Queue du tour, sans branchement : les voies qui sortent du segment ecrivent
 * dans un octet perdu au-dela du bitset, dans les 64 octets de garde alloues.
 */
__attribute__((always_inline))
static inline unsigned
mark_partial(uint8_t *bits,
             const uint32_t *o,
             int64_t base,
             int64_t to,
             int64_t sink,
             unsigned from,
             const unsigned rc)
{
    unsigned n = from;

#define MARK_LANE(K)                                            \
    {                                                           \
        int64_t x  = base + o[K];                               \
        int     in = ((unsigned)(K) >= from) & (x < to);        \
                                                                \
        bits[in ? x : sink + (K)] &= wheel_mask[rc][K];         \
                                                                \
        n += (unsigned)in;                                      \
    }

    MARK_LANE(0) MARK_LANE(1) MARK_LANE(2) MARK_LANE(3)
    MARK_LANE(4) MARK_LANE(5) MARK_LANE(6) MARK_LANE(7)

#undef MARK_LANE

    return n;
}
#endif

/*
 * Balaie jusqu'a `to` sans jamais depasser : la boucle deroulee s'arrete des
 * que le tour entier ne tient plus, la queue est traitee voie par voie.
 * `sub` ramene le curseur a l'origine du segment suivant.
 */
__attribute__((always_inline))
static inline void
sweep_exact(uint8_t *bits,
            wheel_cursor_t *c,
            const uint32_t *o_shared,
            int64_t p,
            int64_t to,
            int64_t sub,
            int64_t sink,
            const unsigned rc)
{
    if (p < 7)
        __builtin_unreachable();

#if RECOMPUTE_TURN
    uint32_t o_buf[8];

    turn_offsets_for(p, o_buf);

    const uint32_t *const o = o_buf;

    (void)o_shared;
#else
    const uint32_t *const o = o_shared;
#endif

    int64_t  base = c->next;
    unsigned j    = c->j;

    if (j != 0)
    {
        do
        {
            int64_t x = base + o[j];

            if (x >= to)
                goto done;

            bits[x] &= wheel_mask[rc][j];
        }
        while (++j < 8);

        base += p;
        j = 0;
    }

    {
        const uint32_t o1 = o[1], o2 = o[2], o3 = o[3],
                       o4 = o[4], o5 = o[5], o6 = o[6], o7 = o[7];

        while (base + o7 < to)
        {
            bits[base]      &= wheel_mask[rc][0];
            bits[base + o1] &= wheel_mask[rc][1];
            bits[base + o2] &= wheel_mask[rc][2];
            bits[base + o3] &= wheel_mask[rc][3];
            bits[base + o4] &= wheel_mask[rc][4];
            bits[base + o5] &= wheel_mask[rc][5];
            bits[base + o6] &= wheel_mask[rc][6];
            bits[base + o7] &= wheel_mask[rc][7];

            base += p;
        }
    }

#if SINK_TAIL
    j = mark_partial(bits, o, base, to, sink, 0, rc);
#else
    (void)sink;

    while (j < 8)
    {
        int64_t x = base + o[j];

        if (x >= to)
            break;

        bits[x] &= wheel_mask[rc][j];
        j++;
    }
#endif

done:
    c->next = (int32_t)(base - sub);
    c->j    = j;
}

/* Meme balayage, decalages recalcules : au-dela de class_mid3 les premiers
   n'ont pas d'entree dans la table des tours. */
__attribute__((always_inline))
static inline void
sweep_exact_calc(uint8_t *bits,
            wheel_cursor_t *c,
            uint32_t p,
            int64_t to,
            int64_t sub,
            int64_t sink,
            const unsigned rc)
{
    uint32_t o[8];

    {
        const uint32_t k = p / 30;

        for (int t = 0; t < 8; t++)
            o[t] = k * r30m1[t] + off30_lo[rc][t];
    }

    int64_t  base = c->next;
    unsigned j    = c->j;

    if (j != 0)
    {
        do
        {
            int64_t x = base + o[j];

            if (x >= to)
                goto done;

            bits[x] &= wheel_mask[rc][j];
        }
        while (++j < 8);

        base += (int64_t)p;
        j = 0;
    }

    {
        const uint32_t o1 = o[1], o2 = o[2], o3 = o[3],
                       o4 = o[4], o5 = o[5], o6 = o[6], o7 = o[7];

        while (base + o7 < to)
        {
            bits[base]      &= wheel_mask[rc][0];
            bits[base + o1] &= wheel_mask[rc][1];
            bits[base + o2] &= wheel_mask[rc][2];
            bits[base + o3] &= wheel_mask[rc][3];
            bits[base + o4] &= wheel_mask[rc][4];
            bits[base + o5] &= wheel_mask[rc][5];
            bits[base + o6] &= wheel_mask[rc][6];
            bits[base + o7] &= wheel_mask[rc][7];

            base += (int64_t)p;
        }
    }

#if SINK_TAIL
    j = mark_partial(bits, o, base, to, sink, 0, rc);
#else
    (void)sink;

    while (j < 8)
    {
        int64_t x = base + o[j];

        if (x >= to)
            break;

        bits[x] &= wheel_mask[rc][j];
        j++;
    }
#endif

done:
    c->next = (int32_t)(base - sub);
    c->j    = j;
}

/*
 * La base : tous les premiers de [7, root], ranges par classe de residu mod 30
 * puis par taille. La classe rc occupe sorted[cls[rc] .. cls[rc + 1]). C'est
 * l'ordre dans lequel le crible les parcourt ; un plan n'y decoupe que des
 * plages. Une fois construite elle n'est plus que lue.
 *
 * refs sert au cache de l'API, sous son verrou.
 */
typedef struct
{
    uint64_t  root;
    uint32_t *sorted;
    size_t    cls[9];
    unsigned  refs;
} base_t;

static void base_free(base_t *b)
{
    if (b)
    {
        free(b->sorted);
        free(b);
    }
}

/*
 * Construit la base jusqu'a `limit`, par le crible du programme lui-meme.
 *
 * Un octet porte 30 entiers au lieu de 2 : la fenetre active tient en L1 et
 * le balayage reutilise sweep_exact_calc, donc la roue 30 et ses masques. Les
 * amorces jusqu'a racine(limit) sortent d'un crible impair trivial a cette
 * taille.
 *
 * L'extraction lit le bitset par mots de 64 bits, un ctz par premier trouve,
 * et le range aussitot dans sa classe : le rang du bit dans son octet est la
 * classe. Un tri apres coup demandait deux passes et un % 30 par premier.
 *
 * Rend NULL si une allocation echoue : la fonction sert aussi a l'API, qui ne
 * peut pas terminer le processus de son appelant.
 */
#define BASE_SEGMENT_BYTES (32 * 1024)

static base_t *base_build(uint64_t limit)
{
    uint32_t *part[8] = { NULL };
    size_t    cap[8]  = { 0 };
    size_t    n[8]    = { 0 };

    uint8_t        *seed = NULL;
    uint32_t       *sp   = NULL;
    wheel_cursor_t *cur  = NULL;
    uint8_t        *ok   = NULL;
    uint8_t        *bits = NULL;

    base_t *b = calloc(1, sizeof *b);

    if (!b)
        return NULL;

    b->root = limit;

    if (limit < 7)
    {
        b->sorted = malloc(sizeof *b->sorted);

        if (!b->sorted)
        {
            free(b);

            return NULL;
        }

        return b;
    }

    tables_init();

    uint64_t root = (uint64_t)sqrt((double)limit);

    while ((root + 1) <= limit / (root + 1))
        root++;

    while (root && root > limit / root)
        root--;

    /* Amorces : premiers impairs de [7, root], indice i pour 2i+1. */
    size_t sn = (size_t)(root / 2) + 1;

    seed = malloc(sn);

    if (!seed)
        goto oom;

    memset(seed, 1, sn);

    seed[0] = 0;

    for (size_t i = 1; i < sn; i++)
    {
        uint64_t p = 2 * (uint64_t)i + 1;

        if (!seed[i] || p * p > root)
            continue;

        for (uint64_t j = p * p / 2; j < sn; j += p)
            seed[j] = 0;
    }

    size_t ns = 0;

    for (size_t i = 3; i < sn; i++)
    {
        if (seed[i])
            ns++;
    }

    sp   = malloc((ns ? ns : 1) * sizeof *sp);
    cur  = malloc((ns ? ns : 1) * sizeof *cur);
    ok   = calloc(ns ? ns : 1, 1);
    bits = aligned_alloc(64, BASE_SEGMENT_BYTES + 64);

    if (!sp || !cur || !ok || !bits)
        goto oom;

    ns = 0;

    for (size_t i = 3; i < sn; i++)
    {
        if (seed[i])
            sp[ns++] = (uint32_t)(2 * i + 1);
    }

    free(seed);

    seed = NULL;

    /* Les classes se partagent les premiers a peu pres egalement ; la marge
       d'un segment evite presque toujours d'agrandir. */
    {
        double est = 1.26 * (double)limit / log((double)limit) / 8.0;

        for (unsigned c = 0; c < 8; c++)
        {
            cap[c]  = (size_t)est + BASE_SEGMENT_BYTES;
            part[c] = malloc(cap[c] * sizeof *part[c]);

            if (!part[c])
                goto oom;
        }
    }

    const uint64_t total    = wheel_count(limit);
    const uint64_t seg_bits = (uint64_t)BASE_SEGMENT_BYTES * 8;

    for (uint64_t first = 0; first < total; first += seg_bits)
    {
        uint64_t nbits = total - first;

        if (nbits > seg_bits)
            nbits = seg_bits;

        uint64_t nbytes = (nbits + 7) >> 3;

        memset(bits, 0xFF, nbytes);

        uint64_t low  = index_to_number(first);
        uint64_t high = index_to_number(first + nbits - 1);

        for (size_t k = 0; k < ns; k++)
        {
            uint64_t p = sp[k];

            /* sp est croissant : au-dela, plus rien ne marque ce segment. */
            if (p * p > high)
                break;

            if (!ok[k])
            {
                activate_prime(sp[k], first, low, &cur[k]);
                ok[k] = 1;
            }

            sweep_exact_calc(bits, &cur[k], sp[k],
                             (int64_t)nbytes, (int64_t)nbytes,
                             (int64_t)nbytes,
                             (unsigned)residue_to_index[sp[k] % 30]);
        }

        /* Le bit 0 porte 1, qui n'est pas premier. */
        if (first == 0)
            clear_bit(bits, 0);

        /* Un segment donne au plus un premier par octet et par classe. */
        for (unsigned c = 0; c < 8; c++)
        {
            if (cap[c] - n[c] < BASE_SEGMENT_BYTES)
            {
                size_t grown_cap = cap[c] * 2;

                uint32_t *grown =
                    realloc(part[c], grown_cap * sizeof *grown);

                if (!grown)
                    goto oom;

                part[c] = grown;
                cap[c]  = grown_cap;
            }
        }

        uint64_t words = (nbits + 63) >> 6;

        /* first est multiple de 8 : le mot w commence a l'octet
           first / 8 + 8w. */
        const uint64_t byte0 = first >> 3;

        for (uint64_t w = 0; w < words; w++)
        {
            uint64_t v;

            memcpy(&v, bits + w * 8, sizeof v);

            uint64_t rest = nbits - w * 64;

            if (rest < 64)
                v &= (1ULL << rest) - 1;

            const uint32_t at = (uint32_t)((byte0 + 8 * w) * 30);

            while (v)
            {
                int t = __builtin_ctzll(v);

                v &= v - 1;

                const unsigned c = (unsigned)t & 7;

                part[c][n[c]++] = at + bit_value[t];
            }
        }
    }

    {
        size_t count = 0;

        for (unsigned c = 0; c < 8; c++)
        {
            b->cls[c] = count;
            count    += n[c];
        }

        b->cls[8] = count;

        b->sorted = malloc((count ? count : 1) * sizeof *b->sorted);

        if (!b->sorted)
            goto oom;

        for (unsigned c = 0; c < 8; c++)
        {
            memcpy(b->sorted + b->cls[c], part[c], n[c] * sizeof *part[c]);

            free(part[c]);

            part[c] = NULL;
        }
    }

    free(sp);
    free(cur);
    free(ok);
    free(bits);

    return b;

oom:
    for (unsigned c = 0; c < 8; c++)
        free(part[c]);

    free(seed);
    free(sp);
    free(cur);
    free(ok);
    free(bits);
    free(b);

    return NULL;
}


/*
 * Variante rapide : elle ecrit un tour entier des que base < to, donc jusqu'a
 * p * 29 / 30 octets au-dela. L'appelant ne l'emploie que s'il reste cette
 * marge, celle que les overshoot mesurent.
 */
__attribute__((always_inline))
static inline void
sweep_over(uint8_t *bits,
           wheel_cursor_t *c,
           const uint32_t *o_shared,
           int64_t p,
           int64_t to,
           const unsigned rc)
{
    if (p < 7)
        __builtin_unreachable();

#if RECOMPUTE_TURN
    uint32_t o_buf[8];

    turn_offsets_for(p, o_buf);

    const uint32_t *const o = o_buf;

    (void)o_shared;
#else
    const uint32_t *const o = o_shared;
#endif

    int64_t  base = c->next;
    unsigned j    = c->j;

    if (j != 0)
    {
        do
        {
            int64_t x = base + o[j];

            if (x >= to)
                goto done;

            bits[x] &= wheel_mask[rc][j];
        }
        while (++j < 8);

        base += p;
        j = 0;
    }

    {
        const uint32_t o1 = o[1], o2 = o[2], o3 = o[3],
                       o4 = o[4], o5 = o[5], o6 = o[6], o7 = o[7];

        while (base < to)
        {
            bits[base]      &= wheel_mask[rc][0];
            bits[base + o1] &= wheel_mask[rc][1];
            bits[base + o2] &= wheel_mask[rc][2];
            bits[base + o3] &= wheel_mask[rc][3];
            bits[base + o4] &= wheel_mask[rc][4];
            bits[base + o5] &= wheel_mask[rc][5];
            bits[base + o6] &= wheel_mask[rc][6];
            bits[base + o7] &= wheel_mask[rc][7];

            base += p;
        }
    }

done:
    c->next = (int32_t)base;
    c->j    = j;
}

__attribute__((noinline))
static void sweep_flat(uint8_t *bits,
                    uint64_t segment_bytes,
                    const uint32_t *sorted,
                    const uint32_t *turn,
                    const size_t   *toff,
                    const size_t *class_start,
                    const size_t *mid,
                    wheel_cursor_t *cursor,
                    const size_t *coff,
                    uint64_t block_bytes,
                    uint64_t overshoot,
                    int64_t sink,
                    int blocked)
{
    const size_t o0 = toff[0], o1 = toff[1], o2 = toff[2], o3 = toff[3],
                 o4 = toff[4], o5 = toff[5], o6 = toff[6], o7 = toff[7];

    /* Les curseurs sont ranges sans trou, classe apres classe. */
    const size_t co0 = coff[0], co1 = coff[1], co2 = coff[2], co3 = coff[3],
                 co4 = coff[4], co5 = coff[5], co6 = coff[6], co7 = coff[7];

        if (blocked)
        {
            for (uint64_t b0 = 0; b0 < segment_bytes; b0 += block_bytes)
            {
                uint64_t b1  = b0 + block_bytes;
                uint64_t sub = 0;

                if (b1 >= segment_bytes)
                {
                    b1  = segment_bytes;
                    sub = segment_bytes;
                }

                if (b1 + overshoot <= segment_bytes)
                {

#define SWEEP_FLAT(RC)                                        \
                    for (size_t i = class_start[RC];          \
                         i < mid[RC];                         \
                         i++)                                 \
                    {                                         \
                        sweep_over(bits, &cursor[i - co##RC],    \
                                   turn + (i - o##RC) * TURN_SLOTS, \
                                   sorted[i], (int64_t)b1, (RC)); \
                    }

                    SWEEP_FLAT(0) SWEEP_FLAT(1)
                    SWEEP_FLAT(2) SWEEP_FLAT(3)
                    SWEEP_FLAT(4) SWEEP_FLAT(5)
                    SWEEP_FLAT(6) SWEEP_FLAT(7)

#undef SWEEP_FLAT

                }
                else
                {

#define SWEEP_FLAT(RC)                                        \
                    for (size_t i = class_start[RC];          \
                         i < mid[RC];                         \
                         i++)                                 \
                    {                                         \
                        sweep_exact(bits, &cursor[i - co##RC],   \
                                    turn + (i - o##RC) * TURN_SLOTS, \
                                    sorted[i], (int64_t)b1,   \
                                    (int64_t)sub, sink, (RC)); \
                    }

                    SWEEP_FLAT(0) SWEEP_FLAT(1)
                    SWEEP_FLAT(2) SWEEP_FLAT(3)
                    SWEEP_FLAT(4) SWEEP_FLAT(5)
                    SWEEP_FLAT(6) SWEEP_FLAT(7)

#undef SWEEP_FLAT

                }
            }
        }
}

__attribute__((noinline))
static void sweep_chunked(uint8_t *bits,
                          uint64_t segment_bytes,
                          const uint32_t *sorted,
                          const uint32_t *turn,
                          const size_t   *toff,
                          const size_t *class_start,
                          const size_t *mid,
                          const size_t *mid2,
                          wheel_cursor_t *cursor,
                          const size_t *coff,
                          uint64_t block_bytes,
                          uint64_t chunk_bytes,
                          uint64_t overshoot,
                          uint64_t overshoot2,
                          int64_t sink,
                          int blocked)
{
    const size_t o0 = toff[0], o1 = toff[1], o2 = toff[2], o3 = toff[3],
                 o4 = toff[4], o5 = toff[5], o6 = toff[6], o7 = toff[7];

    /* Les curseurs sont ranges sans trou, classe apres classe. */
    const size_t co0 = coff[0], co1 = coff[1], co2 = coff[2], co3 = coff[3],
                 co4 = coff[4], co5 = coff[5], co6 = coff[6], co7 = coff[7];

    uint64_t chunk = chunk_bytes;

    {
        for (uint64_t c0 = 0; c0 < segment_bytes; c0 += chunk)
        {
            uint64_t c1   = c0 + chunk;
            int      last = 0;

            if (c1 >= segment_bytes)
            {
                c1   = segment_bytes;
                last = 1;
            }

            if (blocked)
            {
                for (uint64_t b0 = c0; b0 < c1; b0 += block_bytes)
                {
                    uint64_t b1  = b0 + block_bytes;
                    uint64_t sub = 0;

                    if (b1 >= c1)
                    {
                        b1 = c1;

                        if (last)
                            sub = segment_bytes;
                    }

                    if (b1 + overshoot <= segment_bytes)
                    {

#define SWEEP_BLOCK(RC)                                       \
                        for (size_t i = class_start[RC];      \
                             i < mid[RC];                     \
                             i++)                             \
                        {                                     \
                            sweep_over(bits, &cursor[i - co##RC], \
                                       turn + (i - o##RC) * TURN_SLOTS, \
                                       sorted[i], (int64_t)b1, (RC)); \
                        }

                        SWEEP_BLOCK(0) SWEEP_BLOCK(1)
                        SWEEP_BLOCK(2) SWEEP_BLOCK(3)
                        SWEEP_BLOCK(4) SWEEP_BLOCK(5)
                        SWEEP_BLOCK(6) SWEEP_BLOCK(7)

#undef SWEEP_BLOCK

                    }
                    else
                    {

#define SWEEP_BLOCK(RC)                                       \
                        for (size_t i = class_start[RC];      \
                             i < mid[RC];                     \
                             i++)                             \
                        {                                     \
                            sweep_exact(bits, &cursor[i - co##RC], \
                                        turn + (i - o##RC) * TURN_SLOTS,\
                                        sorted[i], (int64_t)b1, \
                                        (int64_t)sub, sink, (RC)); \
                        }

                        SWEEP_BLOCK(0) SWEEP_BLOCK(1)
                        SWEEP_BLOCK(2) SWEEP_BLOCK(3)
                        SWEEP_BLOCK(4) SWEEP_BLOCK(5)
                        SWEEP_BLOCK(6) SWEEP_BLOCK(7)

#undef SWEEP_BLOCK

                    }
                }
            }

            {
                uint64_t sub = last ? segment_bytes : 0;

                if (c1 + overshoot2 <= segment_bytes)
                {

#define SWEEP_CHUNK(RC)                                       \
                    for (size_t i = mid[RC]; i < mid2[RC]; i++) \
                    {                                         \
                        sweep_over(bits, &cursor[i - co##RC],    \
                                   turn + (i - o##RC) * TURN_SLOTS, \
                                   sorted[i], (int64_t)c1, (RC)); \
                    }

                    SWEEP_CHUNK(0) SWEEP_CHUNK(1)
                    SWEEP_CHUNK(2) SWEEP_CHUNK(3)
                    SWEEP_CHUNK(4) SWEEP_CHUNK(5)
                    SWEEP_CHUNK(6) SWEEP_CHUNK(7)

#undef SWEEP_CHUNK

                }
                else
                {

#define SWEEP_CHUNK(RC)                                       \
                    for (size_t i = mid[RC]; i < mid2[RC]; i++) \
                    {                                         \
                        sweep_exact(bits, &cursor[i - co##RC],   \
                                    turn + (i - o##RC) * TURN_SLOTS, \
                                    sorted[i], (int64_t)c1,   \
                                    (int64_t)sub, sink, (RC)); \
                    }

                    SWEEP_CHUNK(0) SWEEP_CHUNK(1)
                    SWEEP_CHUNK(2) SWEEP_CHUNK(3)
                    SWEEP_CHUNK(4) SWEEP_CHUNK(5)
                    SWEEP_CHUNK(6) SWEEP_CHUNK(7)

#undef SWEEP_CHUNK

                }
            }
        }
    }
}

__attribute__((noinline))
static void sweep_slabbed(uint8_t *bits,
                          uint64_t segment_bytes,
                          const uint32_t *sorted,
                          const uint32_t *turn,
                          const size_t   *toff,
                          const size_t *class_start,
                          const size_t *mid,
                          const size_t *mid2,
                          const size_t *mid3,
                          wheel_cursor_t *cursor,
                          const size_t *coff,
                          uint64_t block_bytes,
                          uint64_t chunk_bytes,
                          uint64_t slab_bytes,
                          uint64_t overshoot,
                          uint64_t overshoot2,
                          uint64_t overshoot3,
                          int64_t sink,
                          int blocked)
{
    const size_t o0 = toff[0], o1 = toff[1], o2 = toff[2], o3 = toff[3],
                 o4 = toff[4], o5 = toff[5], o6 = toff[6], o7 = toff[7];

    /* Les curseurs sont ranges sans trou, classe apres classe. */
    const size_t co0 = coff[0], co1 = coff[1], co2 = coff[2], co3 = coff[3],
                 co4 = coff[4], co5 = coff[5], co6 = coff[6], co7 = coff[7];

    for (uint64_t s0 = 0; s0 < segment_bytes; s0 += slab_bytes)
    {
        uint64_t s1        = s0 + slab_bytes;
        int      last_slab = 0;

        if (s1 >= segment_bytes)
        {
            s1        = segment_bytes;
            last_slab = 1;
        }

        const uint64_t chunk = chunk_bytes;

        {
        for (uint64_t c0 = s0; c0 < s1; c0 += chunk)
        {
            uint64_t c1   = c0 + chunk;
            int      last = 0;

            if (c1 >= s1)
            {
                c1   = s1;
                last = last_slab;
            }

            if (blocked)
            {
                for (uint64_t b0 = c0; b0 < c1; b0 += block_bytes)
                {
                    uint64_t b1  = b0 + block_bytes;
                    uint64_t sub = 0;

                    if (b1 >= c1)
                    {
                        b1 = c1;

                        if (last)
                            sub = segment_bytes;
                    }

                    if (b1 + overshoot <= segment_bytes)
                    {

#define SWEEP_BLOCK(RC)                                       \
                        for (size_t i = class_start[RC];      \
                             i < mid[RC];                     \
                             i++)                             \
                        {                                     \
                            sweep_over(bits, &cursor[i - co##RC], \
                                       turn + (i - o##RC) * TURN_SLOTS, \
                                       sorted[i], (int64_t)b1, (RC)); \
                        }

                        SWEEP_BLOCK(0) SWEEP_BLOCK(1)
                        SWEEP_BLOCK(2) SWEEP_BLOCK(3)
                        SWEEP_BLOCK(4) SWEEP_BLOCK(5)
                        SWEEP_BLOCK(6) SWEEP_BLOCK(7)

#undef SWEEP_BLOCK

                    }
                    else
                    {

#define SWEEP_BLOCK(RC)                                       \
                        for (size_t i = class_start[RC];      \
                             i < mid[RC];                     \
                             i++)                             \
                        {                                     \
                            sweep_exact(bits, &cursor[i - co##RC], \
                                        turn + (i - o##RC) * TURN_SLOTS,\
                                        sorted[i], (int64_t)b1, \
                                        (int64_t)sub, sink, (RC)); \
                        }

                        SWEEP_BLOCK(0) SWEEP_BLOCK(1)
                        SWEEP_BLOCK(2) SWEEP_BLOCK(3)
                        SWEEP_BLOCK(4) SWEEP_BLOCK(5)
                        SWEEP_BLOCK(6) SWEEP_BLOCK(7)

#undef SWEEP_BLOCK

                    }
                }
            }

            {
                uint64_t sub = last ? segment_bytes : 0;

                if (c1 + overshoot2 <= segment_bytes)
                {

#define SWEEP_CHUNK(RC)                                       \
                    for (size_t i = mid[RC]; i < mid2[RC]; i++) \
                    {                                         \
                        sweep_over(bits, &cursor[i - co##RC],    \
                                   turn + (i - o##RC) * TURN_SLOTS, \
                                   sorted[i], (int64_t)c1, (RC)); \
                    }

                    SWEEP_CHUNK(0) SWEEP_CHUNK(1)
                    SWEEP_CHUNK(2) SWEEP_CHUNK(3)
                    SWEEP_CHUNK(4) SWEEP_CHUNK(5)
                    SWEEP_CHUNK(6) SWEEP_CHUNK(7)

#undef SWEEP_CHUNK

                }
                else
                {

#define SWEEP_CHUNK(RC)                                       \
                    for (size_t i = mid[RC]; i < mid2[RC]; i++) \
                    {                                         \
                        sweep_exact(bits, &cursor[i - co##RC],   \
                                    turn + (i - o##RC) * TURN_SLOTS, \
                                    sorted[i], (int64_t)c1,   \
                                    (int64_t)sub, sink, (RC)); \
                    }

                    SWEEP_CHUNK(0) SWEEP_CHUNK(1)
                    SWEEP_CHUNK(2) SWEEP_CHUNK(3)
                    SWEEP_CHUNK(4) SWEEP_CHUNK(5)
                    SWEEP_CHUNK(6) SWEEP_CHUNK(7)

#undef SWEEP_CHUNK

                }
            }
        }
    }

        {
            uint64_t sub = last_slab ? segment_bytes : 0;

            if (s1 + overshoot3 <= segment_bytes)
            {

#define SWEEP_SLAB(RC)                                        \
                for (size_t i = mid2[RC]; i < mid3[RC]; i++)  \
                {                                             \
                    sweep_over(bits, &cursor[i - co##RC],        \
                               turn + (i - o##RC) * TURN_SLOTS, \
                               sorted[i], (int64_t)s1, (RC)); \
                }

                SWEEP_SLAB(0) SWEEP_SLAB(1)
                SWEEP_SLAB(2) SWEEP_SLAB(3)
                SWEEP_SLAB(4) SWEEP_SLAB(5)
                SWEEP_SLAB(6) SWEEP_SLAB(7)

#undef SWEEP_SLAB

            }
            else
            {

#define SWEEP_SLAB(RC)                                        \
                for (size_t i = mid2[RC]; i < mid3[RC]; i++)  \
                {                                             \
                    sweep_exact(bits, &cursor[i - co##RC],       \
                                turn + (i - o##RC) * TURN_SLOTS, \
                                sorted[i], (int64_t)s1,       \
                                (int64_t)sub, sink, (RC));    \
                }

                SWEEP_SLAB(0) SWEEP_SLAB(1)
                SWEEP_SLAB(2) SWEEP_SLAB(3)
                SWEEP_SLAB(4) SWEEP_SLAB(5)
                SWEEP_SLAB(6) SWEEP_SLAB(7)

#undef SWEEP_SLAB

            }
        }
    }
}

static size_t upper_bound_u32(const uint32_t *a, size_t lo, size_t hi,
                              uint64_t v);

static uint64_t isqrt_u64(uint64_t n);

/*
 * Les premiers a seau de la classe rc, sorted[from .. to), entrent dans les
 * seaux des leur activation, positionnes sur la roue 210 : chacun est range
 * dans la fenetre de son premier multiple >= low et >= p * p, s'il tombe dans
 * le chunk. Rend 0 si un bloc de seau ne peut pas etre alloue.
 */
__attribute__((noinline))
static int bucket_activate(const uint32_t *sorted,
                           size_t from,
                           size_t to,
                           unsigned rc,
                           uint64_t low,
                           uint64_t first_byte,
                           bucket_ring_t *ring,
                           uint64_t gw0,
                           uint64_t chunk_windows)
{
    bucket_entry_t **const cur = ring->cur;

    const unsigned shift = ring->shift;
    const uint32_t wheel = rc * 48;

    /* Fenetres du chunk encore devant : au-dela, le premier n'y marque pas. */
    const uint64_t room = chunk_windows > gw0 ? chunk_windows - gw0 : 0;

    /*
     * Un seau perdu pour les entrees hors chunk, qui n'avance jamais. Son
     * adresse ne doit pas tomber sur une frontiere de bloc, que le test de
     * bloc plein prendrait pour un seau a ouvrir.
     */
    bucket_entry_t lost[3];

    bucket_entry_t *lost_top =
        ((uintptr_t)&lost[1] & (BUCKET_BLOCK_BYTES - 1)) ? &lost[1]
                                                         : &lost[2];

    for (size_t a = from; a < to; a++)
    {
        const uint32_t p = sorted[a];

        /* Une division entiere par premier. La meme en flottant, par paquets
           vectorises, a ete mesuree plus lente : 28,5 ms contre 24,2. */
        uint64_t m = (low + p - 1) / p;

        if (m < p)
            m = p;

        uint64_t q = m / 210;
        unsigned j = next210[m % 210];

        if (j == 48)
        {
            q++;
            j = 0;
        }

        m = 210 * q + residues210[j];

        /* p * m >= low, donc x >= 0. */
        const uint64_t x    = (uint64_t)p * m / 30 - first_byte;
        const uint64_t skip = x >> shift;

        /*
         * Aucun branchement ne depend de `in`. Pres de 10^16 un chunk
         * etroit ne garde que 41 % des premiers a seau, et le test
         * bascule une fois sur trois : mal predit, il coutait plus que
         * tout le calcul qui precede — 34,7 ms contre 15,3 pour 5,5
         * millions de premiers. L'entree est donc toujours ecrite, dans
         * son seau ou dans le seau perdu, et seul le pointeur du seau
         * avance ou non.
         */
        const uintptr_t in   = skip < room;
        const uintptr_t mask = (uintptr_t)0 - in;

        bucket_entry_t **const slot = (bucket_entry_t **)
            ((((uintptr_t)cur + (uintptr_t)(skip & mask) * sizeof *cur)
              & mask)
             | ((uintptr_t)&lost_top & ~mask));

        bucket_entry_t *e = *slot;

        if (__builtin_expect(
                ((uintptr_t)e & (BUCKET_BLOCK_BYTES - 1)) == 0, 0))
        {
            if (!bucket_open(ring, slot))
                return 0;

            e = *slot;
        }

        e->k  = p / 30;
        e->at = (uint32_t)((x - (skip << shift)) << 9) | (wheel + j);

        *slot = e + in;
    }

    return 1;
}

/*
 * Un segment. Les premiers sont ranges par classe de residu puis par taille ;
 * class_mid a class_mid4 decoupent chaque classe en cinq bandes, une par
 * etage, chacune balayee a l'echelle de son etage.
 */
static void
sieve_segment(uint8_t *bits,
              uint64_t segment_first_index,
              uint64_t segment_bytes,
              uint64_t low,
              uint64_t high,
              const uint32_t *sorted,
              const uint32_t *turn,
              const size_t   *toff,
              const size_t *class_start,
              const size_t *class_mid,
              const size_t *class_mid2,
              const size_t *class_mid3,
              const size_t *class_mid4,
              const size_t *class_stop,
              size_t *active,
              wheel_cursor_t *cursor,
              const size_t *coff,
              uint64_t block_bytes,
              uint64_t chunk_bytes,
              uint64_t slab_bytes,
              uint64_t overshoot,
              uint64_t overshoot2,
              uint64_t overshoot3,
              bucket_ring_t *ring,
              uint64_t gw0,
              uint64_t chunk_windows,
              int *bucket_failed)
{
    const int64_t sink = (int64_t)segment_bytes;

    size_t was[8];

    for (unsigned rc = 0; rc < 8; rc++)
        was[rc] = active[rc];

    /* p * p <= high equivaut a p <= racine(high). */
    const uint64_t root_high = isqrt_u64(high);

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t a   = active[rc];
        size_t end = class_stop[rc];

        {
            size_t stop = end < class_mid4[rc] ? end : class_mid4[rc];

            while (a < stop)
            {
                uint32_t p = sorted[a];

                if ((uint64_t)p * p > high)
                    goto fini;

                activate_prime(p, segment_first_index, low,
                               &cursor[a - coff[rc]]);

                a++;
            }
        }

        /* Les premiers a seau actifs : ceux dont le carre atteint le
           segment. Une dichotomie, la ou un parcours relisait toute la
           classe. */
        a = upper_bound_u32(sorted, a, end, root_high);

    fini:;

        active[rc] = a;
    }

    if (ring)
    {
        for (unsigned rc = 0; rc < 8; rc++)
        {
            size_t from = was[rc] > class_mid4[rc] ? was[rc] : class_mid4[rc];

            if (from < active[rc] &&
                !bucket_activate(sorted, from, active[rc], rc, low,
                                 segment_first_index >> 3, ring, gw0,
                                 chunk_windows))
            {
                *bucket_failed = 1;
            }
        }
    }

    size_t mid[8];

    int blocked = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        mid[rc] = active[rc] < class_mid[rc] ? active[rc] : class_mid[rc];

        if (mid[rc] > class_start[rc])
            blocked = 1;
    }

    size_t mid2[8];

    int chunked = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        mid2[rc] = active[rc] < class_mid2[rc] ? active[rc] : class_mid2[rc];

        if (mid2[rc] > mid[rc])
            chunked = 1;
    }

    size_t mid4[8];

    for (unsigned rc = 0; rc < 8; rc++)
        mid4[rc] = active[rc] < class_mid4[rc] ? active[rc] : class_mid4[rc];

    size_t mid3[8];

    int slabbed = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        mid3[rc] = active[rc] < class_mid3[rc] ? active[rc] : class_mid3[rc];

        if (mid3[rc] > mid2[rc])
            slabbed = 1;
    }

    if (slabbed && slab_bytes)
    {
        sweep_slabbed(bits, segment_bytes, sorted, turn, toff, class_start, mid,
                      mid2, mid3, cursor, coff, block_bytes, chunk_bytes,
                      slab_bytes,
                      overshoot, overshoot2, overshoot3, sink, blocked);
    }
    else if (!chunked)
    {
        sweep_flat(bits, segment_bytes, sorted, turn, toff, class_start, mid,
                   cursor, coff, block_bytes, overshoot, sink, blocked);
    }
    else
    {
        sweep_chunked(bits, segment_bytes, sorted, turn, toff, class_start, mid,
                      mid2, cursor, coff, block_bytes, chunk_bytes, overshoot,
                      overshoot2, sink, blocked);
    }

    if (ring)
    {
        if (!sweep_bucketed(bits, segment_bytes, ring, gw0, chunk_windows))
            *bucket_failed = 1;
    }

#define SWEEP_REST(RC)                                        \
    for (size_t i = (slabbed && slab_bytes ? mid3[RC] : mid2[RC]);\
         i < mid4[RC]; i++)                                   \
    {                                                         \
        sweep_exact_calc(bits, &cursor[i - coff[RC]],         \
                         sorted[i], (int64_t)segment_bytes,   \
                         (int64_t)segment_bytes, sink, (RC)); \
    }

    SWEEP_REST(0) SWEEP_REST(1)
    SWEEP_REST(2) SWEEP_REST(3)
    SWEEP_REST(4) SWEEP_REST(5)
    SWEEP_REST(6) SWEEP_REST(7)

#undef SWEEP_REST

}


static uint64_t detect_cache_kb(int want_level)
{
#ifdef _SC_LEVEL2_CACHE_SIZE
    if (want_level == 2)
    {
        long bytes = sysconf(_SC_LEVEL2_CACHE_SIZE);

        if (bytes > 0)
            return (uint64_t)bytes / 1024ULL;
    }
#endif
#ifdef _SC_LEVEL3_CACHE_SIZE
    if (want_level == 3)
    {
        long bytes = sysconf(_SC_LEVEL3_CACHE_SIZE);

        if (bytes > 0)
            return (uint64_t)bytes / 1024ULL;
    }
#endif

    for (int index = 0; index < 10; index++)
    {
        char path[128];

        snprintf(path,
                 sizeof path,
                 "/sys/devices/system/cpu/cpu0/cache/index%d/level",
                 index);

        FILE *f = fopen(path, "r");

        if (!f)
            continue;

        int level = 0;

        int got = fscanf(f, "%d", &level);

        fclose(f);

        if (got != 1 || level != want_level)
            continue;

        snprintf(path,
                 sizeof path,
                 "/sys/devices/system/cpu/cpu0/cache/index%d/type",
                 index);

        f = fopen(path, "r");

        if (f)
        {
            char type[32] = { 0 };

            if (fscanf(f, "%31s", type) == 1 &&
                strcmp(type, "Unified") != 0 &&
                strcmp(type, "Data")    != 0)
            {
                fclose(f);
                continue;
            }

            fclose(f);
        }

        snprintf(path,
                 sizeof path,
                 "/sys/devices/system/cpu/cpu0/cache/index%d/size",
                 index);

        f = fopen(path, "r");

        if (!f)
            continue;

        unsigned long long value = 0;

        char unit = 0;

        got = fscanf(f, "%llu%c", &value, &unit);

        fclose(f);

        if (got < 1 || value == 0)
            continue;

        if (unit == 'M' || unit == 'm')
            return (uint64_t)value * 1024ULL;

        if (unit == 'K' || unit == 'k')
            return (uint64_t)value;

        return (uint64_t)value / 1024ULL;
    }

    return 0;
}


static unsigned cache_sharers(int want_level)
{
    for (int index = 0; index < 10; index++)
    {
        char path[128];

        snprintf(path,
                 sizeof path,
                 "/sys/devices/system/cpu/cpu0/cache/index%d/level",
                 index);

        FILE *f = fopen(path, "r");

        if (!f)
            break;

        int level = 0;

        if (fscanf(f, "%d", &level) != 1)
            level = 0;

        fclose(f);

        if (level != want_level)
            continue;

        snprintf(path,
                 sizeof path,
                 "/sys/devices/system/cpu/cpu0/cache/index%d/"
                 "shared_cpu_map",
                 index);

        f = fopen(path, "r");

        if (!f)
            break;

        unsigned bits = 0;

        for (int c = fgetc(f); c != EOF; c = fgetc(f))
        {
            int v = -1;

            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;

            if (v >= 0)
                bits += (unsigned)__builtin_popcount((unsigned)v);
        }

        fclose(f);

        return bits ? bits : 1;
    }

    return 1;
}

/*
 * Ce que la machine dit de ses caches, releve une fois : sysconf et sysfs
 * coutent des ouvertures de fichiers, et l'API dimensionne un crible a chaque
 * appel. Une valeur inconnue reste a 0.
 */
typedef struct
{
    long     l1d;
    long     l2;
    long     cpus;
    unsigned share1;
    unsigned share2;
    uint64_t l2_kb;
    uint64_t l3_kb;
} machine_t;

static machine_t g_machine;

static void machine_detect(void)
{
#ifdef _SC_LEVEL1_DCACHE_SIZE
    g_machine.l1d = sysconf(_SC_LEVEL1_DCACHE_SIZE);
#endif
#ifdef _SC_LEVEL2_CACHE_SIZE
    g_machine.l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
#endif
    g_machine.cpus = sysconf(_SC_NPROCESSORS_ONLN);

    g_machine.share1 = cache_sharers(1);
    g_machine.share2 = cache_sharers(2);

    g_machine.l2_kb = detect_cache_kb(2);
    g_machine.l3_kb = detect_cache_kb(3);
}

static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;

static void tables_build(void)
{
    build_wheel210();
    machine_detect();
}

static void tables_init(void)
{
    pthread_once(&g_tables_once, tables_build);
}

static uint64_t default_block_bytes(int threads)
{
    long l1 = g_machine.l1d;

    if (l1 > 0)
    {
        long cpus = g_machine.cpus;

        unsigned share = g_machine.share1;

        unsigned cores =
            (cpus > 0 && (unsigned long)cpus >= share)
                ? (unsigned)cpus / share
                : 1;

        unsigned per_l1 =
            ((unsigned)threads + cores - 1) / cores;

        if (per_l1 < 1)
            per_l1 = 1;

        if (per_l1 > share)
            per_l1 = share;

        uint64_t b = (uint64_t)l1 * 2 / 3 / per_l1;

        if (b < BLOCK_MIN_BYTES)
            b = BLOCK_MIN_BYTES;

        b = (b + 2048) / 4096 * 4096;

        if (b == 0)
            b = 4096;

        return b;
    }

    return 0;
}

#ifdef _OPENMP
/*
 * Nombre de threads que l'intervalle demande peut reellement occuper. Le
 * travail disponible est divise par la part minimale d'un thread ; en dessous
 * d'une part, un seul thread. Ne s'applique qu'au choix automatique.
 */
static int default_thread_count(int max_threads,
                                uint64_t span,
                                uint64_t root)
{
    uint64_t share = root / THREAD_SPAN_ROOT_DIV;

    if (share < MIN_THREAD_SPAN)
        share = MIN_THREAD_SPAN;

    uint64_t want = span / share;

    if (want < 1)
        want = 1;

    if (want > (uint64_t)max_threads)
        want = (uint64_t)max_threads;

    return (int)want;
}
#endif

static uint64_t default_chunk_bytes(int threads)
{
    long l2 = g_machine.l2;

    if (l2 > 0)
    {
        long cpus = g_machine.cpus;

        unsigned share = g_machine.share2;

        unsigned cores =
            (cpus > 0 && (unsigned long)cpus >= share)
                ? (unsigned)cpus / share
                : 1;

        unsigned per_l2 =
            ((unsigned)threads + cores - 1) / cores;

        if (per_l2 < 1)
            per_l2 = 1;

        if (per_l2 > share)
            per_l2 = share;

        uint64_t b = (uint64_t)l2 / per_l2 / 4;

        b = (b + 2048) / 4096 * 4096;

        return b;
    }

    return 0;
}

static uint64_t default_slab_bytes(int threads)
{
    long l2 = g_machine.l2;

    if (l2 > 0)
    {
        long cpus = g_machine.cpus;

        unsigned share = g_machine.share2;

        unsigned cores =
            (cpus > 0 && (unsigned long)cpus >= share)
                ? (unsigned)cpus / share
                : 1;

        unsigned per_l2 =
            ((unsigned)threads + cores - 1) / cores;

        if (per_l2 < 1)
            per_l2 = 1;

        if (per_l2 > share)
            per_l2 = share;

        uint64_t b = (uint64_t)l2 / per_l2;

        b = (b + 2048) / 4096 * 4096;

        return b;
    }

    return 0;
}

/* Premier index de [lo, hi) dont la valeur depasse v, dans un tableau
   croissant. */
static size_t upper_bound_u32(const uint32_t *a,
                              size_t lo,
                              size_t hi,
                              uint64_t v)
{
    while (lo < hi)
    {
        size_t m = lo + (hi - lo) / 2;

        if (a[m] <= v)
            lo = m + 1;
        else
            hi = m;
    }

    return lo;
}

/*
 * Les premiers de crible d'un plan sont huit plages croissantes de la base,
 * une par classe : [lo[rc], hi[rc]). Leur rang se compte de 1, du plus petit
 * au plus grand, toutes classes confondues.
 *
 * Nombre de premiers <= v.
 */
static uint64_t ranks_le(const uint32_t *sorted,
                         const size_t *lo,
                         const size_t *hi,
                         uint64_t v)
{
    uint64_t n = 0;

    for (unsigned rc = 0; rc < 8; rc++)
        n += upper_bound_u32(sorted, lo[rc], hi[rc], v) - lo[rc];

    return n;
}

/* Le premier de rang donne, entre 1 et le nombre de premiers. */
static uint32_t rank_value(const uint32_t *sorted,
                           const size_t *lo,
                           const size_t *hi,
                           uint64_t rank)
{
    uint64_t a = 0;
    uint64_t b = UINT32_MAX;

    while (a < b)
    {
        uint64_t m = a + (b - a) / 2;

        if (ranks_le(sorted, lo, hi, m) >= rank)
            b = m;
        else
            a = m + 1;
    }

    return (uint32_t)a;
}

/*
 * Taille de segment telle qu'un premier de la bande du milieu — ceux qui
 * depassent p_low — fasse en moyenne SEGMENT_MARKS_PER_ENTRY marques par
 * segment : il y fait 8s / p marques pour une seule entree de curseur.
 *
 * La somme des 1/p se prend par paquets de 16 premiers d'une meme classe,
 * chacun compte pour ses deux premiers du milieu. 1/p varie de moins de 1 %
 * dans un paquet et l'erreur est du second ordre, sous 10^-6 ; huit divisions
 * sur seize en moins, sur une somme qui en demandait une par premier a chaque
 * crible.
 */
#define AMORT_PACK 16

static uint64_t amortized_segment_bytes(const uint32_t *sorted,
                                        const size_t *lo,
                                        const size_t *hi,
                                        uint64_t p_low)
{
    double inv = 0.0;

    uint64_t count = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t i   = upper_bound_u32(sorted, lo[rc], hi[rc], p_low);
        size_t end = hi[rc];

        count += end - i;

        for (; i + AMORT_PACK <= end; i += AMORT_PACK)
        {
            inv += (AMORT_PACK / 2.0) / (double)sorted[i + AMORT_PACK / 2 - 1]
                 + (AMORT_PACK / 2.0) / (double)sorted[i + AMORT_PACK / 2];
        }

        for (; i < end; i++)
            inv += 1.0 / (double)sorted[i];
    }

    if (!count || !(inv > 0.0))
        return 0;

    double s =
        (double)count * SEGMENT_MARKS_PER_ENTRY / (8.0 * inv);

    if (!(s > 0.0))
        return 0;

    return (uint64_t)s;
}

static uint64_t isqrt_u64(uint64_t n)
{
    uint64_t root = (uint64_t)sqrt((double)n);

    while ((root + 1) <= n / (root + 1))
        root++;

    while (root && root > n / root)
        root--;

    return root;
}

/*
 * Reglages d'etalonnage, ceux que la ligne de commande peut imposer. Tout a
 * zero, hors presieve_max et slab_mult, donne le dimensionnement automatique.
 */
typedef struct
{
    uint64_t segment_kb;

    /* Plancher du segment automatique, ignore si segment_kb est impose. */
    uint64_t segment_min_kb;

    uint64_t chunk_override;

    uint32_t presieve_max;

    uint64_t block_kb;
    int      block_set;
    uint64_t block_max;

    uint64_t chunk_kb;
    int      chunk_set;

    uint64_t slab_kb;
    int      slab_set;
    uint64_t slab_mult;

    uint64_t bucket_kb;
    int      bucket_set;
    uint64_t bucket_mult;
} tuning_t;

/*
 * Le plan d'un crible de [low_limit, limit] : dans la base, la plage de
 * chaque classe et son decoupage en etages, les tailles retenues, le
 * decoupage en chunks. Construit une fois, puis lu seulement — les threads se
 * le partagent. La base ne lui appartient pas.
 */
typedef struct
{
    uint64_t low_limit;
    uint64_t limit;

    int threads;

    /* base->sorted, et dans chaque classe la plage du plan : les premiers
       de ]presieve_max, racine(limit)]. Les index sont ceux de la base. */
    const uint32_t *sorted;

    size_t class_start[8];
    size_t class_mid[8];
    size_t class_mid2[8];
    size_t class_mid3[8];
    size_t class_mid4[8];
    size_t class_stop[8];
    size_t toff[8];

    /* Les curseurs ne servent qu'aux premiers hors seaux, et sont ranges
       sans trou : celui du premier d'index i de la classe rc est en
       i - coff[rc]. Les index de la base, eux, ont un trou entre deux
       classes des que la base depasse la racine du plan. */
    size_t coff[8];
    size_t cursor_count;

    uint32_t *turn;

    uint64_t    segment_kb;
    uint64_t    segment_bytes;
    uint64_t    segment_bits;
    const char *segment_origin;

    uint64_t block_bytes;
    uint64_t chunk_bytes;
    uint64_t slab_bytes;
    uint64_t bucket_bytes;

    /* Premiers de chaque etage a balayage par blocs. */
    uint64_t n_block;
    uint64_t n_chunk;
    uint64_t n_slab;

    uint32_t p_block;
    uint32_t p_chunk;
    uint32_t p_slab;
    uint32_t p_bucket;

    uint64_t overshoot;
    uint64_t overshoot2;
    uint64_t overshoot3;

    unsigned bucket_shift;
    uint64_t bucket_primes;
    uint64_t ring_slots;

    /* Premiers absents du bitset : 2, 3, 5 et ceux du pre-crible. */
    uint64_t small_total;

    uint64_t total_candidates;
    uint64_t sieve_first;
    uint64_t skip_bits;

    uint64_t chunk_segments;
    uint64_t chunk_count;
    uint64_t chunk_candidates;
} plan_t;

static void plan_free(plan_t *pl)
{
    free(pl->turn);

    pl->turn = NULL;
}

/*
 * Dimensionne le crible de [low_limit, limit] sur une base qui couvre au
 * moins racine(limit). Le pre-crible doit deja etre construit pour
 * tu->presieve_max.
 *
 * threads est le nombre de threads qui cribleront, il regle le decoupage en
 * chunks ; sizing_threads est le nombre de cribles qui tournent en meme temps
 * sur la machine, il regle le partage des caches. La ligne de commande passe
 * deux fois la meme valeur ; l'API, quand elle crible sur le thread de son
 * appelant, passe 1 et le nombre de threads de l'application.
 *
 * Rien ici ne parcourt les premiers un a un : les bandes se trouvent par
 * dichotomie dans chaque classe, pour qu'un plan sur une base deja construite
 * ne coute presque rien.
 *
 * Rend 0 si une allocation echoue.
 */
static int plan_build(plan_t *pl,
                      const base_t *base,
                      uint64_t low_limit,
                      uint64_t limit,
                      const tuning_t *tu,
                      int threads,
                      int sizing_threads)
{
    memset(pl, 0, sizeof *pl);

    tables_init();

    const uint32_t presieve_max = tu->presieve_max;

    uint64_t segment_kb = tu->segment_kb;

    const char *segment_origin = "-s";

    uint64_t segment_bytes = 0;
    uint64_t segment_bits  = 0;

    const uint64_t root = isqrt_u64(limit);

    const uint32_t *const sorted = base->sorted;

    /* La plage de chaque classe : au-dela du pre-crible, jusqu'a la racine. */
    size_t class_start[8];
    size_t class_stop[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        class_start[rc] = upper_bound_u32(sorted, base->cls[rc],
                                          base->cls[rc + 1], presieve_max);
        class_stop[rc]  = upper_bound_u32(sorted, class_start[rc],
                                          base->cls[rc + 1], root);
    }

#define RANKS_LE(v)   ranks_le(sorted, class_start, class_stop, (v))
#define RANK_VALUE(r) rank_value(sorted, class_start, class_stop, (r))

    /* Nombre de premiers de crible. e_block <= e_chunk <= e_slab sont les
       rangs ou finissent les trois premiers etages. */
    const uint64_t sorted_count = RANKS_LE(UINT32_MAX);

    uint64_t block_bytes =
        tu->block_set ? tu->block_kb * 1024ULL
                      : default_block_bytes(sizing_threads);

    uint64_t e_block = 0;

    if (block_bytes)
    {
        if (tu->block_max)
        {
            e_block = tu->block_max;

            if (e_block > sorted_count)
                e_block = sorted_count;
        }
        else
        {
            e_block = RANKS_LE(block_bytes / BLOCK_MIN_TURNS);
        }

        if (e_block == 0)
            block_bytes = 0;
    }

    uint64_t chunk_bytes =
        tu->chunk_set ? tu->chunk_kb * 1024ULL
                      : default_chunk_bytes(sizing_threads);

    uint64_t e_chunk = e_block;

    if (chunk_bytes <= block_bytes)
        chunk_bytes = 0;

    if (chunk_bytes)
    {
        uint64_t e = RANKS_LE(chunk_bytes);

        if (e > e_chunk)
            e_chunk = e;

        if (e_chunk <= e_block)
            chunk_bytes = 0;
    }

    uint32_t p_chunk = chunk_bytes ? RANK_VALUE(e_chunk) : 0;

    /* Marge d'un tour complet, celle qu'exige sweep_over. */
    uint64_t overshoot2 =
        chunk_bytes ? (uint64_t)p_chunk * 29 / 30 + 1 : 0;

    uint64_t slab_bytes =
        tu->slab_set ? tu->slab_kb * 1024ULL
                     : default_slab_bytes(sizing_threads);

    uint64_t e_slab = e_chunk;

    if (!chunk_bytes || slab_bytes <= chunk_bytes)
        slab_bytes = 0;

    if (slab_bytes)
    {
        uint64_t e = RANKS_LE(slab_bytes * tu->slab_mult);

        if (e > e_slab)
            e_slab = e;

        if (e_slab <= e_chunk)
            slab_bytes = 0;
    }

    /*
       La plaque peut couvrir jusqu'a sqrt(N) : la bande directe est alors
       vide, mais la plaque reste utile. L'eteindre renvoyait ses premiers
       sur le segment entier, deux fois la part de L2 du thread : +1,5 % a
       10^11 (707 ms contre 691 pour ddd_multi4 a plaque conservee, le
       2026-09-07). Le segment se dimensionne alors sur le L2, plus bas.
    */

    uint32_t p_slab = slab_bytes ? RANK_VALUE(e_slab) : 0;

    uint64_t overshoot3 =
        slab_bytes ? (uint64_t)p_slab * 29 / 30 + 1 : 0;

    if (segment_kb == 0)
    {
        /* La bande du milieu : tout ce qui suit les trois premiers etages. */
        uint64_t s =
            amortized_segment_bytes(sorted, class_start, class_stop,
                                    e_slab ? RANK_VALUE(e_slab) : 0);

        if (s)
        {
            segment_origin = "amorti auto";
        }
        else
        {
            uint64_t l2_kb = g_machine.l2_kb;

            if (l2_kb >= MIN_SEGMENT_KB &&
                l2_kb <= MAX_SEGMENT_KB)
            {
                if (slab_bytes)
                {
                    /* la plaque vaut la part de L2 du thread : le
                       segment en porte autant que de threads partageant
                       ce L2, deux sur le 9700X, comme ddd_multi4 */
                    s = l2_kb * 1024ULL;
                    segment_origin = "L2, chemin direct vide (plaque)";
                }
                else
                {
                    s = l2_kb * 1024ULL / 2;
                    segment_origin = "L2/2, chemin direct vide";
                }
            }
            else
            {
                s = FALLBACK_SEGMENT_KB * 1024ULL;
                segment_origin = "defaut, chemin direct vide";
            }
        }

        {
            uint64_t l3_kb = g_machine.l3_kb;

            if (l3_kb && sizing_threads > 0)
            {
                uint64_t share = slab_bytes ? 1ULL : 2ULL;

                uint64_t cap =
                    l3_kb * 1024ULL / share / (uint64_t)sizing_threads;

                if (cap < MIN_SEGMENT_KB * 1024ULL)
                    cap = MIN_SEGMENT_KB * 1024ULL;

                if (s > cap)
                {
                    s = cap;

                    segment_origin =
                        slab_bytes ? "plafond L3 par thread (plaque)"
                                   : "plafond L3/2 par thread";
                }
            }
        }

        if (block_bytes)
        {
            uint64_t k =
                (s + block_bytes / 2) / block_bytes;

            if (k == 0)
                k = 1;

            s = k * block_bytes;
        }

        segment_kb = s / 1024;

        if (segment_kb < tu->segment_min_kb)
            segment_kb = tu->segment_min_kb;

        if (segment_kb < MIN_SEGMENT_KB)
            segment_kb = MIN_SEGMENT_KB;

        if (segment_kb > MAX_SEGMENT_KB)
            segment_kb = MAX_SEGMENT_KB;
    }

    segment_bytes = segment_kb * 1024ULL;
    segment_bits  = segment_bytes * 8ULL;

    if (block_bytes > segment_bytes)
    {
        block_bytes = segment_bytes;

        if (!tu->block_max)
            e_block = RANKS_LE(block_bytes / BLOCK_MIN_TURNS);

        if (e_block == 0)
            block_bytes = 0;
    }

    /* La fenetre de seau par defaut suit la tranche telle que les caches la
       donnent, avant que le segment ne la borne ci-dessous. */
    const uint64_t bucket_default = chunk_bytes ? chunk_bytes : slab_bytes;

    /*
     * Un etage ne balaie pas plus large que le segment, et sa bande suit sa
     * fenetre. Un premier dont le tour depasse la fenetre y serait parcouru
     * a chaque passage pour moins d'un tour de marques : c'est le travail de
     * la bande directe et des seaux. Mesure a [10^12, +1,6e7] en segments de
     * 32 Kio, la plaque gardait 38 229 premiers de tour jusqu'a 640 Kio et le
     * criblage coutait 7,5 ms ; sans elle, 5,0 ms.
     *
     * Ne mord qu'avec un segment plus petit que la plaque : un -s explicite,
     * ou un L3 etroit par thread.
     */
    if (chunk_bytes > segment_bytes)
    {
        chunk_bytes = segment_bytes;
        e_chunk     = e_block;

        if (chunk_bytes > block_bytes)
        {
            uint64_t e = RANKS_LE(chunk_bytes);

            if (e > e_chunk)
                e_chunk = e;
        }

        if (e_chunk <= e_block)
            chunk_bytes = 0;

        p_chunk = chunk_bytes ? RANK_VALUE(e_chunk) : 0;

        overshoot2 =
            chunk_bytes ? (uint64_t)p_chunk * 29 / 30 + 1 : 0;
    }

    if (slab_bytes > segment_bytes)
    {
        slab_bytes = segment_bytes;
        e_slab     = e_chunk;

        if (chunk_bytes && slab_bytes > chunk_bytes)
        {
            uint64_t e = RANKS_LE(slab_bytes * tu->slab_mult);

            if (e > e_slab)
                e_slab = e;
        }

        if (e_slab <= e_chunk)
            slab_bytes = 0;

        p_slab = slab_bytes ? RANK_VALUE(e_slab) : 0;

        overshoot3 =
            slab_bytes ? (uint64_t)p_slab * 29 / 30 + 1 : 0;
    }

    uint32_t p_block =
        block_bytes ? RANK_VALUE(e_block) : 0;

    uint64_t overshoot =
        block_bytes
            ? (uint64_t)p_block * 29 / 30 + 1
            : 0;

    /* Dans chaque classe, la fin de chaque etage. Un etage eteint a un seuil
       nul et ne prend rien. */
    size_t class_mid[8];
    size_t class_mid2[8];
    size_t class_mid3[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        class_mid[rc] =
            upper_bound_u32(sorted, class_start[rc], class_stop[rc], p_block);

        class_mid2[rc] =
            upper_bound_u32(sorted, class_mid[rc], class_stop[rc], p_chunk);

        class_mid3[rc] =
            upper_bound_u32(sorted, class_mid2[rc], class_stop[rc], p_slab);
    }

    uint64_t bucket_bytes = tu->bucket_set ? tu->bucket_kb * 1024
                          : bucket_default;

    if (bucket_bytes > segment_bytes)
        bucket_bytes = segment_bytes;

    if (bucket_bytes > BUCKET_WINDOW_MAX_BYTES)
        bucket_bytes = BUCKET_WINDOW_MAX_BYTES;

    if (bucket_bytes)
    {
        uint64_t pow2 = 1;

        while (pow2 * 2 <= bucket_bytes)
            pow2 *= 2;

        bucket_bytes = pow2;

        while (bucket_bytes && segment_bytes % bucket_bytes)
            bucket_bytes >>= 1;
    }

    unsigned bucket_shift = 0;

    for (uint64_t v = bucket_bytes; v > 1; v >>= 1)
        bucket_shift++;

    if (!sorted_count)
        bucket_bytes = 0;

    int wheel210 = (presieve_max >= 7 && g_pre_passes);

    if (!wheel210)
        bucket_bytes = 0;

    uint64_t p_bucket_want =
        tu->bucket_mult ? bucket_bytes * tu->bucket_mult
                        : segment_bytes * 5 / 2;

    uint32_t p_bucket =
        bucket_bytes && p_bucket_want < UINT32_MAX
            ? (uint32_t)p_bucket_want
            : 0;

    size_t class_mid4[8];

    uint64_t bucket_primes = 0;

    size_t coff[8];

    size_t cursor_count = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        class_mid4[rc] =
            p_bucket ? upper_bound_u32(sorted, class_mid3[rc],
                                       class_stop[rc], p_bucket)
                     : class_stop[rc];

        bucket_primes += class_stop[rc] - class_mid4[rc];

        coff[rc]      = class_start[rc] - cursor_count;
        cursor_count += class_mid4[rc] - class_start[rc];
    }

    size_t tbase[8];

    size_t turn_count = 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        tbase[rc]   = turn_count;
        turn_count += class_mid3[rc] - class_start[rc];
    }

    size_t toff[8];

    for (unsigned rc = 0; rc < 8; rc++)
        toff[rc] = class_start[rc] - tbase[rc];

    uint32_t *turn =
        malloc((turn_count ? turn_count : 1)
               * TURN_SLOTS * sizeof *turn);

    if (!turn)
        return 0;

    for (unsigned rc = 0; rc < 8; rc++)
    {
        build_turn_offsets(sorted + class_start[rc],
                           class_mid3[rc] - class_start[rc],
                           turn + tbase[rc] * TURN_SLOTS);
    }

    uint64_t small_total = 0;

    if (2 >= low_limit && 2 <= limit) small_total++;
    if (3 >= low_limit && 3 <= limit) small_total++;
    if (5 >= low_limit && 5 <= limit) small_total++;

    for (int i = 0; i < g_pre_count; i++)
    {
        if (g_pre_primes[i] >= low_limit &&
            g_pre_primes[i] <= limit)
        {
            small_total++;
        }
    }

    uint64_t total_candidates =
        wheel_count(limit);

    uint64_t first_candidate =
        low_limit > 1 ? wheel_count(low_limit - 1) : 0;

    /* Le crible demarre sur une frontiere d'octet ; les bits candidats situes
       sous low_limit sont effaces apres coup. */
    uint64_t sieve_first = first_candidate & ~(uint64_t)7;

    uint64_t skip_bits = first_candidate - sieve_first;

    uint64_t work_candidates = total_candidates - sieve_first;

    uint64_t total_segments =
        (work_candidates + segment_bits - 1) / segment_bits;

    uint64_t chunk_segments;

    if (tu->chunk_override)
    {
        /* Plafonne a la plage entiere : au-dela, chunk_segments *
           segment_bits deborde et chunk_candidates retombe sous la
           taille reelle, ne criblant qu'une partie du domaine. */
        chunk_segments =
            tu->chunk_override < total_segments ? tu->chunk_override
                                                : total_segments;

        if (chunk_segments == 0)
            chunk_segments = 1;
    }
    else
    {
        uint64_t target =
            (uint64_t)threads * CHUNKS_PER_THREAD;

        chunk_segments = (total_segments + target - 1) / target;

        if (chunk_segments == 0)
            chunk_segments = 1;

        /* Sans premier a seau il n'y a rien a reposer : le decoupage ne
           bouge pas. La contrainte ne mord donc que sur les intervalles
           etroits a borne haute, ou la repose domine le criblage. */
        if (bucket_primes)
        {
            uint64_t need =
                (bucket_primes * BUCKET_AMORT + segment_bits - 1)
                / segment_bits;

            uint64_t floor_target =
                (uint64_t)threads * MIN_CHUNKS_PER_THREAD;

            uint64_t cap =
                (total_segments + floor_target - 1) / floor_target;

            if (cap == 0)
                cap = 1;

            if (need > cap)
                need = cap;

            if (need > chunk_segments)
                chunk_segments = need;
        }
    }

    uint64_t chunk_count =
        (total_segments + chunk_segments - 1) / chunk_segments;

    uint64_t chunk_candidates = chunk_segments * segment_bits;

    uint64_t ring_slots = 1;

    if (bucket_bytes)
    {
        /* L'anneau est indexe depuis le debut du segment, sa fente 0, et
           doit couvrir deux portees. Le saut d'un premier entre deux marques
           d'abord : l'ecart maximal des residus de la roue 210 vaut 10, soit
           p / 3 octets, que 2p majore largement. L'activation ensuite, qui
           pose la premiere marque en p * p, donc n'importe ou dans le
           segment. */
        uint64_t need =
            (segment_bytes + 2 * (uint64_t)RANK_VALUE(sorted_count))
            / bucket_bytes + 4;

        uint64_t cap_windows =
            (chunk_candidates / 8 + bucket_bytes - 1) / bucket_bytes + 2;

        if (need > cap_windows)
            need = cap_windows;

        while (ring_slots < need)
            ring_slots <<= 1;
    }

#undef RANKS_LE
#undef RANK_VALUE

    pl->low_limit = low_limit;
    pl->limit     = limit;
    pl->threads   = threads;

    pl->sorted = sorted;

    memcpy(pl->class_start, class_start, sizeof class_start);
    memcpy(pl->class_mid,   class_mid,   sizeof class_mid);
    memcpy(pl->class_mid2,  class_mid2,  sizeof class_mid2);
    memcpy(pl->class_mid3,  class_mid3,  sizeof class_mid3);
    memcpy(pl->class_mid4,  class_mid4,  sizeof class_mid4);
    memcpy(pl->class_stop,  class_stop,  sizeof class_stop);
    memcpy(pl->toff,        toff,        sizeof toff);

    memcpy(pl->coff, coff, sizeof coff);

    pl->cursor_count = cursor_count;

    pl->turn = turn;

    pl->segment_kb     = segment_kb;
    pl->segment_bytes  = segment_bytes;
    pl->segment_bits   = segment_bits;
    pl->segment_origin = segment_origin;

    pl->block_bytes  = block_bytes;
    pl->chunk_bytes  = chunk_bytes;
    pl->slab_bytes   = slab_bytes;
    pl->bucket_bytes = bucket_bytes;

    pl->n_block = e_block;
    pl->n_chunk = e_chunk - e_block;
    pl->n_slab  = e_slab - e_chunk;

    pl->p_block  = p_block;
    pl->p_chunk  = p_chunk;
    pl->p_slab   = p_slab;
    pl->p_bucket = p_bucket;


    pl->overshoot  = overshoot;
    pl->overshoot2 = overshoot2;
    pl->overshoot3 = overshoot3;

    pl->bucket_shift  = bucket_shift;
    pl->bucket_primes = bucket_primes;
    pl->ring_slots    = ring_slots;

    pl->small_total = small_total;

    pl->total_candidates = total_candidates;
    pl->sieve_first      = sieve_first;
    pl->skip_bits        = skip_bits;

    pl->chunk_segments   = chunk_segments;
    pl->chunk_count      = chunk_count;
    pl->chunk_candidates = chunk_candidates;

    return 1;
}

/* Ce qu'un thread possede en propre : son bitset, ses curseurs, ses seaux. */
typedef struct
{
    uint8_t        *bits;
    wheel_cursor_t *cursor;
    bucket_ring_t   ring;
} worker_t;

static int worker_init(const plan_t *pl, worker_t *w)
{
    w->bits =
        aligned_alloc(64, pl->segment_bytes + 64);

    w->cursor =
        malloc((pl->cursor_count ? pl->cursor_count : 1) * sizeof *w->cursor);

    memset(&w->ring, 0, sizeof w->ring);

    w->ring.slots = pl->ring_slots;
    w->ring.win   = pl->bucket_bytes;
    w->ring.shift = pl->bucket_shift;

    /* Deux residus consecutifs de la roue 210 sont distants d'au moins
       2 : le pas d'un premier a seau vaut au moins 2 * (p / 30) octets. */
    w->ring.one   = 2 * (uint64_t)(pl->p_bucket / 30) >= pl->bucket_bytes;

    if (pl->bucket_bytes)
        w->ring.cur = calloc((size_t)pl->ring_slots, sizeof *w->ring.cur);

    return w->bits && w->cursor && (!pl->bucket_bytes || w->ring.cur);
}

static void worker_free(worker_t *w)
{
    free(w->bits);
    free(w->cursor);
    bucket_ring_free(&w->ring);

    w->bits   = NULL;
    w->cursor = NULL;
}

/*
 * Ce qu'on fait d'un segment crible. bits porte nbits candidats a partir de
 * l'index first, toujours multiple de 8 ; les bits sous low_limit sont deja
 * effaces, ceux au-dela de nbits sont indetermines. Rend 0 pour arreter.
 */
typedef int (*segment_fn)(void *ctx,
                          const uint8_t *bits,
                          uint64_t first,
                          uint64_t nbits);

/*
 * Crible le chunk c, segment par segment. Un chunk peut demarrer n'importe
 * ou : il reactive ses curseurs et repose ses seaux. Rend 0 si visit a
 * demande l'arret.
 */
static int chunk_run(const plan_t *pl,
                     worker_t *w,
                     uint64_t c,
                     segment_fn visit,
                     void *ctx,
                     int *bucket_failed)
{
    const uint64_t segment_bits = pl->segment_bits;
    const uint64_t bucket_bytes = pl->bucket_bytes;

    uint8_t *const bits = w->bits;

    bucket_ring_t *const ring = w->ring.cur ? &w->ring : NULL;

    uint64_t chunk_first =
        pl->sieve_first + c * pl->chunk_candidates;

    uint64_t chunk_end =
        chunk_first + pl->chunk_candidates;

    if (chunk_end > pl->total_candidates)
        chunk_end = pl->total_candidates;

    size_t active[8];

    for (unsigned rc = 0; rc < 8; rc++)
        active[rc] = pl->class_start[rc];

    uint64_t chunk_windows = 0;

    if (ring)
    {
        for (uint64_t i = 0; i < pl->ring_slots; i++)
        {
            bucket_entry_t *p = ring->cur[i];

            ring->cur[i] = NULL;

            for (bucket_block_t *b = p ? BUCKET_BLOCK_OF(p) : NULL;
                 b; )
            {
                bucket_block_t *prev = b->next;

                b->next     = ring->stock;
                ring->stock = b;
                b = prev;
            }
        }

        chunk_windows =
            ((chunk_end - chunk_first + 7) / 8 + bucket_bytes - 1)
            / bucket_bytes;
    }

    uint64_t segment_first = chunk_first;

    while (segment_first < chunk_end)
    {
        uint64_t segment_last =
            segment_first + segment_bits - 1;

        if (segment_last >= chunk_end)
            segment_last = chunk_end - 1;

        uint64_t low =
            index_to_number(segment_first);

        uint64_t high =
            index_to_number(segment_last);

        if (high > pl->limit)
            high = pl->limit;

        uint64_t actual_bits =
            segment_last - segment_first + 1;

        uint64_t actual_bytes =
            (actual_bits + 7) >> 3;

        if (g_pre_passes)
        {
            presieve_fill(bits,
                          actual_bytes,
                          segment_first >> 3);
        }
        else
        {
            memset(bits, 0xFF, actual_bytes);
        }

        sieve_segment(
            bits,
            segment_first,
            actual_bytes,
            low,
            high,
            pl->sorted,
            pl->turn,
            pl->toff,
            pl->class_start,
            pl->class_mid,
            pl->class_mid2,
            pl->class_mid3,
            pl->class_mid4,
            pl->class_stop,
            active,
            w->cursor,
            pl->coff,
            pl->block_bytes,
            pl->chunk_bytes,
            pl->slab_bytes,
            pl->overshoot,
            pl->overshoot2,
            pl->overshoot3,
            ring,
            ((segment_first - chunk_first) / 8) / (bucket_bytes ?
                                                   bucket_bytes : 1),
            chunk_windows,
            bucket_failed
        );

        if (segment_first == pl->sieve_first)
        {
            for (uint64_t b = 0; b < pl->skip_bits; b++)
                clear_bit(bits, b);
        }

        if (segment_first == 0)
        {
            clear_bit(bits, 0);
        }

        if (!visit(ctx, bits, segment_first, actual_bits))
            return 0;

        segment_first = segment_last + 1;
    }

    return 1;
}

static int visit_count(void *ctx,
                       const uint8_t *bits,
                       uint64_t first,
                       uint64_t nbits)
{
    (void)first;

    *(uint64_t *)ctx += count_set_bits(bits, nbits);

    return 1;
}

/*
 * Compte les bits restes a 1 sur tout le plan, les chunks distribues entre
 * pl->threads threads. Rend 0 si une allocation echoue.
 */
static int engine_count(const plan_t *pl, uint64_t *out)
{
    int alloc_failed = 0;

    uint64_t found = 0;

#ifdef _OPENMP
#   pragma omp parallel num_threads(pl->threads) reduction(+:found)
#endif
    {
        worker_t w;

        uint64_t mine = 0;

        int bucket_failed = 0;

        if (!worker_init(pl, &w))
        {
#ifdef _OPENMP
#           pragma omp atomic write
#endif
            alloc_failed = 1;
        }

#ifdef _OPENMP
#       pragma omp for schedule(dynamic, 1)
#endif
        for (int64_t c = 0; c < (int64_t)pl->chunk_count; c++)
        {
            int stop;

#ifdef _OPENMP
#           pragma omp atomic read
#endif
            stop = alloc_failed;

            if (stop)
                continue;

            chunk_run(pl, &w, (uint64_t)c, visit_count, &mine,
                      &bucket_failed);

            if (bucket_failed)
            {
#ifdef _OPENMP
#               pragma omp atomic write
#endif
                alloc_failed = 1;
            }
        }

        found += mine;

        worker_free(&w);
    }

    *out = found;

    return !alloc_failed;
}

/*
 * Parcourt le plan dans l'ordre, sur le thread appelant, et remet chaque
 * segment a visit. Rend 0 si une allocation echoue ; un arret demande par
 * visit n'est pas un echec.
 */
static int engine_walk(const plan_t *pl, segment_fn visit, void *ctx)
{
    worker_t w;

    int bucket_failed = 0;

    int ok = worker_init(pl, &w);

    for (uint64_t c = 0; ok && c < pl->chunk_count; c++)
    {
        int more = chunk_run(pl, &w, c, visit, ctx, &bucket_failed);

        if (bucket_failed)
            ok = 0;

        if (!more)
            break;
    }

    worker_free(&w);

    return ok;
}


/*
 * L'API de roue12.h. Le pre-crible y est toujours celui par defaut, construit
 * au premier appel et garde jusqu'a la fin du processus.
 */
static const tuning_t tuning_default =
{
    .presieve_max = PRESIEVE_DEFAULT_MAX,
    .slab_mult    = 1,
};

static unsigned g_api_threads;

/*
 * Le cache de l'API : une base, la plus large demandee jusqu'ici, que tous
 * les appels et tous les threads se partagent. Un programme qui crible par
 * morceaux refaisait la meme base a chaque appel ; pres de 10^16 c'etait un
 * quart du temps de crible_count_points.
 *
 * Deux verrous. g_base_mu garde le pointeur et les comptes de reference, le
 * temps de quelques instructions. g_build_mu met les constructions en file :
 * un appel que la base courante sert deja n'attend pas celle qu'un autre
 * agrandit.
 *
 * La base grandit par bonds de moitie, pour qu'une suite de morceaux a bornes
 * croissantes ne la refasse pas a chaque pas ; elle ne depasse pas
 * racine(10^16), soit 5,76 millions de premiers et 23 Mo. Elle n'est jamais
 * rendue : le cache garde sa reference jusqu'a la fin du processus.
 */
#define BASE_ROOT_MAX 100000000ULL
#define BASE_ROOT_MIN 65536ULL

static pthread_mutex_t g_base_mu  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_build_mu = PTHREAD_MUTEX_INITIALIZER;

static base_t *g_base;

/* La base courante si elle couvre root, une reference de plus. */
static base_t *base_try(uint64_t root)
{
    base_t *b = NULL;

    pthread_mutex_lock(&g_base_mu);

    if (g_base && g_base->root >= root)
    {
        b = g_base;
        b->refs++;
    }

    pthread_mutex_unlock(&g_base_mu);

    return b;
}

static void base_release(const base_t *held)
{
    base_t *b = (base_t *)held;

    pthread_mutex_lock(&g_base_mu);

    int dead = (--b->refs == 0);

    pthread_mutex_unlock(&g_base_mu);

    if (dead)
        base_free(b);
}

/* Une base qui couvre root, a rendre par base_release. NULL si une
   allocation echoue. */
static const base_t *base_acquire(uint64_t root)
{
    base_t *b = base_try(root);

    if (b)
        return b;

    pthread_mutex_lock(&g_build_mu);

    /* Un autre thread a pu la construire pendant l'attente. */
    b = base_try(root);

    if (!b)
    {
        uint64_t want = root + root / 2;

        if (want < BASE_ROOT_MIN)
            want = BASE_ROOT_MIN;

        if (want > BASE_ROOT_MAX)
            want = BASE_ROOT_MAX;

        if (want < root)
            want = root;

        b = base_build(want);

        /* Faute de memoire pour la marge, la base au plus juste. */
        if (!b && want > root)
            b = base_build(root);

        if (b)
        {
            /* Une reference pour le cache, une pour l'appelant. */
            b->refs = 2;

            pthread_mutex_lock(&g_base_mu);

            base_t *old = g_base;

            g_base = b;

            int dead = (old && --old->refs == 0);

            pthread_mutex_unlock(&g_base_mu);

            if (dead)
                base_free(old);
        }
    }

    pthread_mutex_unlock(&g_build_mu);

    return b;
}

static pthread_once_t g_api_once = PTHREAD_ONCE_INIT;

static int g_api_ready;

static void api_build(void)
{
    double want = 0.0;

    tables_init();

    g_api_ready = (presieve_build(PRESIEVE_DEFAULT_MAX, &want) == 0);
}

static int api_init(void)
{
    pthread_once(&g_api_once, api_build);

    return g_api_ready;
}

void crible_set_threads(unsigned n)
{
    g_api_threads = n;
}

unsigned crible_get_threads(void)
{
    tables_init();

    unsigned cpus = g_machine.cpus > 0 ? (unsigned)g_machine.cpus : 1u;

    return (g_api_threads == 0 || g_api_threads > cpus) ? cpus
                                                        : g_api_threads;
}

crible_status crible_count_primes(uint64_t start,
                                  uint64_t stop,
                                  uint64_t *result)
{
    *result = 0;

    if (start > stop)
        return CRIBLE_OK;

    if (stop > MAX_LIMIT)
        return CRIBLE_EUNSUPPORTED;

    if (!api_init())
        return CRIBLE_ENOMEM;

    int threads = 1;

#ifdef _OPENMP
    threads = default_thread_count((int)crible_get_threads(),
                                   stop - start + 1,
                                   isqrt_u64(stop));
#endif

    const base_t *base = base_acquire(isqrt_u64(stop));

    if (!base)
        return CRIBLE_ENOMEM;

    plan_t pl;

    if (!plan_build(&pl, base, start, stop, &tuning_default, threads,
                    threads))
    {
        base_release(base);

        return CRIBLE_ENOMEM;
    }

    uint64_t found = 0;

    int ok = engine_count(&pl, &found);

    if (ok)
        *result = pl.small_total + found;

    plan_free(&pl);
    base_release(base);

    return ok ? CRIBLE_OK : CRIBLE_ENOMEM;
}

/* Premiers absents du bitset — 2, 3, 5 et ceux du pre-crible — dans
   [lo, hi]. out recoit ceux trouves s'il n'est pas NULL. */
#define SMALL_PRIMES_MAX \
    (3 + sizeof g_pre_primes / sizeof g_pre_primes[0])

static size_t small_primes_in(uint64_t lo, uint64_t hi, uint64_t *out)
{
    size_t n = 0;

    for (uint64_t p = 2; p <= 5; p++)
    {
        if (p != 4 && p >= lo && p <= hi)
        {
            if (out)
                out[n] = p;

            n++;
        }
    }

    for (int i = 0; i < g_pre_count; i++)
    {
        uint64_t p = g_pre_primes[i];

        if (p >= lo && p <= hi)
        {
            if (out)
                out[n] = p;

            n++;
        }
    }

    return n;
}

/* Bits a 1 de la plage [a, b) du bitset. */
static uint64_t count_bits_range(const uint8_t *bits,
                                 uint64_t a,
                                 uint64_t b)
{
    if (a >= b)
        return 0;

    const uint64_t wa = a >> 6;
    const uint64_t wb = (b - 1) >> 6;

    const uint64_t head = ~0ULL << (a & 63);
    const uint64_t tail = ~0ULL >> (63 - ((b - 1) & 63));

    uint64_t w;

    memcpy(&w, bits + 8 * wa, sizeof w);

    if (wa == wb)
        return (uint64_t)__builtin_popcountll(w & head & tail);

    uint64_t count = (uint64_t)__builtin_popcountll(w & head);

    for (uint64_t k = wa + 1; k < wb; k++)
    {
        memcpy(&w, bits + 8 * k, sizeof w);

        count += (uint64_t)__builtin_popcountll(w);
    }

    memcpy(&w, bits + 8 * wb, sizeof w);

    return count + (uint64_t)__builtin_popcountll(w & tail);
}

typedef struct
{
    const uint64_t *points;
    uint64_t       *counts;
    size_t          n;
    size_t          i;
    uint64_t        run;
} points_ctx_t;

/*
 * run porte les premiers du bitset deja passes. Un point se regle dans le
 * segment qui contient son dernier candidat : wheel_count(point) est l'index
 * juste apres lui.
 */
static int visit_points(void *vctx,
                        const uint8_t *bits,
                        uint64_t first,
                        uint64_t nbits)
{
    points_ctx_t *c = vctx;

    uint64_t pos = 0;

    while (c->i < c->n)
    {
        uint64_t idx = wheel_count(c->points[c->i]);

        if (idx > first + nbits)
            break;

        uint64_t rel = idx > first ? idx - first : 0;

        if (rel > pos)
        {
            c->run += count_bits_range(bits, pos, rel);
            pos = rel;
        }

        c->counts[c->i++] += c->run;
    }

    c->run += count_bits_range(bits, pos, nbits);

    return c->i < c->n;
}

crible_status crible_count_points(uint64_t start,
                                  const uint64_t *points,
                                  size_t n,
                                  uint64_t *counts)
{
    if (!n)
        return CRIBLE_OK;

    uint64_t stop = points[n - 1];

    if (stop > MAX_LIMIT)
        return CRIBLE_EUNSUPPORTED;

    if (!api_init())
        return CRIBLE_ENOMEM;

    for (size_t i = 0; i < n; i++)
        counts[i] = small_primes_in(start, points[i], NULL);

    if (stop < start)
        return CRIBLE_OK;

    /* Un seul chunk : rien ne se vole sur un thread, et chaque chunk de plus
       reactiverait tous les premiers. */
    tuning_t tu = tuning_default;

    tu.chunk_override = UINT64_MAX;

    const base_t *base = base_acquire(isqrt_u64(stop));

    if (!base)
        return CRIBLE_ENOMEM;

    plan_t pl;

    if (!plan_build(&pl, base, start, stop, &tu, 1,
                    (int)crible_get_threads()))
    {
        base_release(base);

        return CRIBLE_ENOMEM;
    }

    points_ctx_t c = { points, counts, n, 0, 0 };

    int ok = engine_walk(&pl, visit_points, &c);

    /* Les points que le crible n'a pas atteints : l'intervalle ne porte
       aucun candidat. */
    for (; c.i < n; c.i++)
        counts[c.i] += c.run;

    plan_free(&pl);
    base_release(base);

    return ok ? CRIBLE_OK : CRIBLE_ENOMEM;
}

/* Octets de bitset par lot, et son plus grand contenu : 8 premiers par octet. */
#define FOREACH_BATCH_BYTES 4096
#define FOREACH_BATCH_MAX   (FOREACH_BATCH_BYTES * 8)

/* L'extraction AVX2 ecrit jusqu'a 8 cases au-dela de son resultat. */
#define FOREACH_OFF_SLACK 8

typedef struct
{
    crible_batch_cb cb;
    void           *ctx;
    uint64_t       *buf;
    uint32_t       *off;
} foreach_ctx_t;

#if defined(__AVX2__)
/*
 * Les premiers d'un mot, en decalages 32 bits depuis l'octet 0 du lot. Chaque
 * octet ecrit ses huit cases d'un coup, lues dans byte_offsets, a la place que
 * donne le popcount des octets qui le precedent : aucun branchement ne depend
 * des donnees, et l'octet suivant recouvre les cases en trop du precedent.
 * Une boucle a un ctz par premier coutait le double sur la meme fenetre.
 */
static inline size_t
extract_word_avx2(uint32_t *o, uint64_t w, uint32_t rel)
{
    const __m256i base = _mm256_set1_epi32((int)rel);

    for (int k = 0; k < 8; k++)
    {
        const unsigned v = (unsigned)(w >> (8 * k)) & 0xFFu;

        const size_t at =
            k ? (size_t)__builtin_popcountll(w & ((1ULL << (8 * k)) - 1))
              : 0;

        __m256i t =
            _mm256_loadu_si256((const __m256i *)(const void *)byte_offsets[v]);

        t = _mm256_add_epi32(t,
                             _mm256_add_epi32(base,
                                              _mm256_set1_epi32(30 * k)));

        _mm256_storeu_si256((__m256i *)(void *)(o + at), t);
    }

    return (size_t)__builtin_popcountll(w);
}
#endif

static int visit_foreach(void *vctx,
                         const uint8_t *bits,
                         uint64_t first,
                         uint64_t nbits)
{
    foreach_ctx_t *c = vctx;

    const uint64_t words = (nbits + 63) >> 6;
    const uint64_t step  = FOREACH_BATCH_BYTES / 8;

    /* first est multiple de 8 : le mot w commence a l'octet first / 8 + 8w. */
    const uint64_t byte0 = first >> 3;

    for (uint64_t w0 = 0; w0 < words; w0 += step)
    {
        const uint64_t w1 = w0 + step < words ? w0 + step : words;

        size_t n = 0;

#if defined(__AVX2__)
        for (uint64_t w = w0; w < w1; w++)
        {
            uint64_t v;

            memcpy(&v, bits + 8 * w, sizeof v);

            uint64_t rest = nbits - w * 64;

            if (rest < 64)
                v &= (1ULL << rest) - 1;

            n += extract_word_avx2(c->off + n, v,
                                   (uint32_t)(240 * (w - w0)));
        }

        {
            const uint64_t low = (byte0 + 8 * w0) * 30;

            for (size_t k = 0; k < n; k++)
                c->buf[k] = low + c->off[k];
        }
#else
        for (uint64_t w = w0; w < w1; w++)
        {
            uint64_t v;

            memcpy(&v, bits + 8 * w, sizeof v);

            uint64_t rest = nbits - w * 64;

            if (rest < 64)
                v &= (1ULL << rest) - 1;

            const uint64_t low = (byte0 + 8 * w) * 30;

            while (v)
            {
                int t = __builtin_ctzll(v);

                v &= v - 1;

                c->buf[n++] = low + bit_value[t];
            }
        }
#endif

        if (n && !c->cb(c->buf, n, c->ctx))
            return 0;
    }

    return 1;
}

crible_status crible_foreach_seg(uint64_t start,
                                 uint64_t stop,
                                 unsigned k,
                                 uint32_t segment_bytes,
                                 crible_batch_cb cb,
                                 void *ctx)
{
    if (start > stop || k < 1 || k > 6)
        return CRIBLE_OK;

    if (k != 1 || stop > MAX_LIMIT)
        return CRIBLE_EUNSUPPORTED;

    if (!api_init())
        return CRIBLE_ENOMEM;

    {
        uint64_t small[SMALL_PRIMES_MAX];

        size_t ns = small_primes_in(start, stop, small);

        if (ns && !cb(small, ns, ctx))
            return CRIBLE_OK;
    }

    tuning_t tu = tuning_default;

    tu.chunk_override = UINT64_MAX;

    /* segment_bytes n'est qu'un plancher. crible en fait la taille exacte,
       et compte passe 32 Kio pour l'empecher de lancer des threads et garder
       de petits lots : roue12 tient les deux quel que soit le segment, et
       32 Kio lui coutent le double du segment qu'il choisit lui-meme. */
    tu.segment_min_kb = ((uint64_t)segment_bytes + 1023) / 1024;

    const base_t *base = base_acquire(isqrt_u64(stop));

    if (!base)
        return CRIBLE_ENOMEM;

    plan_t pl;

    if (!plan_build(&pl, base, start, stop, &tu, 1,
                    (int)crible_get_threads()))
    {
        base_release(base);

        return CRIBLE_ENOMEM;
    }

    foreach_ctx_t c = { cb, ctx, NULL, NULL };

    /* Un seul bloc : les valeurs du lot, puis ses decalages 32 bits. */
    c.buf = malloc((size_t)FOREACH_BATCH_MAX * sizeof *c.buf
                   + ((size_t)FOREACH_BATCH_MAX + FOREACH_OFF_SLACK)
                     * sizeof *c.off);

    if (c.buf)
        c.off = (uint32_t *)(void *)(c.buf + FOREACH_BATCH_MAX);

    int ok = c.buf && engine_walk(&pl, visit_foreach, &c);

    free(c.buf);

    plan_free(&pl);
    base_release(base);

    return ok ? CRIBLE_OK : CRIBLE_ENOMEM;
}


#ifndef ROUE12_LIB

static double elapsed_seconds(struct timespec *a,
                              struct timespec *b)
{
    return
        (double)(b->tv_sec - a->tv_sec)
        +
        (double)(b->tv_nsec - a->tv_nsec)
        / 1e9;
}

static int parse_bound(const char *s, uint64_t *out)
{
    char *end;

    unsigned long long v = strtoull(s, &end, 10);

    if (end != s && *end == '\0')
    {
        *out = (uint64_t)v;

        return 1;
    }

    double d = strtod(s, &end);

    if (end == s || *end != '\0' || d <= 0.0 || d >= 1.8e19)
        return 0;

    *out = (uint64_t)d;

    return 1;
}


static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [BAS] HAUT [-d DIST] [-s KiB] [-b KiB] "
            "[-m N] [-B KiB]\n"
            "       [-S KiB] [-L N] [-K KiB] [-J N] [-t THREADS] "
            "[-c SEGMENTS]\n"
            "       [-p PMAX] [-Q N] [-v] [-h]\n"
            "  Un seul nombre : les premiers jusqu'a HAUT.\n"
            "  Deux nombres   : les premiers de l'intervalle "
            "[BAS, HAUT], bornes comprises.\n"
            "  -d DIST        : la meme chose en largeur, "
            "[DEBUT, DEBUT + DIST]\n"
            "                   (convention primesieve : "
            "`roue12 1e13 -d 1e11`).\n"
            "                   Le cout suit la LARGEUR de "
            "l'intervalle, plus le pre-crible\n"
            "                   des premiers jusqu'a racine(HAUT).\n"
            "  Les cinq etages, du plus petit balayage au plus "
            "grand : -b, -B, -S, puis la\n"
            "  bande du milieu (-J la borne) et les seaux (-K). "
            "Chacun s'eteint a 0.\n"
            "  -s KiB  taille du bitset par thread\n"
            "          (defaut : amorti sur les premiers au-dela de "
            "la plaque, plafonne au\n"
            "          L3 par thread — a un demi-L3 si la plaque est "
            "eteinte — et arrondi\n"
            "          a un multiple de bloc)\n"
            "  -b KiB  blocage L1 : les plus petits premiers rayent "
            "par blocs de cette taille\n"
            "          (0 pour desactiver ; defaut : deux tiers du L1 "
            "de donnees, divises\n"
            "          par le nombre de threads qui se partagent ce "
            "L1)\n"
            "  -m N    nombre de premiers passant par le chemin "
            "bloque, du plus petit au plus\n"
            "          grand (0 = automatique : ceux dont p <= taille "
            "de bloc)\n"
            "  -B KiB  tranche L2 : les premiers dont le tour depasse "
            "le bloc mais tient dans\n"
            "          la tranche rayent tranche par tranche "
            "(0 pour desactiver ; defaut :\n"
            "          un quart du L2 par thread). Le segment amorti "
            "suit : la tranche\n"
            "          reprend a son compte des premiers qui payaient "
            "une entree par segment\n"
            "  -S KiB  plaque : quatrieme etage, entre la tranche et "
            "le segment (0 pour\n"
            "          desactiver ; defaut : le L2 par thread). Elle "
            "reste allumee\n"
            "          meme quand sa bande couvre racine(HAUT) et vide "
            "la bande du milieu\n"
            "  -L N    bande de la plaque, en plaques : elle prend "
            "les premiers jusqu'a\n"
            "          N fois sa taille (defaut 1 ; l'elargir a ete "
            "mesure perdant)\n"
            "  -K KiB  fenetre de seau, cinquieme etage : chaque "
            "premier est range dans la\n"
            "          fenetre ou il marque au lieu d'etre parcouru "
            "partout (0 pour\n"
            "          desactiver, les grands premiers repassant "
            "alors par la bande\n"
            "          directe ; defaut : la tranche L2, ramenee a la "
            "puissance de deux\n"
            "          qui pave le segment)\n"
            "  -J N    frontiere du seau, en fenetres : au-dela de N "
            "fenetres un premier\n"
            "          passe par le seau, en deca par la bande du "
            "milieu (0 = automatique :\n"
            "          2,5 segments)\n"
            "  -t N    nombre de threads. Par defaut, le travail "
            "disponible divise par la\n"
            "          part minimale d'un thread (10^7 entiers, ou "
            "racine(HAUT)/5 au-dela\n"
            "          de 2,5e15), plafonne aux CPU logiques : un "
            "intervalle de moins de\n"
            "          2e7 entiers tourne sur un seul thread.\n"
            "          Une valeur explicite est honoree telle quelle "
            "et -v dit le choix\n"
            "  -c SEG  segments par chunk, l'unite que les threads se "
            "volent\n"
            "          (defaut : de quoi faire %d chunks par thread, "
            "elargi quand les\n"
            "          premiers a seau sont assez nombreux pour que leur "
            "repose a chaque\n"
            "          chunk pese, sans descendre sous %d chunks par "
            "thread)\n"
            "  -p PMAX borne du pre-crible (defaut : %d, 0 pour "
            "desactiver ; en dessous de 7\n"
            "          la marche 210 devient illicite et les seaux "
            "s'eteignent avec elle)\n"
            "  -Q N    distance de prechargement du vidage des seaux, "
            "en entrees\n"
            "          (defaut : %d ; 0 precharge l'entree courante, "
            "temoin qui garde la\n"
            "          meme forme de boucle sans precharger quoi que ce "
            "soit d'utile)\n"
            "  -v      recapitulatif detaille (roue, les cinq etages "
            "avec leur bande,\n"
            "          pre-crible, chunks) au lieu de la ligne "
            "unique ; --verbose aussi\n"
            "  -h      cette aide ; --help aussi\n",
            prog,
            CHUNKS_PER_THREAD,
            MIN_CHUNKS_PER_THREAD,
            PRESIEVE_DEFAULT_MAX,
            PREFETCH_DEFAUT);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage(stderr, argv[0]);

        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-h") ||
            !strcmp(argv[i], "--help"))
        {
            usage(stdout, argv[0]);

            return EXIT_SUCCESS;
        }
    }

    char *end;

    uint64_t limit;

    if (!parse_bound(argv[1], &limit))
    {
        fprintf(stderr,
                "Invalid limit\n");

        return EXIT_FAILURE;
    }

    uint64_t low_limit = 0;

    int first_flag = 2;

    if (argc > 2 && argv[2][0] != '-')
    {
        uint64_t high;

        if (!parse_bound(argv[2], &high))
        {
            fprintf(stderr, "Invalid interval bound\n");

            return EXIT_FAILURE;
        }

        low_limit = limit;
        limit     = high;

        first_flag = 3;

        if (low_limit > limit)
        {
            fprintf(stderr,
                    "Empty interval: %llu > %llu\n",
                    (unsigned long long)low_limit,
                    (unsigned long long)limit);

            return EXIT_FAILURE;
        }
    }

    if (limit > MAX_LIMIT)
    {
        fprintf(stderr,
                "Limit %llu exceeds the maximum %llu\n",
                (unsigned long long)limit,
                (unsigned long long)MAX_LIMIT);

        return EXIT_FAILURE;
    }

    uint64_t segment_kb = 0;

    long     want_threads  = 0;
    uint64_t chunk_override = 0;

    uint32_t presieve_max = PRESIEVE_DEFAULT_MAX;

    int      verbose     = 0;

    uint64_t dist     = 0;
    int      dist_set = 0;

    uint64_t bucket_kb  = 0;
    int      bucket_set = 0;

    uint64_t bucket_mult = 0;

    uint64_t slab_mult = 1;

    uint64_t block_kb    = 0;
    uint64_t chunk_kb  = 0;
    int      chunk_set = 0;
    uint64_t slab_kb   = 0;
    int      slab_set  = 0;
    int      block_set   = 0;
    uint64_t block_max   = 0;

    for (int i = first_flag; i < argc; i++)
    {
        if (!strcmp(argv[i], "-s"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-s requires a value\n");

                return EXIT_FAILURE;
            }

            segment_kb =
                strtoull(argv[++i],
                         &end,
                         10);

            if (*end != '\0' ||
                segment_kb == 0)
            {
                fprintf(stderr,
                        "Invalid segment size\n");

                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "-S"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "-S requires a value\n");

                return EXIT_FAILURE;
            }

            slab_kb = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr, "Invalid slab size\n");

                return EXIT_FAILURE;
            }

            slab_set = 1;
        }
        else if (!strcmp(argv[i], "-B"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "-B requires a value\n");

                return EXIT_FAILURE;
            }

            chunk_kb = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr, "Invalid chunk size\n");

                return EXIT_FAILURE;
            }

            chunk_set = 1;
        }
        else if (!strcmp(argv[i], "-b"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-b requires a value\n");

                return EXIT_FAILURE;
            }

            block_kb = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr,
                        "Invalid block size\n");

                return EXIT_FAILURE;
            }

            block_set = 1;
        }
        else if (!strcmp(argv[i], "-m"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-m requires a value\n");

                return EXIT_FAILURE;
            }

            block_max = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr,
                        "Invalid blocked prime count\n");

                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "-t"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-t requires a value\n");

                return EXIT_FAILURE;
            }

            want_threads = strtol(argv[++i], &end, 10);

            if (*end != '\0' || want_threads <= 0)
            {
                fprintf(stderr,
                        "Invalid thread count\n");

                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "-c"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-c requires a value\n");

                return EXIT_FAILURE;
            }

            chunk_override = strtoull(argv[++i], &end, 10);

            if (*end != '\0' || chunk_override == 0)
            {
                fprintf(stderr,
                        "Invalid chunk size\n");

                return EXIT_FAILURE;
            }
        }
        else if (!strcmp(argv[i], "-p"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-p requires a value\n");

                return EXIT_FAILURE;
            }

            unsigned long long v =
                strtoull(argv[++i], &end, 10);

            if (*end != '\0' || v > PRESIEVE_MAX_PRIME)
            {
                fprintf(stderr,
                        "Invalid presieve bound (0 .. %d)\n",
                        PRESIEVE_MAX_PRIME);

                return EXIT_FAILURE;
            }

            presieve_max = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "-K"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-K requires a value\n");

                return EXIT_FAILURE;
            }

            unsigned long long v = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr, "Invalid bucket window\n");

                return EXIT_FAILURE;
            }

            bucket_kb  = (uint64_t)v;
            bucket_set = 1;
        }
        else if (!strcmp(argv[i], "-L"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "-L requires a value\n");

                return EXIT_FAILURE;
            }

            unsigned long long v = strtoull(argv[++i], &end, 10);

            if (*end != '\0' || v == 0)
            {
                fprintf(stderr, "Invalid slab band\n");

                return EXIT_FAILURE;
            }

            slab_mult = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "-Q"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "-Q requires a value\n");

                return EXIT_FAILURE;
            }

            long v = strtol(argv[++i], &end, 10);

            if (*end != '\0' || v < 0 || v > 64)
            {
                fprintf(stderr, "Invalid prefetch distance (0 .. 64)\n");

                return EXIT_FAILURE;
            }

            g_prefetch = (int)v;
        }
        else if (!strcmp(argv[i], "-J"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "-J requires a value\n");

                return EXIT_FAILURE;
            }

            unsigned long long v = strtoull(argv[++i], &end, 10);

            if (*end != '\0')
            {
                fprintf(stderr, "Invalid bucket threshold\n");

                return EXIT_FAILURE;
            }

            bucket_mult = (uint64_t)v;
        }
        else if (!strcmp(argv[i], "-d"))
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr,
                        "-d requires a value\n");

                return EXIT_FAILURE;
            }

            uint64_t d;

            if (!parse_bound(argv[++i], &d))
            {
                fprintf(stderr,
                        "Invalid distance\n");

                return EXIT_FAILURE;
            }

            dist     = d;
            dist_set = 1;
        }
        else if (!strcmp(argv[i], "-v") ||
                 !strcmp(argv[i], "--verbose"))
        {
            verbose = 1;
        }
        else
        {
            fprintf(stderr,
                    "Unknown argument: %s\n"
                    "Try `%s -h`.\n",
                    argv[i],
                    argv[0]);

            return EXIT_FAILURE;
        }
    }

    if (dist_set)
    {
        if (first_flag == 3)
        {
            fprintf(stderr,
                    "BAS HAUT et -d ensemble : choisir l'un ou l'autre\n");

            return EXIT_FAILURE;
        }

        low_limit = limit;

        if (dist > MAX_LIMIT - low_limit)
        {
            fprintf(stderr,
                    "Interval end %llu + %llu exceeds the maximum "
                    "%llu\n",
                    (unsigned long long)low_limit,
                    (unsigned long long)dist,
                    (unsigned long long)MAX_LIMIT);

            return EXIT_FAILURE;
        }

        limit = low_limit + dist;
    }

    tuning_t tu =
    {
        .segment_kb     = segment_kb,
        .chunk_override = chunk_override,
        .presieve_max   = presieve_max,
        .block_kb       = block_kb,
        .block_set      = block_set,
        .block_max      = block_max,
        .chunk_kb       = chunk_kb,
        .chunk_set      = chunk_set,
        .slab_kb        = slab_kb,
        .slab_set       = slab_set,
        .slab_mult      = slab_mult,
        .bucket_kb      = bucket_kb,
        .bucket_set     = bucket_set,
        .bucket_mult    = bucket_mult,
    };

#ifdef _OPENMP
    int threads =
        want_threads > 0 ? (int)want_threads : omp_get_max_threads();
#else
    (void)want_threads;

    int threads = 1;
#endif

    int max_threads    = threads;
    int threads_capped = 0;

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);

#ifdef _OPENMP
    /* Le plafond ne vaut que pour le choix automatique : un -t explicite est
       honore tel quel, comme une taille de segment explicite. */
    if (want_threads <= 0)
    {
        int useful =
            default_thread_count(max_threads,
                                 limit - low_limit + 1,
                                 isqrt_u64(limit));

        if (useful < threads)
        {
            threads        = useful;
            threads_capped = 1;
        }
    }
#endif

    double presieve_want = 0.0;

    int presieve_status =
        presieve_build(presieve_max, &presieve_want);

    if (presieve_status == 1)
    {
        fprintf(stderr,
                "Presieve bound %u would need %.1f MiB of tables, "
                "budget is %llu MiB\n",
                presieve_max,
                presieve_want / (1024.0 * 1024.0),
                (unsigned long long)
                    (PRESIEVE_BUDGET_BYTES >> 20));

        return EXIT_FAILURE;
    }

    if (presieve_status == 2)
    {
        fprintf(stderr,
                "Unable to allocate presieve tables\n");

        return EXIT_FAILURE;
    }

    base_t *base = base_build(isqrt_u64(limit));

    plan_t pl;

    if (!base ||
        !plan_build(&pl, base, low_limit, limit, &tu, threads, threads))
    {
        fprintf(stderr, "allocation failed\n");

        base_free(base);
        presieve_free();

        return EXIT_FAILURE;
    }

    uint64_t found = 0;

    if (!engine_count(&pl, &found))
    {
        fprintf(stderr,
                "Unable to allocate per-thread buffers\n");

        plan_free(&pl);
        base_free(base);
        presieve_free();

        return EXIT_FAILURE;
    }

    uint64_t total = pl.small_total + found;

    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (!verbose)
    {
        double secs = elapsed_seconds(&t0, &t1);

        char when[32];

        if (secs < 1.0)
        {
            snprintf(when, sizeof when,
                     "%.1f ms", secs * 1000.0);
        }
        else
        {
            snprintf(when, sizeof when,
                     "%.3fs", secs);
        }

        if (low_limit)
        {
            printf("Found %llu primes between %llu and %llu "
                   "using %d threads, segment %llu KiB in %s\n",
                   (unsigned long long)total,
                   (unsigned long long)low_limit,
                   (unsigned long long)limit,
                   threads,
                   (unsigned long long)pl.segment_kb,
                   when);
        }
        else
        {
            printf("Found %llu primes up to %llu using %d threads, "
                   "segment %llu KiB in %s\n",
                   (unsigned long long)total,
                   (unsigned long long)limit,
                   threads,
                   (unsigned long long)pl.segment_kb,
                   when);
        }

        plan_free(&pl);
        base_free(base);
        presieve_free();

        return EXIT_SUCCESS;
    }

    if (low_limit)
    {
        printf("Found %llu primes between %llu and %llu\n",
               (unsigned long long)total,
               (unsigned long long)low_limit,
               (unsigned long long)limit);
    }
    else
    {
        printf("Found %llu primes up to %llu\n",
               (unsigned long long)total,
               (unsigned long long)limit);
    }

    printf("Wheel: 30\n");

    printf("Threads: %d", threads);

    if (threads_capped)
        printf(" (ramene de %d, part minimale par thread)", max_threads);

    printf("\n");

    printf("Segment: %llu KiB bitset (par thread, %s)\n",
           (unsigned long long)pl.segment_kb,
           pl.segment_origin);

    printf("Candidates/segment: %llu\n",
           (unsigned long long)pl.segment_bits);

    if (pl.block_bytes)
    {
        printf("L1 block: %llu KiB, %llu prime(s) blocked, p <= %u\n",
               (unsigned long long)(pl.block_bytes / 1024),
               (unsigned long long)pl.n_block,
               pl.p_block);
    }
    else
    {
        printf("L1 block: off\n");
    }

    if (pl.chunk_bytes)
    {
        printf("L2 chunk: %llu KiB, %llu prime(s), p <= %u\n",
               (unsigned long long)(pl.chunk_bytes / 1024),
               (unsigned long long)pl.n_chunk,
               pl.p_chunk);
    }
    else
    {
        printf("L2 chunk: off\n");
    }

    if (pl.slab_bytes)
    {
        printf("L2 slab: %llu KiB, %llu prime(s), p <= %u\n",
               (unsigned long long)(pl.slab_bytes / 1024),
               (unsigned long long)pl.n_slab,
               pl.p_slab);
    }
    else
    {
        printf("L2 slab: off\n");
    }

    if (pl.bucket_bytes)
    {
        printf("Seaux: fenetre %llu KiB, %llu anneau(x), p > %u, "
               "%llu premier(s), marche roue 210\n",
               (unsigned long long)(pl.bucket_bytes / 1024),
               (unsigned long long)pl.ring_slots,
               pl.p_bucket,
               (unsigned long long)pl.bucket_primes);
    }
    else
    {
        printf("Seaux: off\n");
    }

    if (g_pre_passes)
    {
        printf("Presieve: primes <= %u (%d), %d tables fused, "
               "%d passe(s) %s, %llu KiB\n",
               presieve_max,
               g_pre_count,
               g_pre_groups,
               g_pre_passes,
               PRESIEVE_PATH_LABEL,
               (unsigned long long)(g_pre_footprint / 1024));
    }
    else
    {
        printf("Presieve: off\n");
    }

    printf("Chunks: %llu of %llu segment(s)\n",
           (unsigned long long)pl.chunk_count,
           (unsigned long long)pl.chunk_segments);

    printf("Time: %.6f s\n",
           elapsed_seconds(&t0, &t1));

    plan_free(&pl);
    base_free(base);
    presieve_free();

    return EXIT_SUCCESS;
}

#endif /* ROUE12_LIB */
