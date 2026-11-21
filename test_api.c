/*
 * Controle de l'API de roue12.h.
 *
 * 1) Chaque fonction contre une reference independante : un crible naif
 *    jusqu'a NAIVE_N, des valeurs connues de pi, Miller-Rabin aux grandes
 *    bornes.
 * 2) Les lignes "D ..." resument tous les resultats. Le meme programme, lie a
 *    libcrible.a et lance avec l'argument "ref", doit sortir les memes :
 *    c'est ce que compare `make check-crible`. En mode "ref" les controles
 *    propres a roue12 (borne 10^16, k >= 2) sont sautes.
 */
#include "roue12.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond))                                      \
        {                                                 \
            failures++;                                   \
            printf("ECHEC  " __VA_ARGS__);                \
            printf("\n");                                 \
        }                                                 \
    } while (0)

#define FNV_BASIS 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t mix(uint64_t h, uint64_t x)
{
    return (h ^ x) * FNV_PRIME;
}

static void digest(const char *what, uint64_t h)
{
    printf("D %-22s %016" PRIx64 "\n", what, h);
}

/* xorshift64* : la meme suite dans les deux bibliotheques. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;

    return rng_state * 0x2545F4914F6CDD1DULL;
}

/* Miller-Rabin deterministe sur 64 bits. */
static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)((unsigned __int128)a * b % m);
}

static uint64_t powmod(uint64_t a, uint64_t e, uint64_t m)
{
    uint64_t r = 1;

    a %= m;

    while (e)
    {
        if (e & 1)
            r = mulmod(r, a, m);

        a = mulmod(a, a, m);
        e >>= 1;
    }

    return r;
}

static int is_prime(uint64_t n)
{
    static const uint64_t bases[12] =
        { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };

    if (n < 2)
        return 0;

    for (int i = 0; i < 12; i++)
    {
        if (n % bases[i] == 0)
            return n == bases[i];
    }

    uint64_t d = n - 1;
    int      s = 0;

    while (!(d & 1))
    {
        d >>= 1;
        s++;
    }

    for (int i = 0; i < 12; i++)
    {
        uint64_t x = powmod(bases[i], d, n);

        if (x == 1 || x == n - 1)
            continue;

        int composite = 1;

        for (int r = 1; r < s; r++)
        {
            x = mulmod(x, x, n);

            if (x == n - 1)
            {
                composite = 0;
                break;
            }
        }

        if (composite)
            return 0;
    }

    return 1;
}

/* Crible naif : prefix[n] = pi(n), isp[n] = n premier. */
enum { NAIVE_N = 3000000 };

static uint32_t *prefix;
static uint8_t  *isp;

static void build_naive(void)
{
    isp    = malloc(NAIVE_N + 1);
    prefix = malloc((NAIVE_N + 1) * sizeof *prefix);

    if (!isp || !prefix)
    {
        printf("allocation du crible naif\n");
        exit(1);
    }

    memset(isp, 1, NAIVE_N + 1);

    isp[0] = isp[1] = 0;

    for (uint64_t i = 2; i * i <= NAIVE_N; i++)
    {
        if (isp[i])
            for (uint64_t k = i * i; k <= NAIVE_N; k += i)
                isp[k] = 0;
    }

    uint32_t c = 0;

    for (uint32_t i = 0; i <= NAIVE_N; i++)
    {
        c += isp[i];
        prefix[i] = c;
    }
}

static uint64_t naive(uint64_t a, uint64_t b)
{
    if (a > b)
        return 0;

    return prefix[b] - (a ? prefix[a - 1] : 0);
}

/* Longueur log-uniforme dans [0, max]. */
static uint64_t rand_len(uint64_t max)
{
    uint64_t bits = rnd() % 64;
    uint64_t v    = bits ? rnd() >> (64 - bits) : 0;

    return v % (max + 1);
}

static uint64_t count(uint64_t a, uint64_t b)
{
    uint64_t r = 0;

    crible_status st = crible_count_primes(a, b, &r);

    CHECK(st == CRIBLE_OK, "count_primes [%" PRIu64 ", %" PRIu64 "] : "
          "statut %d", a, b, (int)st);

    return r;
}

/* --- crible_count_primes ---------------------------------------------- */

