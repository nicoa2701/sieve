/*
 * Crible de premiers segmente, roue mod 30, OpenMP.
 *
 * 1) Un octet porte les 8 residus premiers a 30 ; index_to_number et
 *    wheel_count convertissent entre index de bit et entier.
 * 2) Cinq etages de balayage, choisis par la taille du tour p*29/30 :
 *    bloc L1, tranche L2, plaque, bande directe, seaux.
 * 3) Seaux : un premier n'est parcouru que dans la fenetre ou il
 *    marque. Pas de roue 210 pretabule, anneau de blocs recycles.
 * 4) Pre-crible : les premiers <= 113 en tables periodiques fusionnees
 *    quatre par passe, AVX-512 intrinseque ou C vectorisable.
 * 5) Tailles deduites a l'execution des caches detectes ; le segment
 *    est amorti sur les 1/p de la bande du milieu.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(__AVX512F__)
#include <immintrin.h>
#endif


#define MAX_LIMIT 10000000000000000ULL

#define FALLBACK_SEGMENT_KB 512

#define MIN_SEGMENT_KB 32
#define MAX_SEGMENT_KB 65536

#define SEGMENT_MARKS_PER_ENTRY 40.0

#ifndef SINK_TAIL
#define SINK_TAIL 1
#endif

#define BLOCK_MIN_BYTES (16 * 1024ULL)

#define CHUNKS_PER_THREAD 8

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


static uint32_t *generate_base_primes(uint64_t limit,
                                       size_t *count)
{
    uint8_t *composite = calloc(limit + 1, 1);

    if (!composite)
    {
        fprintf(stderr, "allocation failed\n");
        exit(EXIT_FAILURE);
    }

    for (uint64_t p = 2; p * p <= limit; p++)
    {
        if (!composite[p])
        {
            for (uint64_t x = p * p;
                 x <= limit;
                 x += p)
            {
                composite[x] = 1;
            }
        }
    }

    size_t n = 0;

    for (uint64_t i = 2; i <= limit; i++)
    {
        if (!composite[i])
            n++;
    }

    uint32_t *primes =
        malloc((n ? n : 1) * sizeof(uint32_t));

    if (!primes)
    {
        fprintf(stderr, "allocation failed\n");
        free(composite);
        exit(EXIT_FAILURE);
    }

    size_t k = 0;

    for (uint64_t i = 2; i <= limit; i++)
    {
        if (!composite[i])
            primes[k++] = (uint32_t)i;
    }

    free(composite);

    *count = n;

    return primes;
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

static void presieve_free(void)
{
    for (int g = 0; g < g_pre_groups; g++)
        free((void *)g_pre[g].data);

    g_pre_groups = 0;
    g_pre_tables = 0;
    g_pre_passes = 0;
}

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

        if (t == 0)
        {
            for (uint64_t i = 0; i < n; i += 64)
            {
                __m512i v = _mm512_loadu_si512(b0 + o0);

                v = _mm512_and_si512(v, _mm512_loadu_si512(b1 + o1));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b2 + o2));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b3 + o3));

                _mm512_store_si512((__m512i *)(dst + i), v);

                PRESIEVE_ADVANCE
            }
        }
        else
        {
            for (uint64_t i = 0; i < n; i += 64)
            {
                __m512i v = _mm512_loadu_si512(b0 + o0);

                v = _mm512_and_si512(v, _mm512_loadu_si512(b1 + o1));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b2 + o2));
                v = _mm512_and_si512(v, _mm512_loadu_si512(b3 + o3));

                v = _mm512_and_si512(
                        v,
                        _mm512_load_si512((const __m512i *)(dst + i)));

                _mm512_store_si512((__m512i *)(dst + i), v);

                PRESIEVE_ADVANCE
            }
        }

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
}


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

/* Le tableau d'entrees remplit exactement le bloc : le pointeur atteint la
   frontiere suivante quand le bloc est plein, et vaut NULL quand il n'y a pas
   encore de bloc — un seul test couvre les deux cas. */
