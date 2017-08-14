/**
 * bound.c
 * Author: Thomas Delacour
 * Description: Implementation of error bounding as described in 
 * the May 2007 Paris Traceroute workshop and April 2009 Infocom papers. 
 * Detailed explanation can be found at www.paris-traceroute.net/publications.
 */

#include <stdbool.h> // bool
#include <stdio.h>   // fprintf, sscanf
#include <stdlib.h>  // malloc, calloc, free
#include <string.h>  // memset
#include <math.h>    // pow

#include "bound.h"

// We condiser a set of diagonal vectors (indexed by i) made of several cells (indexed by j)
#define PROBA_HOR(i, j)    ((long double)(j) / (i))              // Probability to follow a horizontal transition
#define PROBA_VER(i, j)    ((long double)((i) - (j) + 1) / (i)) // Probability to follow a vertical transition
#define NUM_PROBES(i, j)   ((i) + (j) - 1)                      // Translate position (i, j) into the corresponding number of probes
#define HSTART             2                                    // First two hypothesis (0 or 1 interfaces) are ignored

//--------------------------------------------------------------------------
// bound_state_t
//--------------------------------------------------------------------------

static bound_state_t * bound_state_create(size_t max_interfaces)
{
    bound_state_t * state;
    
    if (!(state = malloc(sizeof(bound_state_t)))) {
        goto ERR_STATE_MALLOC;
    }

    // Create parallel vectors contained in state
    if (!(state->first = malloc(max_interfaces * sizeof(probability_t)))) {
        goto ERR_FIRST_VECTOR;
    }

    if (!(state->second = malloc(max_interfaces * sizeof(probability_t)))) {
        goto ERR_SECOND_VECTOR;
    }

    return state;

    ERR_SECOND_VECTOR:
        free(state->first);
    ERR_FIRST_VECTOR:
        free(state);
    ERR_STATE_MALLOC:
        return NULL;
}

static void bound_state_free(bound_state_t * bound_state) 
{
    if (bound_state) {
        if (bound_state->first)  free(bound_state->first);
        if (bound_state->second) free(bound_state->second);

        free(bound_state);
    }
}

//--------------------------------------------------------------------------
// bound_t
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
// private helper functions
//--------------------------------------------------------------------------

/**
 * \brief The inverse of the stopping condition, which is:
 * if at a state that is on the same vertical level as the stopping point to
 * be computed, AND the sum of the current state + the probabilities of
 * reaching a failing state beforehand is less than or equal to the given
 * confidence.
 */

inline static bool continue_condition(
    size_t        jstart, 
    size_t        hypothesis, 
    long double   cur_state, 
    bound_t     * bound
) {
    bool ret = true;
    
    if (jstart == hypothesis - 1) {
        if (cur_state <= bound->ak_table[hypothesis]) {
            ret = false;
            bound->pr_failure[hypothesis] = cur_state;
        }
    }
    return ret;
}

/**
 * \brief Calculation of a single state. Calculation is the summation of two
 * parts: 1) the probability of reaching the state from a horizontal move
 *        2) the probability of reaching the state from a vertical move
 */

inline static probability_t calculate(bound_state_t * bound_state, size_t hypothesis, size_t j) {
    return bound_state->first[j]
         * PROBA_HOR(hypothesis, j)   //1
         + bound_state->second[j - 1]
         * PROBA_VER(hypothesis, j);  //2
}

inline static void swap(bound_state_t * bound_state) {
    probability_t * temp = bound_state->first;
    bound_state->first = bound_state->second;
    bound_state->second = temp;
}

/**
 * \brief For each new hypothesis' state space, initialize dummy "first" 
 * vector to all with probability_t 0.0 and initialize second vector
 * with probability 1.0 at first reachable  state - state(1,1)
 */

static probability_t init_state(bound_t * bound, bound_state_t * bound_state) {
    size_t j;

    for (j = 0; j < bound->max_n; ++j) {
        bound_state->first[j] = 0.0;
    }

    bound_state->second[0] = 0.0;
    bound_state->second[1] = 1.0;

    return 1.0;
}