static void test_count_refs(void)
{
    static const struct { uint64_t a, b, want; } t[] =
    {
        { 0, 0, 0 }, { 0, 1, 0 }, { 2, 2, 1 }, { 10, 1, 0 },
        { 0, 10, 4 }, { 0, 100, 25 }, { 100, 1000, 143 },
        { 5, 11, 3 }, { 24, 28, 0 }, { 113, 127, 2 },
        { 0, 1000000, 78498 },
        { 0, 100000000, 5761455 },
        { 0, 1000000000, 50847534 },
        { 4294967296ULL, 4294967296ULL + 100000000, 4506732 },
        { 10000000000ULL, 10000000000ULL + 1000000000, 43336106 },
        { 1000000000000ULL, 1000000000000ULL + 1000000000, 36190991 },
        { 1000000000000000ULL, 1000000000000000ULL + 1000000000, 28946421 },
        { 1000000000000000ULL, 1000000000100000ULL, 2805 },
        { 9999999999999999ULL, 10000000000000000ULL, 0 },
        { 10000000000000000ULL, 10000000000000000ULL, 0 },
    };

    uint64_t h = FNV_BASIS;

    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
    {
        uint64_t r = count(t[i].a, t[i].b);

        CHECK(r == t[i].want, "count_primes [%" PRIu64 ", %" PRIu64 "] = %"
              PRIu64 ", attendu %" PRIu64, t[i].a, t[i].b, r, t[i].want);

        h = mix(h, r);
    }

    digest("count_primes refs", h);
}

static void test_count_random(int iters)
{
    uint64_t h = FNV_BASIS;

    for (int it = 0; it < iters; it++)
    {
        uint64_t a = rnd() % (NAIVE_N + 1);
        uint64_t b = a + rand_len(NAIVE_N);

        if (b > NAIVE_N)
            b = NAIVE_N;

        if (rnd() % 16 == 0)
        {
            uint64_t s = a;

            a = b;
            b = s;
        }

        uint64_t r = count(a, b);
        uint64_t e = naive(a, b);

        CHECK(r == e, "count_primes [%" PRIu64 ", %" PRIu64 "] = %" PRIu64
              ", crible naif %" PRIu64, a, b, r, e);

        h = mix(h, r);
    }

    digest("count_primes naif", h);
}

static void test_threads(void)
{
    static const unsigned th[] = { 1, 2, 5, 0 };

    uint64_t h = FNV_BASIS;

    /* Le second intervalle n'a pas de valeur connue : les nombres de threads
       doivent s'accorder entre eux. */
    uint64_t first = 0;

    for (size_t i = 0; i < sizeof th / sizeof th[0]; i++)
    {
        crible_set_threads(th[i]);

        unsigned got = crible_get_threads();

        CHECK(got >= 1 && (th[i] != 1 || got == 1),
              "get_threads apres set_threads(%u) = %u", th[i], got);

        uint64_t r1 = count(0, 1000000000);
        uint64_t r2 = count(10000000000000ULL,
                            10000000000000ULL + 300000000);

        if (i == 0)
            first = r2;

        CHECK(r1 == 50847534, "%u thread(s) : pi(1e9) = %" PRIu64,
              th[i], r1);
        CHECK(r2 == first, "%u thread(s) : [1e13, +3e8] = %" PRIu64
              ", %" PRIu64 " sur un thread", th[i], r2, first);

        h = mix(mix(h, r1), r2);
    }

    crible_set_threads(0);

    digest("threads", h);
}

/* --- crible_count_points ---------------------------------------------- */

static int cmp_u64(const void *p, const void *q)
{
    uint64_t u = *(const uint64_t *)p;
    uint64_t v = *(const uint64_t *)q;

    return (u > v) - (u < v);
}

enum { MAX_POINTS = 200 };

/* Tire une serie de points croissants autour de [start, start + span], avec
   des doublons et des points sous start. */
static size_t draw_points(uint64_t start, uint64_t span, uint64_t *pts)
{
    size_t np = 1 + (size_t)(rnd() % MAX_POINTS);

    for (size_t i = 0; i < np; i++)
    {
        uint64_t off = rnd() % (span + 1);

        pts[i] = start + off;

        if (i % 17 == 3 && start >= 20)
            pts[i] = start - 1 - rnd() % 10;

        if (i % 11 == 5)
            pts[i] = pts[i - 1];
    }

    qsort(pts, np, sizeof *pts, cmp_u64);

    return np;
}

