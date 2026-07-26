/* pcfx_sum32.c -- the CD asset integrity check (see pcfx_sum32.h for why it exists).
 *
 * Bit-identical to sum32() in tools/pcfx_sum32.py, which stamps the checksums into the
 * asset blobs at build time. If you change one, change the other.
 *
 * Out of line, not a static inline in the header: w_wad.c calls it from a dozen sites
 * and one copy of the loop is the difference between fitting the program image and not
 * (the ASSERT in platform/pcfx_hot.ld). Only ever reached at load time, so the call
 * overhead is irrelevant next to the CD read it is checking. */
#include "pcfx_sum32.h"

static unsigned mix(unsigned h, unsigned v)
{
    h ^= v;
    h = (h << 5) | (h >> 27);
    return h + v;
}

unsigned pcfx_sum32(const void *p, unsigned n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned h = 0x811c9dc5u ^ n;
    unsigned k = n >> 2;

    /* Aligned is the norm here (CD reads land on sector-aligned buffers, pack lumps are
     * 4-aligned), so take the word path when we can and assemble words by hand otherwise
     * — both must produce the same little-endian word stream. */
    if ((((unsigned)(unsigned long)b) & 3u) == 0)
    {
        const unsigned *w = (const unsigned *)p;
        while (k--)
            h = mix(h, *w++);
        b += (n & ~3u);
    }
    else
    {
        while (k--)
        {
            unsigned v = (unsigned)b[0] | ((unsigned)b[1] << 8)
                       | ((unsigned)b[2] << 16) | ((unsigned)b[3] << 24);
            h = mix(h, v);
            b += 4;
        }
    }

    if (n & 3u)
    {
        unsigned v = 0, i;
        for (i = 0; i < (n & 3u); i++)          /* tail bytes, zero-padded to a word */
            v |= (unsigned)b[i] << (i * 8);
        h = mix(h, v);
    }

    h ^= h >> 15;
    h *= 0x2545f491u;
    h ^= h >> 17;
    return h;
}