/**
 * \brief Calculate confidence required at each branching point given 
 * graph-wide confidence and max assumed branching points
 */

// NOTE: this comes from (10) in the 2009 MDA paper
// Basically: calculates B*_k_i given B*_all and a bound on total number of
// load balancers in the graph
static double node_confidence(double graph_confidence, size_t max_branch)
{
    double ret;
    double power = 1.0 / max_branch;
    
    ret = pow((1 - graph_confidence), power); 
    return 1 - ret;
} 

/**
 * \brief Pre-compute significance levels following procedure derived from 
 * equations (8) and (9) in the 2009 MDA paper
 */
static void bound_init_aks(bound_t * bound) 
{
    size_t i;
    double r = 0.9; // Section III.B of 2009 MDA paper finds this to be a reasonable value
    double a1 = (1 - r) * bound->confidence; 

    // To stay consistent with nk table, set the first two "dummy states" to zero
    bound->ak_table[0] = 0.0;
    bound->ak_table[1] = 0.0;
   
    // Annoyingly, the index shift rightwards in the MDA implementation means that a1
    // maps to index 2...
    bound->ak_table[2] = a1; 
                         
    for (i = 3; i < bound->max_n + 1; i++) {
        // From (8) in 2009 MDA paper. Minus 2 because, again, the indexes are one larger 
        // than the corresponding paper indexes
        bound->ak_table[i] = a1 * pow(r, i - 2);
    }     
}

/**
 * \brief Reallocate memory in bound structure given new Hypothesis 
 * range (Param new_max_n)
 */

static bool reallocate_bound(bound_t * bound, size_t new_max_n)
{
    bound_state_t * bound_state = bound->state;

    // Reallocate state space memory
    if (!(bound_state->first = realloc(
                       bound_state->first, 
                       (new_max_n * sizeof(probability_t))
                       ))){
        goto ERR_REALLOC;
    }
    if (!(bound_state->second = realloc(
                       bound_state->second, 
                       (new_max_n * sizeof(probability_t))
                       ))){
        goto ERR_REALLOC;
    }

    // Reallocate nk, pk, and failure pr table memory
    if (!(bound->nk_table = realloc(
                 bound->nk_table,
                 ((new_max_n + 1) * sizeof(size_t))
                 ))){
        goto ERR_REALLOC;
    } 
    if (!(bound->ak_table = realloc(
                 bound->ak_table,
                 ((new_max_n + 1) * sizeof(probability_t))
                 ))){
        goto ERR_REALLOC;
    }
    if (!(bound->pr_failure = realloc(
                 bound->pr_failure,
                 ((new_max_n + 1) * sizeof(probability_t))
                 ))){
        goto ERR_REALLOC;
    }


    return true;

    ERR_REALLOC:
        fprintf(stderr, "Error allocating resources for new hypothesis\n");
        return false;
}

bound_t * bound_create(double confidence, size_t max_interfaces, size_t max_branch)
{
    bound_t * bound;

    // Allocate and populate bound_t structure
    if (!(bound = malloc(sizeof(bound_t)))) goto ERR_BOUND_MALLOC;

    bound->confidence = node_confidence(confidence, max_branch);
    bound->max_n      = max_interfaces;

    // Create parrallel tables to store stopping points and associated
    // probabilities. Note: of size + 1 because table ranges from 0 - 16
    // (max_interfaces), which is size 17.
    if (!(bound->nk_table = calloc((max_interfaces + 1), sizeof(size_t)))) {
        goto ERR_NK_TABLE_MALLOC;
    }

    if (!(bound->ak_table = calloc((max_interfaces + 1), sizeof(probability_t)))) {
        goto ERR_PK_TABLE_MALLOC;
    }

    if (!(bound->pr_failure = calloc((max_interfaces + 1), sizeof(probability_t)))) {
        goto ERR_PR_FAILURE;
    }   

    if (!(bound->state = bound_state_create(max_interfaces))) {
        goto ERR_STATE;
    }

    // First two hypotheses represent impossible "dummy" states
    bound->nk_table[0] = 0.0;
    bound->nk_table[1] = 0.0;
    bound_init_aks(bound);            // Pre-compute significance levels (aks)
    bound_build(bound, bound->max_n); // Calculate stopping points

    return bound;

    ERR_STATE:
        free(bound->pr_failure);
    ERR_PR_FAILURE:
        free(bound->ak_table);
    ERR_PK_TABLE_MALLOC:
        free(bound->nk_table);
    ERR_NK_TABLE_MALLOC:
        free(bound);
    ERR_BOUND_MALLOC:
        return NULL;
}