static void test_points_naive(int iters)
{
    uint64_t h = FNV_BASIS;

    for (int it = 0; it < iters; it++)
    {
        uint64_t start = rnd() % (NAIVE_N / 2);
        uint64_t span  = 1 + rand_len(NAIVE_N / 2 - 1);

        if (it % 5 == 0)
            start = rnd() % 200;

        uint64_t pts[MAX_POINTS], got[MAX_POINTS];

        size_t np = draw_points(start, span, pts);

        crible_status st = crible_count_points(start, pts, np, got);

        CHECK(st == CRIBLE_OK, "count_points : statut %d", (int)st);

        for (size_t i = 0; i < np; i++)
        {
            uint64_t e = naive(start, pts[i]);

            CHECK(got[i] == e, "count_points [%" PRIu64 ", %" PRIu64
                  "] = %" PRIu64 ", crible naif %" PRIu64,
                  start, pts[i], got[i], e);

            h = mix(h, got[i]);
        }
    }

    digest("count_points naif", h);
}

/* Aux grandes bornes : chaque point contre crible_count_primes. */
static void test_points_high(void)
{
    static const uint64_t bases[] =
    {
        10000000000ULL, 1000000000000ULL, 100000000000000ULL,
        1000000000000000ULL, 9999999900000000ULL,
    };

    uint64_t h = FNV_BASIS;

    for (size_t bi = 0; bi < sizeof bases / sizeof bases[0]; bi++)
    {
        uint64_t start = bases[bi] + rnd() % 1000000;
        uint64_t span  = 1000000 + rnd() % 30000000;

        uint64_t pts[24], got[24];

        size_t np = 24;

        for (size_t i = 0; i < np; i++)
            pts[i] = start + rnd() % (span + 1);

        pts[5]  = start;
        pts[11] = pts[10];

        qsort(pts, np, sizeof *pts, cmp_u64);

        crible_status st = crible_count_points(start, pts, np, got);

        CHECK(st == CRIBLE_OK, "count_points haut : statut %d", (int)st);

        for (size_t i = 0; i < np; i++)
        {
            uint64_t e = count(start, pts[i]);

            CHECK(got[i] == e, "count_points [%" PRIu64 ", %" PRIu64
                  "] = %" PRIu64 ", count_primes %" PRIu64,
                  start, pts[i], got[i], e);

            h = mix(h, got[i]);
        }
    }

    /* n = 0 : rien n'est lu ni ecrit. */
    CHECK(crible_count_points(0, NULL, 0, NULL) == CRIBLE_OK,
          "count_points n = 0");

    digest("count_points haut", h);
}

/* --- crible_foreach_seg ----------------------------------------------- */

typedef struct
{
    uint64_t lo, hi;

    uint64_t n;
    uint64_t last;
    uint64_t hash;

    int ordered;
    int in_range;

    int      test_primality;
    int      all_prime;

    uint64_t stop_after;
    int      stopped;
    int      calls_after_stop;
} walk_t;

static void walk_init(walk_t *w, uint64_t lo, uint64_t hi)
{
    memset(w, 0, sizeof *w);

    w->lo        = lo;
    w->hi        = hi;
    w->hash      = FNV_BASIS;
    w->ordered   = 1;
    w->in_range  = 1;
    w->all_prime = 1;
}

static bool walk_cb(const uint64_t *v, size_t n, void *ctx)
{
    walk_t *w = ctx;

    if (w->stopped)
        w->calls_after_stop++;

    for (size_t i = 0; i < n; i++)
    {
        uint64_t x = v[i];

        if (w->n && x <= w->last)
            w->ordered = 0;

        if (x < w->lo || x > w->hi)
            w->in_range = 0;

        if (w->test_primality && !is_prime(x))
            w->all_prime = 0;

        w->hash = mix(w->hash, x);
        w->last = x;
        w->n++;
    }

    if (w->stop_after && w->n >= w->stop_after)
    {
        w->stopped = 1;

        return false;
    }

    return true;
}

static const uint32_t seg_sizes[] = { 0, 32768, 65536, 1048576, 100 };

