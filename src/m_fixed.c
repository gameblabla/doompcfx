/* Out-of-line fixed-point helpers for the V810.
 *
 * FixedDiv computes (a<<16)/b, a 64-bit-dividend / 32-bit-divisor divide. The
 * portable header path lowers that to a __divdi3 libcall (a ~400-instruction
 * 64-bit division routine). The V810 has a hardware 32/32 divide (`divu`,
 * quotient in reg2 + remainder in r30) but no 64/32, and simple remainder
 * chaining overflows once the divisor exceeds 2^16 (the common case here --
 * divisors are distances >= 1.0). So we use Hacker's Delight's divlu (Knuth
 * Algorithm D specialised to a single-word quotient): normalise, then two
 * hardware `divu`s with the classic two-digit correction. Bit-exact with the
 * signed (int64)(a<<16)/b truncation (verified over 20M random + edge inputs).
 *
 * Out-of-line (one copy, ~150 instrs) rather than inlined at FixedDiv's ~38
 * call sites; none of them are per-column hot (view/light-table setup, per-seg,
 * per-sprite), so the call is free next to the divides it replaces.
 */

#include "doomtype.h"
#include "m_fixed.h"

#ifdef __v810__

/* number of leading zeros, 0..31 (x != 0). No V810 CLZ instruction, so a
 * branch-halving sequence. */
static int nlz32(unsigned int x)
{
    int n = 0;
    if (x <= 0x0000FFFFu) { n += 16; x <<= 16; }
    if (x <= 0x00FFFFFFu) { n += 8;  x <<= 8;  }
    if (x <= 0x0FFFFFFFu) { n += 4;  x <<= 4;  }
    if (x <= 0x3FFFFFFFu) { n += 2;  x <<= 2;  }
    if (x <= 0x7FFFFFFFu) { n += 1;             }
    return n;
}

/* floor((u1:u0) / v) for a 64-bit dividend and 32-bit divisor, result assumed
 * to fit in 32 bits (caller guarantees u1 < v). Two hardware divides. */
static unsigned int divlu(unsigned int u1, unsigned int u0, unsigned int v)
{
    const unsigned int b = 65536u;              /* number base (16 bits) */
    unsigned int vn1, vn0, un32, un21, un1, un0, q1, q0, rhat, un10;
    int s = nlz32(v);

    v <<= s;                                    /* normalise divisor */
    vn1 = v >> 16;
    vn0 = v & 0xFFFFu;

    un32 = (s == 0) ? u1 : ((u1 << s) | (u0 >> (32 - s)));
    un10 = u0 << s;
    un1  = un10 >> 16;
    un0  = un10 & 0xFFFFu;

    q1   = un32 / vn1;                          /* hardware divu */
    rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) { q1--; rhat += vn1; if (rhat >= b) break; }

    un21 = un32 * b + un1 - q1 * v;
    q0   = un21 / vn1;                          /* hardware divu */
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) { q0--; rhat += vn1; if (rhat >= b) break; }

    return q1 * b + q0;
}

fixed_t CONSTFUNC FixedDiv(fixed_t a, fixed_t b)
{
    unsigned int ua, ub;

    /* Saturate when the quotient wouldn't fit (also covers b == 0). Matches the
     * original: INT_MAX for like signs, INT_MIN for unlike. */
    if (((unsigned int)D_abs(a) >> 14) >= (unsigned int)D_abs(b))
        return ((a ^ b) >> 31) ^ INT_MAX;

    ua = (unsigned int)D_abs(a);
    ub = (unsigned int)D_abs(b);

    /* (ua << 16) as a 64-bit dividend: high word ua>>16, low word ua<<16. The
     * guard above ensures ua>>16 < ub, so the quotient fits in 32 bits. */
    {
        unsigned int q = divlu(ua >> 16, ua << 16, ub);
        return ((a ^ b) < 0) ? -(fixed_t)q : (fixed_t)q;
    }
}

#endif /* __v810__ */
