#ifndef RL_INT_H
#define RL_INT_H
/**********************************************************/
/*                                                        */
/*  "Rather Long" Integers for Collatz demo               */
/*                                                        */
/*  Esa Hyytiä, Nov 2021                                  */
/**********************************************************/
/*
 * Fixed size big integers:
 * - Fixed size
 * - basic type: 32bit uint, of which 30bit is the "payload"
 * - 10x30 = 300 bits
 */
#include <stdint.h>

#define INT_LEN  10
#define BLEN     30   // word size - 2
#define MASK     ((((uint32_t)1)<<BLEN) - 1)  /* BLEN bits set */
#define MAX_BSTR ((BLEN*INT_LEN+7)>>2)        /* max length of a hex string incl. null-terminator */

typedef struct 
{
    uint32_t  len; /* number of non-zero elements */
    uint32_t  a[ INT_LEN ];
} bigint_t;

/*
 * Global static variables
 */
extern int rl_overflow;        // Set if rl_add or rl_f3n1 overflows

/*
 *  Function prototypes
 */
int rl_cmp(    const bigint_t *x, const bigint_t *y);
int rl_equal(  const bigint_t *x, const bigint_t *y);
int rl_greater(const bigint_t *x, const bigint_t *y);

const char*rl_str(const bigint_t *x );  /* returns a local static array! */

void rl_set(   bigint_t *x, const bigint_t *y);
void rl_add(   bigint_t *x, uint32_t c );
void rl_f3n1(  bigint_t *x );
void rl_fdiv2( bigint_t *x );

#endif