static void test_foreach_naive(int iters)
{
    uint64_t h = FNV_BASIS;

    for (int it = 0; it < iters; it++)
    {
        uint64_t a = rnd() % (NAIVE_N + 1);
        uint64_t b = a + rand_len(NAIVE_N);

        if (b > NAIVE_N)
            b = NAIVE_N;

        if (it % 7 == 0)
            a = rnd() % 150;

        uint32_t seg = seg_sizes[rnd() % 5];

        walk_t w;

        walk_init(&w, a, b);

        crible_status st = crible_foreach_seg(a, b, 1, seg, walk_cb, &w);

        uint64_t eh = FNV_BASIS, en = 0;

        for (uint64_t x = a; x <= b; x++)
        {
            if (isp[x])
            {
                eh = mix(eh, x);
                en++;
            }
        }

        CHECK(st == CRIBLE_OK && w.n == en && w.hash == eh && w.ordered,
              "foreach_seg [%" PRIu64 ", %" PRIu64 "] seg=%u : %" PRIu64
              " premiers, crible naif %" PRIu64 "%s", a, b, seg, w.n, en,
              w.n == en ? ", suite differente" : "");

        h = mix(h, w.hash);
    }

    digest("foreach_seg naif", h);
}

/* Le rappel interrompt le parcours : statut CRIBLE_OK, plus aucun appel. */
static void test_foreach_stop(int iters)
{
    for (int it = 0; it < iters; it++)
    {
        uint64_t a = rnd() % (NAIVE_N / 2);
        uint64_t b = a + NAIVE_N / 2;

        walk_t w;

        walk_init(&w, a, b);

        w.stop_after = 1 + rnd() % 60000;

        crible_status st =
            crible_foreach_seg(a, b, 1, seg_sizes[rnd() % 5], walk_cb, &w);

        uint64_t total = naive(a, b);

        CHECK(st == CRIBLE_OK && w.calls_after_stop == 0 && w.ordered &&
              w.in_range &&
              (w.stopped ? w.n >= w.stop_after : w.n == total),
              "foreach_seg interrompu apres %" PRIu64 " : %" PRIu64
              " recus, %d appel(s) de trop", w.stop_after, w.n,
              w.calls_after_stop);
    }
}

static void test_foreach_high(void)
{
    static const uint64_t bases[] =
    {
        1000000000000ULL, 30000000000000ULL, 1000000000000000ULL,
        9999999990000000ULL,
    };

    uint64_t h = FNV_BASIS;

    for (size_t bi = 0; bi < sizeof bases / sizeof bases[0]; bi++)
    {
        for (int s = 0; s < 2; s++)
        {
            uint64_t a = bases[bi] + rnd() % 1000000;
            uint64_t b = a + 200000 + rnd() % 1500000;

            walk_t w;

            walk_init(&w, a, b);

            w.test_primality = 1;

            crible_status st =
                crible_foreach_seg(a, b, 1, s ? 32768 : 0, walk_cb, &w);

            uint64_t e = count(a, b);

            CHECK(st == CRIBLE_OK && w.n == e && w.ordered && w.in_range &&
                  w.all_prime,
                  "foreach_seg haut [%" PRIu64 ", %" PRIu64 "] : %" PRIu64
                  " recus, count_primes %" PRIu64 "%s%s%s", a, b, w.n, e,
                  w.ordered ? "" : ", desordre",
                  w.in_range ? "" : ", hors intervalle",
                  w.all_prime ? "" : ", un compose");

            h = mix(h, w.hash);
        }
    }

    /* Cas sans appel du rappel. */
    walk_t w;

    walk_init(&w, 0, 0);

    CHECK(crible_foreach_seg(10, 1, 1, 0, walk_cb, &w) == CRIBLE_OK &&
          w.n == 0, "foreach_seg start > stop");
    CHECK(crible_foreach_seg(0, 100, 7, 0, walk_cb, &w) == CRIBLE_OK &&
          w.n == 0, "foreach_seg k = 7");
    CHECK(crible_foreach_seg(24, 28, 1, 0, walk_cb, &w) == CRIBLE_OK &&
          w.n == 0, "foreach_seg [24, 28]");

    digest("foreach_seg haut", h);
}

/* --- Appels simultanes depuis plusieurs threads ----------------------- */

enum { JOBS = 8, JOB_POINTS = 40 };

typedef struct
{
    uint64_t start;
    uint64_t span;
    uint64_t pts[JOB_POINTS];
    uint64_t got[JOB_POINTS];
    uint64_t walk_n;
    uint64_t walk_hash;
    int      ok;
} job_t;

static void *job_run(void *arg)
{
    job_t *j = arg;

    walk_t w;

    j->ok = 1;

    for (int round = 0; round < 3; round++)
    {
        if (crible_count_points(j->start, j->pts, JOB_POINTS, j->got)
            != CRIBLE_OK)
        {
            j->ok = 0;
        }

        walk_init(&w, j->start, j->start + j->span);

        if (crible_foreach_seg(j->start, j->start + j->span, 1, 32768,
                               walk_cb, &w) != CRIBLE_OK || !w.ordered)
        {
            j->ok = 0;
        }
    }

    j->walk_n    = w.n;
    j->walk_hash = w.hash;

    return NULL;
}