void bound_build(bound_t * bound, size_t end)
{
    size_t          hypothesis, i, j = 0, jstart; // Note: j does not require to be set to 0
    bound_state_t * state;
    probability_t   cur_state;

    // Handle potential nulls
    if (!bound || !(bound->nk_table) || !(bound->state)) {
        goto ERR_NULL_ARG;
    }
    state      = bound->state;
    hypothesis = HSTART;

    // If expanding the build to higher hypotheses
    if (end > bound->max_n) {
        if (reallocate_bound(bound, end)) {
            hypothesis   = bound->max_n + 1; // Begin at next hypothesis
            bound->max_n = end;
            bound_init_aks(bound); // More significance levels will need to be pre-computed
        }
    }

    for ( ; hypothesis <= bound->max_n; ++hypothesis) {
        cur_state = init_state(bound, state);
        jstart = 2;
        
        // Walk horizontally accross state space
        for (i = 1; continue_condition(jstart, hypothesis, cur_state, bound); ++i) {

            // Compute values and fill vector (vertically)
            for (j = jstart; j < hypothesis; ++j) {
                cur_state = calculate(state, hypothesis, j); 

                // If at a previously computed stopping point, enter
                // unreachable state (probability 0). 
                if (NUM_PROBES(i, j) == (bound->nk_table)[j + 1]) {
                    jstart = j + 1;
                    state->second[j] = 0.0;
                    state->first[j] = 0.0;
                } else {
                    state->second[j] = cur_state;
                }
            }
            jstart = (i == 1) ? 1 : jstart; // Awkward check required to get around state(1, 1) = 1.0 necessarily
            swap(state);
        }
        bound->nk_table[hypothesis] = NUM_PROBES(i, j) - 2; // Sub 2 for increment of i and j after stopping condition
    }
    return;

    ERR_NULL_ARG:
        fprintf(stderr, "Provided bound struct contained null values or was itself null\n");
}

size_t bound_get_nk(bound_t * bound, size_t k)
{
    size_t ret = 0;

    if (bound) {
        if ((bound->nk_table) && (bound->max_n >= k)) {
            ret = (bound->nk_table)[k];
        }
    }
    return ret;
}

void bound_failure_dump(bound_t * bound)
{
    size_t i;

    printf("Expected failure:\n");
    for (i = 0; i <= bound->max_n; ++i) {
        printf("%zu - %Lf\n", i, bound->pr_failure[i]);    
    }
}

void bound_dump(bound_t * bound) 
{
    size_t i;

    for (i = 0; i <= bound->max_n; i++) {
        printf("%zu - %zu\n", i, bound_get_nk(bound, i));
    }
}

void bound_free(bound_t * bound)
{
    if (bound) {
        //if (bound->pk_table) free(bound->pk_table);
        if (bound->ak_table) free(bound->ak_table);
        if (bound->nk_table) free(bound->nk_table);
        if (bound->pr_failure) free(bound->pr_failure);
        if (bound->state)    bound_state_free(bound->state);
        free(bound);
    }
}

int main(int argc, const char * argv[]) {
    long double confidence;
    size_t      interfaces;
    size_t      max_branch;

    confidence = .05;
    interfaces = 16;
    max_branch = 1;
    bound_t * bound = bound_create(confidence, interfaces, max_branch);
    bound_dump(bound);
    bound_failure_dump(bound);
    bound_free(bound);
    return 0;
}

