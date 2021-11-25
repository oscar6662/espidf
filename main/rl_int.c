/**********************************************************/
/*                                                        */
/*  "Rather Long" Integers for Collatz demo               */
/*                                                        */
/*  Esa Hyytiä, Nov 2021                                  */
/**********************************************************/
#include "rl_int.h"

// Use this to disable builtin function?
//undef __GNUC__

int rl_overflow;        // Set if rl_add or rl_f3n1 overflows (which is an error)

// Compare: "x-y"
int rl_cmp(const bigint_t *x, const bigint_t *y)
{
    int i = x->len;
    
    if ( i > y->len )  return  1;
    if ( i < y->len )  return -1;
    i--;
    while( x->a[i] == y->a[i] )
    {
        if ( !i )
            return 0;
        i--;
    }
    return x->a[i] > y->a[i] ? 1 : -1;
}

int rl_equal(const bigint_t *x, const bigint_t *y)
{
    if ( x->len != y->len )
        return 1;
    for(int i=0; i<x->len; i++)
        if ( x->a[i] != y->a[i] )
            return 1;
    return 0;
}


int rl_greater(const bigint_t *x, const bigint_t *y)
{
    int i = x->len;
    int j = y->len;
    
    if ( i > j )  return 1;
    if ( i < j )  return 0;
    i--;
    while( x->a[i] == y->a[i] && i )
        i--;
    return x->a[i] > y->a[i] ? 1 : 0;
}


/*
 *  single computing task - static buffer ok!?
 */
const char*rl_str(const bigint_t *x )
{
    static char buf[ MAX_BSTR ];
    
    int m = BLEN*x->len;    /* nr. of bits            */
    int b = 1 << ((m+3)&3); /* note the implicit MSBs */
    int v = 0;
    int len = 0;
    
    for(int i=x->len-1; i>=0; i--) 
    {
        uint32_t z = ((uint32_t)1) << (BLEN-1);  // the MSB
        do {
            v = v | (( x->a[i] & z ) ? b : 0);
            b = b >> 1;
            if ( !b )
            {
                if ( len || v ) /* skip the leading zeroes */
                    buf[len++] = ( v < 10 ? ('0'+v) : ('a'+v-10));
                b = 1 << 3;
                v = 0;
            }
            z = z >> 1;            
        } while( z );
    }
    if ( !len )
        buf[len++] = '0';
    buf[len] = '\0';
    //ASSERT( len < MAX_BSTR, "bint2str: buffer overflow :)" );
    return buf;
}

/*
 *  Actual operations
 */
void rl_set( bigint_t *x, const bigint_t *y)
{
    x->len = y->len;
    for(int i=0; i<y->len; i++)
        x->a[i] = y->a[i];
}

void rl_add( bigint_t *x, uint32_t c ) 
{
    uint32_t r;
    
    for(int i=0; i<x->len; i++)
    {
        r = x->a[i] + c;
        c = r >> BLEN;
        x->a[i] = r & MASK;
        if ( !c )
            return;
    }
    if ( x->len >= INT_LEN )
    {
        rl_overflow = 1;
    }
    else
    {        
        x->a[ x->len ] = c;
        x->len++;
    }
}


void rl_f3n1(bigint_t *x) 
{
    uint32_t r,c = 1;
    
    for(int i=0; i<x->len; i++)
    {
        r = x->a[i] + (x->a[i]<<1) + c;
        c = r >> BLEN;
        x->a[i] = r & MASK;
    }
    if ( c ) 
    {
        if ( x->len >= INT_LEN )
        {
            rl_overflow = 1;
        }
        else
        {
            x->a[ x->len ] = c;
            x->len++;
        }
    }
}


void rl_fdiv2(bigint_t *x) 
{
    uint32_t *n = x->a;
    
    // shift whole integers
#if INT_LEN > 1
    if ( n[0]==0 )
    {
        int i,k=1;
        while( n[k]==0 )
            k++;
        i=0;
        while( k < x->len )
            n[i++] = n[k++];
        x->len = i;
    }
#endif

    // shift the remaining bits
    {
        int k;
#if defined(__GNUC__)
        k = __builtin_ctzl( n[0] );  // here uint32_t so long
#else
        k = (n[0] & 1 ) ^ 1;
#endif
        if ( k )
        {
#if !defined(__GNUC__)
            uint32_t n0 = n[0] >> 1;
            while( !(n0&1) )
            {
                n0 = n0 >> 1;
                k++;
            }
#endif
            // k lowest bits are zero, shift them out!
#if INT_LEN > 1
            int p = BLEN-k;
            for(int i=1; i<x->len; i++)
                n[i-1] = (n[i]<<p & MASK) | (n[i-1]>>k);
#endif
            n[x->len-1] = (n[x->len-1]>>k);

            while( !n[ x->len-1 ] )
                x->len--;
        }
    }
}