static void test_concurrent(void)
{
    job_t par[JOBS], seq[JOBS];

    pthread_t th[JOBS];

    for (int i = 0; i < JOBS; i++)
    {
        memset(&par[i], 0, sizeof par[i]);

        par[i].start = (i % 2 ? 1000000000000ULL : 50000000ULL)
                     + (uint64_t)i * 123456789ULL;
        par[i].span  = 3000000 + rnd() % 3000000;

        for (int k = 0; k < JOB_POINTS; k++)
            par[i].pts[k] = par[i].start + rnd() % (par[i].span + 1);

        qsort(par[i].pts, JOB_POINTS, sizeof par[i].pts[0], cmp_u64);

        seq[i] = par[i];
    }

    int created = 0;

    for (int i = 0; i < JOBS; i++)
    {
        if (pthread_create(&th[i], NULL, job_run, &par[i]))
            break;

        created++;
    }

    for (int i = 0; i < created; i++)
        pthread_join(th[i], NULL);

    CHECK(created == JOBS, "%d thread(s) crees sur %d", created, JOBS);

    uint64_t h = FNV_BASIS;

    for (int i = 0; i < created; i++)
    {
        job_run(&seq[i]);

        int same = par[i].ok && seq[i].ok
                && par[i].walk_n == seq[i].walk_n
                && par[i].walk_hash == seq[i].walk_hash
                && !memcmp(par[i].got, seq[i].got, sizeof par[i].got);

        CHECK(same, "appels simultanes, travail %d : resultats differents "
              "du passage sur un thread", i);

        uint64_t e = count(seq[i].start, seq[i].start + seq[i].span);

        CHECK(seq[i].walk_n == e, "appels simultanes, travail %d : %" PRIu64
              " premiers parcourus, count_primes %" PRIu64, i,
              seq[i].walk_n, e);

        h = mix(h, seq[i].walk_hash);

        for (int k = 0; k < JOB_POINTS; k++)
            h = mix(h, seq[i].got[k]);
    }

    digest("appels simultanes", h);
}

/* --- Limites propres a roue12 ----------------------------------------- */

static void test_unsupported(void)
{
    const uint64_t over = ROUE12_MAX_LIMIT + 1;

    uint64_t r = 123;

    CHECK(crible_count_primes(0, over, &r) == CRIBLE_EUNSUPPORTED && r == 0,
          "count_primes au-dela de 10^16");

    uint64_t pts[2] = { 100, over }, got[2] = { 7, 7 };

    CHECK(crible_count_points(0, pts, 2, got) == CRIBLE_EUNSUPPORTED,
          "count_points au-dela de 10^16");

    walk_t w;

    walk_init(&w, 0, over);

    CHECK(crible_foreach_seg(over - 10, over, 1, 0, walk_cb, &w)
          == CRIBLE_EUNSUPPORTED && w.n == 0,
          "foreach_seg au-dela de 10^16");
    CHECK(crible_foreach_seg(0, 100, 2, 0, walk_cb, &w)
          == CRIBLE_EUNSUPPORTED && w.n == 0,
          "foreach_seg k = 2");

    /* La borne elle-meme est admise. */
    uint64_t e = 0;

    for (uint64_t x = ROUE12_MAX_LIMIT - 1000; x <= ROUE12_MAX_LIMIT; x++)
        e += (uint64_t)is_prime(x);

    CHECK(crible_count_primes(ROUE12_MAX_LIMIT - 1000, ROUE12_MAX_LIMIT, &r)
          == CRIBLE_OK && r == e, "count_primes [10^16 - 1000, 10^16] = %"
          PRIu64 ", Miller-Rabin %" PRIu64, r, e);
}

int main(int argc, char **argv)
{
    int ref = argc > 1 && !strcmp(argv[1], "ref");

    build_naive();

    test_count_refs();
    test_count_random(300);
    test_threads();

    test_points_naive(150);
    test_points_high();

    test_foreach_naive(200);
    test_foreach_stop(40);
    test_foreach_high();

    test_concurrent();

    if (!ref)
        test_unsupported();

    printf("\n%d controle(s), %d echec(s)\n", checks, failures);

    return failures ? 1 : 0;
}
