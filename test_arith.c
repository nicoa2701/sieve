/*
 * Les deux calculs de l'activation qui frolent 2^64, contre une reference sur
 * 128 bits. Le fichier inclut main12.c pour atteindre ses fonctions statiques.
 *
 * Un crible pres de 2^64 demande tous les premiers jusqu'a 2^32 ; ce controle
 * n'en demande aucun et passe en une seconde sur n'importe quelle machine. Il
 * ne dit rien du crible lui-meme : c'est `test_api haut` qui le dit.
 */
#define ROUE12_LIB
#include "main12.c"

#include <inttypes.h>

typedef unsigned __int128 u128;

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;

    return rng_state * 0x2545F4914F6CDD1DULL;
}

int main(void)
{
    tables_init();

    uint64_t checks = 0, failures = 0, wrapped = 0, wrapped_sum = 0;

    for (int it = 0; it < 20000000; it++)
    {
        /* Un premier de crible : impair, de 7 a 2^32 - 1, souvent grand. */
        uint32_t p = (uint32_t)(rnd() >> (32 + rnd() % 29)) | 1u;

        if (p < 7)
            p = 7;

        /* Une borne basse : partout, et surtout a moins de 12 p de 2^64. */
        uint64_t low;

        switch (it % 4)
        {
        case 0:  low = rnd(); break;
        case 1:  low = UINT64_MAX - rnd() % ((uint64_t)12 * p + 1); break;
        case 2:  low = UINT64_MAX - rnd() % 64; break;
        default: low = UINT64_MAX - (rnd() >> (rnd() % 64)); break;
        }

        if (low == 0)
            low = 1;

        /* Plafond de low / p. */
        const u128 m_ref = ((u128)low + p - 1) / p;
        const uint64_t m = ceil_div_u32(low, p);

        checks++;

        if ((u128)m != m_ref)
        {
            if (failures++ < 10)
                printf("ECHEC  plafond de %" PRIu64 " / %u\n", low, p);

            continue;
        }

        wrapped_sum += ((u128)low + p - 1) >> 64 ? 1 : 0;

        /* Le cofacteur arrondi sur la roue 210, puis l'octet du multiple. */
        uint64_t cof = m < p ? p : m;
        uint64_t q   = cof / 210;
        unsigned j   = next210[cof % 210];

        if (j == 48)
        {
            q++;
            j = 0;
        }

        const unsigned R = residues210[j];

        const u128 product  = (u128)p * ((u128)210 * q + R);
        const u128 byte_ref = product / 30;

        checks++;

        if (product >> 64)
            wrapped++;

        if (byte_ref >> 64 ||
            byte_of_multiple210(p, q, R) != (uint64_t)byte_ref ||
            product < low)
        {
            if (failures++ < 10)
                printf("ECHEC  octet du multiple, low = %" PRIu64 ", p = %u\n",
                       low, p);
        }
    }

    printf("%" PRIu64 " controle(s), %" PRIu64 " echec(s)\n", checks, failures);
    printf("dont %" PRIu64 " cas ou low + p - 1 depasse 2^64, et %" PRIu64
           " ou le multiple lui-meme le depasse\n", wrapped_sum, wrapped);

    return failures ? 1 : 0;
}