static inline int bucket_push(bucket_ring_t *r,
                              uint64_t d,
                              uint32_t k,
                              uint32_t at)
{
    bucket_entry_t **slot = &r->cur[d];

    if (((uintptr_t)*slot & (BUCKET_BLOCK_BYTES - 1)) == 0 &&
        !bucket_open(r, slot))
    {
        return 0;
    }

    (*slot)->k  = k;
    (*slot)->at = at;
    (*slot)++;

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
__attribute__((noinline))
static int sweep_bucketed(uint8_t *bits,
                          uint64_t segment_bytes,
                          bucket_ring_t *r,
                          uint64_t gw0,
                          uint64_t chunk_windows)
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

        bucket_entry_t **slot = &r->cur[0];

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

            while (idx < end)
            {
                const wheel_step_t st = wheel210_step[wi];

                w[idx] &= st.mask;

                idx += (uint64_t)k * st.dR + st.dT;
                wi   = st.next;
            }

            {
                uint64_t skip = idx >> shift;

                if (skip < left &&
                    !bucket_push(r, skip, k,
                                 (uint32_t)(((idx - (skip << shift)) << 9)
                                            | wi)))
                {
                    return 0;
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
                    uint64_t block_bytes,
                    uint64_t overshoot,
                    int64_t sink,
                    int blocked)
{
    const size_t o0 = toff[0], o1 = toff[1], o2 = toff[2], o3 = toff[3],
                 o4 = toff[4], o5 = toff[5], o6 = toff[6], o7 = toff[7];

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
                        sweep_over(bits, &cursor[i],          \
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
                        sweep_exact(bits, &cursor[i],         \
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
                          uint64_t block_bytes,
                          uint64_t chunk_bytes,
                          uint64_t overshoot,
                          uint64_t overshoot2,
                          int64_t sink,
                          int blocked)
{
    const size_t o0 = toff[0], o1 = toff[1], o2 = toff[2], o3 = toff[3],
                 o4 = toff[4], o5 = toff[5], o6 = toff[6], o7 = toff[7];

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
                            sweep_over(bits, &cursor[i],      \
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
                            sweep_exact(bits, &cursor[i],     \
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
                        sweep_over(bits, &cursor[i],          \
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
                        sweep_exact(bits, &cursor[i],         \
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
                            sweep_over(bits, &cursor[i],      \
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
                            sweep_exact(bits, &cursor[i],     \
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
                        sweep_over(bits, &cursor[i],          \
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
                        sweep_exact(bits, &cursor[i],         \
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
                    sweep_over(bits, &cursor[i],              \
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
                    sweep_exact(bits, &cursor[i],             \
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
              size_t *active,
              wheel_cursor_t *cursor,
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

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t a   = active[rc];
        size_t end = class_start[rc + 1];

        {
            size_t stop = end < class_mid4[rc] ? end : class_mid4[rc];

            while (a < stop)
            {
                uint32_t p = sorted[a];

                if ((uint64_t)p * p > high)
                    goto fini;

                activate_prime(p, segment_first_index, low, &cursor[a]);

                a++;
            }
        }

        while (a < end)
        {
            uint32_t p = sorted[a];

            if ((uint64_t)p * p > high)
                break;

            a++;
        }

    fini:;

        active[rc] = a;
    }

    if (ring)
    {
        for (unsigned rc = 0; rc < 8; rc++)
        {
            size_t from = was[rc] > class_mid4[rc] ? was[rc] : class_mid4[rc];

            /* Les premiers au-dela de class_mid4 entrent dans les seaux des
               leur activation, positionnes sur la roue 210. */
            for (size_t a = from; a < active[rc]; a++)
            {
                uint32_t p = sorted[a];

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

                int64_t x = (int64_t)((uint64_t)p * m / 30)
                          - (int64_t)(segment_first_index >> 3);

                if (x < 0)
                    continue;

                uint64_t skip = (uint64_t)x >> ring->shift;

                if (gw0 + skip < chunk_windows &&
                    !bucket_push(ring, skip, p / 30,
                                 (uint32_t)(((uint64_t)x -
                                             (skip << ring->shift)) << 9)
                                 | (uint32_t)(rc * 48 + j)))
                {
                    *bucket_failed = 1;
                }
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
                      mid2, mid3, cursor, block_bytes, chunk_bytes, slab_bytes,
                      overshoot, overshoot2, overshoot3, sink, blocked);
    }
    else if (!chunked)
    {
        sweep_flat(bits, segment_bytes, sorted, turn, toff, class_start, mid,
                   cursor, block_bytes, overshoot, sink, blocked);
    }
    else
    {
        sweep_chunked(bits, segment_bytes, sorted, turn, toff, class_start, mid,
                      mid2, cursor, block_bytes, chunk_bytes, overshoot,
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
        sweep_exact_calc(bits, &cursor[i],                    \
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

static uint64_t default_block_bytes(int threads)
{
#ifdef _SC_LEVEL1_DCACHE_SIZE
    long l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);

    if (l1 > 0)
    {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);

        unsigned share = cache_sharers(1);

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
#else
    (void)threads;
#endif

    return 0;
}

static uint64_t default_chunk_bytes(int threads)
{
#ifdef _SC_LEVEL2_CACHE_SIZE
    long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);

    if (l2 > 0)
    {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);

        unsigned share = cache_sharers(2);

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
#else
    (void)threads;
#endif

    return 0;
}

static uint64_t default_slab_bytes(int threads)
{
#ifdef _SC_LEVEL2_CACHE_SIZE
    long l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);

    if (l2 > 0)
    {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);

        unsigned share = cache_sharers(2);

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
#else
    (void)threads;
#endif

    return 0;
}

/*
 * Taille de segment telle qu'un premier de [lo, hi) fasse en moyenne
 * SEGMENT_MARKS_PER_ENTRY marques par segment : il y fait 8s / p marques pour
 * une seule entree de curseur.
 */
static uint64_t amortized_segment_bytes(const uint32_t *primes,
                                        size_t lo,
                                        size_t hi)
{
    if (hi <= lo)
        return 0;

    double inv = 0.0;

    for (size_t i = lo; i < hi; i++)
        inv += 1.0 / (double)primes[i];

    if (!(inv > 0.0))
        return 0;

    double s =
        (double)(hi - lo) * SEGMENT_MARKS_PER_ENTRY / (8.0 * inv);

    if (!(s > 0.0))
        return 0;

    return (uint64_t)s;
}


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
            "          (defaut : amorti sur les premiers de la bande "
            "du milieu, plafonne au\n"
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
            "s'eteint d'elle-meme\n"
            "          quand elle viderait la bande du milieu, et -v "
            "le dit\n"
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
            "  -t N    nombre de threads (defaut : ce que decide "
            "OpenMP, soit tous les CPU\n"
            "          logiques)\n"
            "  -c SEG  segments par chunk, l'unite que les threads se "
            "volent\n"
            "          (defaut : de quoi faire %d chunks par thread)\n"
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

#ifdef _OPENMP
    if (want_threads > 0)
        omp_set_num_threads((int)want_threads);

    int threads = omp_get_max_threads();
#else
    (void)want_threads;

    int threads = 1;
#endif

    struct timespec t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    const char *segment_origin = "-s";


    uint64_t segment_bytes = 0;
    uint64_t segment_bits  = 0;

    uint64_t root =
        (uint64_t)sqrt((double)limit);

    while ((root + 1) <= limit / (root + 1))
        root++;

    while (root && root > limit / root)
        root--;

    size_t prime_count;

    uint32_t *primes =
        generate_base_primes(root,
                             &prime_count);

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

        free(primes);

        return EXIT_FAILURE;
    }

    if (presieve_status == 2)
    {
        fprintf(stderr,
                "Unable to allocate presieve tables\n");

        free(primes);

        return EXIT_FAILURE;
    }

    size_t sieve_start = prime_count < 3 ? prime_count : 3;

    while (sieve_start < prime_count &&
           primes[sieve_start] <= presieve_max)
    {
        sieve_start++;
    }

    uint64_t block_bytes =
        block_set ? block_kb * 1024ULL
                  : default_block_bytes(threads);

    size_t block_end = sieve_start;

    if (block_bytes)
    {
        if (block_max)
        {
            block_end = sieve_start + block_max;

            if (block_end > prime_count)
                block_end = prime_count;
        }
        else
        {
            uint64_t pmax = block_bytes / BLOCK_MIN_TURNS;

            while (block_end < prime_count &&
                   primes[block_end] <= pmax)
            {
                block_end++;
            }
        }

        if (block_end <= sieve_start)
            block_bytes = 0;
    }

    uint64_t chunk_bytes =
        chunk_set ? chunk_kb * 1024ULL
                  : default_chunk_bytes(threads);

    size_t chunk_end = block_end;

    if (chunk_bytes <= block_bytes)
        chunk_bytes = 0;

    if (chunk_bytes)
    {
        while (chunk_end < prime_count &&
               primes[chunk_end] <= chunk_bytes)
        {
            chunk_end++;
        }

        if (chunk_end <= block_end)
            chunk_bytes = 0;
    }

    uint32_t p_chunk = chunk_bytes ? primes[chunk_end - 1] : 0;

    /* Marge d'un tour complet, celle qu'exige sweep_over. */
    uint64_t overshoot2 =
        chunk_bytes ? (uint64_t)p_chunk * 29 / 30 + 1 : 0;

    uint64_t slab_bytes =
        slab_set ? slab_kb * 1024ULL
                 : default_slab_bytes(threads);

    size_t slab_end = chunk_end;

    if (!chunk_bytes || slab_bytes <= chunk_bytes)
        slab_bytes = 0;

    if (slab_bytes)
    {
        uint64_t slab_band = slab_bytes * slab_mult;

        while (slab_end < prime_count &&
               (uint64_t)primes[slab_end] <= slab_band)
        {
            slab_end++;
        }

        if (slab_end <= chunk_end)
            slab_bytes = 0;
    }

    const char *slab_off = "";

    if (slab_bytes && slab_end >= prime_count)
    {
        slab_bytes = 0;
        slab_end   = chunk_end;
        slab_off   = " (sqrt(N) <= plaque : elle vide la bande directe)";
    }

    uint32_t p_slab = slab_bytes ? primes[slab_end - 1] : 0;

    uint64_t overshoot3 =
        slab_bytes ? (uint64_t)p_slab * 29 / 30 + 1 : 0;

    if (segment_kb == 0)
    {
        uint64_t s =
            amortized_segment_bytes(primes, slab_end, prime_count);

        if (s)
        {
            segment_origin = "amorti auto";
        }
        else
        {
            uint64_t l2_kb = detect_cache_kb(2);

            if (l2_kb >= MIN_SEGMENT_KB &&
                l2_kb <= MAX_SEGMENT_KB)
            {
                s = l2_kb * 1024ULL / 2;
                segment_origin = "L2/2, chemin direct vide";
            }
            else
            {
                s = FALLBACK_SEGMENT_KB * 1024ULL;
                segment_origin = "defaut, chemin direct vide";
            }
        }

        {
            uint64_t l3_kb = detect_cache_kb(3);

            if (l3_kb && threads > 0)
            {
                uint64_t share = slab_bytes ? 1ULL : 2ULL;

                uint64_t cap =
                    l3_kb * 1024ULL / share / (uint64_t)threads;

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

        if (!block_max)
        {
            uint64_t pmax = block_bytes / BLOCK_MIN_TURNS;

            block_end = sieve_start;

            while (block_end < prime_count &&
                   primes[block_end] <= pmax)
            {
                block_end++;
            }
        }

        if (block_end <= sieve_start)
            block_bytes = 0;
    }

    uint32_t p_block =
        block_bytes ? primes[block_end - 1] : 0;

    uint64_t overshoot =
        block_bytes
            ? (uint64_t)p_block * 29 / 30 + 1
            : 0;

    size_t sorted_count = prime_count - sieve_start;

    uint32_t *sorted =
        malloc((sorted_count ? sorted_count : 1) * sizeof *sorted);

    if (!sorted)
    {
        fprintf(stderr, "allocation failed\n");

        free(primes);
        presieve_free();

        return EXIT_FAILURE;
    }

    size_t class_start[9] = { 0 };

    {
        size_t count[8] = { 0 };

        for (size_t i = sieve_start; i < prime_count; i++)
            count[residue_to_index[primes[i] % 30]]++;

        size_t pos[8];

        for (unsigned rc = 0; rc < 8; rc++)
        {
            class_start[rc + 1] = class_start[rc] + count[rc];
            pos[rc] = class_start[rc];
        }

        for (size_t i = sieve_start; i < prime_count; i++)
            sorted[pos[residue_to_index[primes[i] % 30]]++] = primes[i];
    }

    size_t class_mid[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t i = class_start[rc];

        while (i < class_start[rc + 1] && sorted[i] <= p_block)
            i++;

        class_mid[rc] = i;
    }

    if (chunk_bytes > segment_bytes)
        chunk_bytes = segment_bytes;

    size_t class_mid2[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t i = class_mid[rc];

        while (i < class_start[rc + 1] && sorted[i] <= p_chunk)
            i++;

        class_mid2[rc] = i;
    }

    if (slab_bytes > segment_bytes)
        slab_bytes = segment_bytes;

    uint64_t bucket_bytes = bucket_set ? bucket_kb * 1024
                          : (chunk_bytes ? chunk_bytes : slab_bytes);

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

    int bucket_alloc_failed = 0;

    size_t class_mid3[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t i = class_mid2[rc];

        while (i < class_start[rc + 1] && sorted[i] <= p_slab)
            i++;

        class_mid3[rc] = i;
    }

    uint64_t p_bucket_want =
        bucket_mult ? bucket_bytes * bucket_mult : segment_bytes * 5 / 2;

    uint32_t p_bucket =
        bucket_bytes && p_bucket_want < UINT32_MAX
            ? (uint32_t)p_bucket_want
            : 0;

    size_t class_mid4[8];

    for (unsigned rc = 0; rc < 8; rc++)
    {
        size_t i = class_mid3[rc];

        if (p_bucket)
            while (i < class_start[rc + 1] && sorted[i] <= p_bucket)
                i++;
        else
            i = class_start[rc + 1];

        class_mid4[rc] = i;
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
    {
        fprintf(stderr, "allocation failed\n");

        free(primes);
        free(sorted);
        presieve_free();

        return EXIT_FAILURE;
    }

    for (unsigned rc = 0; rc < 8; rc++)
    {
        build_turn_offsets(sorted + class_start[rc],
                           class_mid3[rc] - class_start[rc],
                           turn + tbase[rc] * TURN_SLOTS);
    }

    build_wheel210();

    uint64_t total = 0;

    if (2 >= low_limit && 2 <= limit) total++;
    if (3 >= low_limit && 3 <= limit) total++;
    if (5 >= low_limit && 5 <= limit) total++;

    for (int i = 0; i < g_pre_count; i++)
    {
        if (g_pre_primes[i] >= low_limit &&
            g_pre_primes[i] <= limit)
        {
            total++;
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
    else
    {
        uint64_t target =
            (uint64_t)threads * CHUNKS_PER_THREAD;

        chunk_segments = (total_segments + target - 1) / target;

        if (chunk_segments == 0)
            chunk_segments = 1;
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
            (segment_bytes + 2 * (uint64_t)primes[prime_count - 1])
            / bucket_bytes + 4;

        uint64_t cap_windows =
            (chunk_candidates / 8 + bucket_bytes - 1) / bucket_bytes + 2;

        if (need > cap_windows)
            need = cap_windows;

        while (ring_slots < need)
            ring_slots <<= 1;
    }


    int alloc_failed = 0;

    uint64_t found = 0;

#ifdef _OPENMP
#   pragma omp parallel reduction(+:found)
#endif
    {
        uint8_t *bits =
            aligned_alloc(64, segment_bytes + 64);

        wheel_cursor_t *cursor =
            malloc((sorted_count ? sorted_count : 1) * sizeof *cursor);

        bucket_ring_t ring;

        memset(&ring, 0, sizeof ring);

        ring.slots = ring_slots;
        ring.win   = bucket_bytes;
        ring.shift = bucket_shift;

        if (bucket_bytes)
        {
            ring.cur = calloc((size_t)ring_slots, sizeof *ring.cur);

            if (!ring.cur)
            {
#ifdef _OPENMP
#               pragma omp atomic write
#endif
                bucket_alloc_failed = 1;
            }
        }

        int bucket_failed = 0;

        if (!bits || !cursor)
        {
#ifdef _OPENMP
#           pragma omp atomic write
#endif
            alloc_failed = 1;
        }

#ifdef _OPENMP
#       pragma omp for schedule(dynamic, 1)
#endif
        for (int64_t c = 0; c < (int64_t)chunk_count; c++)
        {
            int stop;

#ifdef _OPENMP
#           pragma omp atomic read
#endif
            stop = alloc_failed;

            if (stop)
                continue;

            uint64_t chunk_first =
                sieve_first + (uint64_t)c * chunk_candidates;

            uint64_t chunk_end =
                chunk_first + chunk_candidates;

            if (chunk_end > total_candidates)
                chunk_end = total_candidates;

            size_t active[8];

            for (unsigned rc = 0; rc < 8; rc++)
                active[rc] = class_start[rc];

            uint64_t chunk_windows = 0;

            if (ring.cur)
            {
                for (uint64_t i = 0; i < ring_slots; i++)
                {
                    bucket_entry_t *p = ring.cur[i];

                    ring.cur[i] = NULL;

                    for (bucket_block_t *b = p ? BUCKET_BLOCK_OF(p) : NULL;
                         b; )
                    {
                        bucket_block_t *prev = b->next;

                        b->next  = ring.stock;
                        ring.stock = b;
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

                if (high > limit)
                    high = limit;

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
                    sorted,
                    turn,
                    toff,
                    class_start,
                    class_mid,
                    class_mid2,
                    class_mid3,
                    class_mid4,
                    active,
                    cursor,
                    block_bytes,
                    chunk_bytes,
                    slab_bytes,
                    overshoot,
                    overshoot2,
                    overshoot3,
                    ring.cur ? &ring : NULL,
                    ((segment_first - chunk_first) / 8) / (bucket_bytes ?
                                                           bucket_bytes : 1),
                    chunk_windows,
                    &bucket_failed
                );

                if (bucket_failed)
                {
#ifdef _OPENMP
#                   pragma omp atomic write
#endif
                    bucket_alloc_failed = 1;
                }

                if (segment_first == sieve_first)
                {
                    for (uint64_t b = 0; b < skip_bits; b++)
                        clear_bit(bits, b);
                }

                if (segment_first == 0)
                {
                    clear_bit(bits, 0);
                }

                found += count_set_bits(
                    bits,
                    actual_bits
                );

                segment_first = segment_last + 1;
            }
        }

        free(bits);
        free(cursor);
        bucket_ring_free(&ring);
    }

    if (bucket_alloc_failed)
        alloc_failed = 1;

    if (alloc_failed)
    {
        fprintf(stderr,
                "Unable to allocate per-thread buffers\n");

        free(primes);
        free(sorted);
        free(turn);
        presieve_free();

        return EXIT_FAILURE;
    }

    total += found;

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
                   (unsigned long long)segment_kb,
                   when);
        }
        else
        {
            printf("Found %llu primes up to %llu using %d threads, "
                   "segment %llu KiB in %s\n",
                   (unsigned long long)total,
                   (unsigned long long)limit,
                   threads,
                   (unsigned long long)segment_kb,
                   when);
        }

        free(primes);
        free(sorted);
        free(turn);
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

    printf("Threads: %d\n", threads);

    printf("Segment: %llu KiB bitset (par thread, %s)\n",
           (unsigned long long)segment_kb,
           segment_origin);

    printf("Candidates/segment: %llu\n",
           (unsigned long long)segment_bits);

    if (block_bytes)
    {
        printf("L1 block: %llu KiB, %llu prime(s) blocked, p <= %u\n",
               (unsigned long long)(block_bytes / 1024),
               (unsigned long long)(block_end - sieve_start),
               p_block);
    }
    else
    {
        printf("L1 block: off\n");
    }

    if (chunk_bytes)
    {
        printf("L2 chunk: %llu KiB, %llu prime(s), p <= %u\n",
               (unsigned long long)(chunk_bytes / 1024),
               (unsigned long long)(chunk_end - block_end),
               p_chunk);
    }
    else
    {
        printf("L2 chunk: off\n");
    }

    if (slab_bytes)
    {
        printf("L2 slab: %llu KiB, %llu prime(s), p <= %u\n",
               (unsigned long long)(slab_bytes / 1024),
               (unsigned long long)(slab_end - chunk_end),
               p_slab);
    }
    else
    {
        printf("L2 slab: off%s\n", slab_off);
    }

    if (bucket_bytes)
    {
        printf("Seaux: fenetre %llu KiB, %llu anneau(x), p > %u, "
               "marche roue 210\n",
               (unsigned long long)(bucket_bytes / 1024),
               (unsigned long long)ring_slots,
               p_bucket);
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
           (unsigned long long)chunk_count,
           (unsigned long long)chunk_segments);

    printf("Time: %.6f s\n",
           elapsed_seconds(&t0, &t1));

    free(primes);
    free(sorted);
    free(turn);
    presieve_free();

    return EXIT_SUCCESS;
}
