/*
 * roue12 en bibliotheque : les cinq fonctions de crible.h dont compte se sert,
 * sous les memes noms et les memes signatures, portees par le moteur de
 * roue12. Un programme ecrit contre crible.h se relie a libroue12.a sans
 * changer une ligne de source ; les deux bibliotheques ne peuvent donc pas
 * etre liees dans le meme binaire.
 *
 * Edition des liens : libroue12.a -fopenmp -lm.
 *
 * Ce qui differe de crible :
 *   - la borne haute est 10^16 et non 2^64 - 1 : au-dela, les fonctions
 *     rendent CRIBLE_EUNSUPPORTED sans rien calculer ;
 *   - crible_foreach_seg ne parcourt que les premiers (k = 1) ; k = 2..6 rend
 *     CRIBLE_EUNSUPPORTED ;
 *   - crible_count_points et crible_foreach_seg ne lancent jamais de thread,
 *     quel que soit segment_bytes : tout se fait sur le thread appelant. Leur
 *     dimensionnement suppose crible_get_threads() appels simultanes, un par
 *     thread de l'application ;
 *   - crible_foreach_seg livre des lots d'au plus 32 768 premiers, plusieurs
 *     par segment, et prend segment_bytes pour un plancher : roue12 choisit
 *     son segment et ne descend pas en dessous.
 *
 * Les premiers de base sont gardes d'un appel a l'autre et partages entre les
 * threads : une seule table, la plus large demandee jusqu'ici, au plus 23 Mo
 * (les premiers jusqu'a 10^8). Elle n'est rendue qu'a la fin du processus.
 *
 * Toutes les fonctions sont sures en contexte multi-thread, sauf
 * crible_set_threads, qui modifie un reglage global.
 *
 * La garde est celle de crible.h : inclure les deux en-tetes dans la meme
 * unite ne redefinit rien, le premier inclus l'emporte.
 */
#ifndef CRIBLE_H
#define CRIBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memes valeurs que crible.h ; CRIBLE_EUNSUPPORTED est propre a roue12. */
typedef enum {
    CRIBLE_OK = 0,
    CRIBLE_ENOMEM = 1,       /* echec d'allocation */
    CRIBLE_EOVERFLOW = 2,    /* jamais rendu par roue12 */
    CRIBLE_EUNDERFLOW = 3,   /* jamais rendu par roue12 */
    CRIBLE_EINTERNAL = 4,    /* jamais rendu par roue12 */
    CRIBLE_EUNSUPPORTED = 5  /* borne au-dela de 10^16, ou k-uplets (k >= 2) */
} crible_status;

/* Borne haute admise par toutes les fonctions : 10^16. */
#define ROUE12_MAX_LIMIT 10000000000000000ULL

/*
 * Nombre de threads de crible_count_primes : 0 = tous les processeurs logiques
 * en ligne. crible_get_threads() rend le nombre effectif, ramene au nombre de
 * processeurs logiques s'il le depasse. C'est un plafond : un intervalle court
 * en occupe moins (un thread par 10^7 entiers).
 */
void     crible_set_threads(unsigned n);
unsigned crible_get_threads(void);

/*
 * Nombre de premiers de l'intervalle ferme [start, stop] ; 0 si start > stop.
 * Parallele, sur au plus crible_get_threads() threads OpenMP.
 */
crible_status crible_count_primes(uint64_t start, uint64_t stop,
                                  uint64_t *result);

/*
 * Comptes a plusieurs points en un seul crible, sur le thread appelant :
 * counts[i] = nombre de premiers de [start, points[i]], pour des points
 * croissants au sens large. L'ordre n'est pas verifie : des points mal ranges
 * donnent des comptes faux sans erreur.
 */
crible_status crible_count_points(uint64_t start, const uint64_t *points,
                                  size_t n, uint64_t *counts);

/*
 * Parcours des premiers de [start, stop] dans l'ordre croissant, par lots, sur
 * le thread appelant. Le rappel rend false pour interrompre le parcours ; le
 * statut reste CRIBLE_OK. values n'est valide que pendant l'appel.
 *
 * k doit valoir 1. segment_bytes est la taille minimale du segment en octets,
 * arrondie au Kio superieur ; roue12 dimensionne son segment d'apres les
 * caches et l'intervalle, entre 32 Kio et 64 Mio, sans descendre sous ce
 * plancher. 0 = pas de plancher. Ni les threads ni la taille des lots n'en
 * dependent.
 */
typedef bool (*crible_batch_cb)(const uint64_t *values, size_t n, void *ctx);

crible_status crible_foreach_seg(uint64_t start, uint64_t stop, unsigned k,
                                 uint32_t segment_bytes, crible_batch_cb cb,
                                 void *ctx);

#ifdef __cplusplus
}
#endif
#endif
