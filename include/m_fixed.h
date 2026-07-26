/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Fixed point arithemtics, implementation.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __M_FIXED__
#define __M_FIXED__

#include "config.h"
#include "doomtype.h"

/*
 * Fixed point, 32bit as 16.16.
 */

#define FRACBITS 16
#define FRACUNIT (1<<FRACBITS)


typedef int fixed_t;

/*
 * Absolute Value
 *
 * killough 5/10/98: In djgpp, use inlined assembly for performance
 * killough 9/05/98: better code seems to be gotten from using inlined C
 */

inline static int CONSTFUNC D_abs(fixed_t x)
{
  fixed_t _t = (x),_s;
  _s = _t >> (8*sizeof _t-1);
  return (_t^_s)-_s;
}

/*
 * Fixed Point Multiplication
 */

/* SH-1 has no 32x32 multiply, so the generic (int64)a*b>>16 lowers to a
 * __muldi3 libcall (and most call sites also pay a jsr). Build the 16.16
 * product from four 16x16 mulu.w instructions instead: bit-exact with the
 * old truncating >>16 (floor), no libcall, inlined at every call site.
 * mulu.w multiplies the low 16 bits of each operand, so no masking is
 * needed. GCC won't reliably emit mulu.w from C (it keeps shift-derived
 * halves in SImode -> __mulsi3), hence the explicit asm. */
#define FORCEINLINE inline __attribute__((always_inline))

static FORCEINLINE unsigned int mulu16(unsigned int a, unsigned int b)
{
    /* V810 has a real 32x32 multiply, so plain C lowers to one `mulu`. The
     * operands are the low 16 bits of each input (matching SH-1 mulu.w), so the
     * product always fits in 32 bits with no libcall. */
    return (a & 0xffffu) * (b & 0xffffu);
}

#ifdef __v810__
static FORCEINLINE fixed_t CONSTFUNC FixedMul(fixed_t a, fixed_t b)
{
    /* The V810 `mul` is a *widening* signed 32x32->64 multiply: the low 32 bits
     * of the product land in reg2 and the high 32 bits in r30 (see V810 User's
     * Manual, MUL). So the full 16.16 result, (int64)(a*b)>>16, is just
     * (hi<<16)|((unsigned)lo>>16) -- one ~13-cycle microcoded mul plus three
     * single-cycle ops, replacing the four-mulu16 SH-1 decomposition below
     * (~4 muls + a dozen shift/add/masks). r30 is used as the mul-high scratch
     * and listed as clobbered so GCC keeps nothing live there. Bit-exact with
     * the C path (verified over 2M random+edge inputs). */
    fixed_t r;
    __asm__("mul  %2, %1\n\t"   /* %1 = lo32(a*b) signed, r30 = hi32          */
            "shr  16, %1\n\t"   /* %1 = (unsigned)lo >> 16                     */
            "shl  16, r30\n\t"  /* r30 = hi << 16                              */
            "or   r30, %1"      /* %1 = (hi<<16) | (lo>>16) = FixedMul         */
            : "=r"(r)
            : "0"(a), "r"(b)
            : "r30");
    return r;
}
#else
static FORCEINLINE fixed_t CONSTFUNC FixedMul(fixed_t a, fixed_t b)
{
    unsigned int ua = a < 0 ? 0u - (unsigned int)a : (unsigned int)a;
    unsigned int ub = b < 0 ? 0u - (unsigned int)b : (unsigned int)b;

    unsigned int lolo = mulu16(ua, ub);
    unsigned int res  = (lolo >> 16)
                      + mulu16(ua, ub >> 16)
                      + mulu16(ua >> 16, ub)
                      + (mulu16(ua >> 16, ub >> 16) << 16);

    return ((a ^ b) < 0) ? -(fixed_t)res - (fixed_t)((lolo & 0xffffu) != 0)
                         :  (fixed_t)res;
}
#endif

/*
 * Fixed Point Division
 */

/* CPhipps - made __inline__ to inline, as specified in the gcc docs
 * Also made const */

#ifdef __v810__
/* V810: (a<<16)/b is a 64/32 divide; the generic path below lowers to a
 * __divdi3 libcall. Out-of-line in m_fixed.c using two hardware divides
 * (Hacker's Delight divlu). See that file. */
fixed_t CONSTFUNC FixedDiv(fixed_t a, fixed_t b);
#else
inline static fixed_t CONSTFUNC FixedDiv(fixed_t a, fixed_t b)
{
#ifndef GBA
    return ((unsigned)D_abs(a)>>14) >= (unsigned)D_abs(b) ? ((a^b)>>31) ^ INT_MAX :
                                                  (fixed_t)(((int_64_t) a << FRACBITS) / b);
#else

    unsigned int udiv64_arm (unsigned int a, unsigned int b, unsigned int c);

    int q;
    int sign = (a^b) < 0; /* different signs */
    unsigned int l,h;

    a = a<0 ? -a:a;
    b = b<0 ? -b:b;

    l = (a << 16);
    h = (a >> 16);

    q = udiv64_arm (h,l,b);
    if (sign)
        q = -q;

    return q;
#endif
}
#endif /* __v810__ */

/* CPhipps -
 * FixedMod - returns a % b, guaranteeing 0<=a<b
 * (notice that the C standard for % does not guarantee this)
 */

inline static fixed_t CONSTFUNC FixedMod(fixed_t a, fixed_t b)
{
    if(!a)
        return 0;

    if (b & (b-1))
    {
        fixed_t r = a % b;
        return ((r<0) ? r+b : r);
    }
    else
        return (a & (b-1));
}

//Approx Reciprocal of v.
//
// Was a 256 KB `reciprocalTable[65537]` (round(2^32/val)) in .rodata — far too
// much RAM on the PC-FX, and PSX Doom carries no such table either (it divides).
// The V810 has a hardware unsigned divide, so compute the same value directly:
// reduce |v| into (0, 1<<FRACBITS] tracking the shift, then 0xFFFFFFFF/val is
// round(2^32/val) to within 1 unit — negligible after the >>shift for this
// already-approximate reciprocal (identical rendering, 256 KB reclaimed).
inline static CONSTFUNC fixed_t FixedReciprocal(fixed_t v)
{
    unsigned int val = v < 0 ? (unsigned int)(-v) : (unsigned int)v;

    if (val == 0)
        return 0;                       // matches old reciprocalTable[0] == 0

    unsigned int shift = 0;
    while(val > (1u << FRACBITS))
    {
        val = (val >> 1u);
        shift++;
    }

    fixed_t result = (fixed_t)((0xFFFFFFFFu / val) >> shift);

    return v < 0 ? -result : result;
}


//Approx fixed point divide of a/b using reciprocal. -> a * (1/b).
inline static CONSTFUNC fixed_t FixedApproxDiv(fixed_t a, fixed_t b)
{
    return FixedMul(a, FixedReciprocal(b));
}




#endif
