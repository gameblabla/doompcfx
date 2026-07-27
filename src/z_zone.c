// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Zone Memory Allocation. Neat.
//
//-----------------------------------------------------------------------------

#include "z_zone.h"
#include "doomdef.h"
#include "doomtype.h"
#include "lprintf.h"


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
//

#define ZONEID	0x1d4a11
#define MINFRAGMENT		64

// Zone size. Grown (960->1152 KB) now that gameplay assets are held resident
// (compressed) so the renderer never streams from the CD mid-frame: a level's
// whole compressed graphics working set (~0.6-0.75 MB) lives in PU_LEVEL. The
// freed .rodata SFX bank + shrunk staging + the previously-unused ~200 KB stack
// gap pay for it; __end still clears the descending stack with margin.
/* Grown 1440 -> 1512 KB (2026-07-13) using the ~84 KB reclaimed by compressing the weapon
 * sprite patterns to per-frame LZ4 (platform/pcfx_weapon.c, was 162 KB of flat RAM tables).
 * The extra heap lets EVERY E1 map's full asset pack fit one contiguous arena chunk — the
 * heaviest, E1M6, was ~64 KB over before — so no map falls to the slow scattered path and
 * there is no partial/CD-streamed pack. */
/* Code, generated VDC tables and fixed video scratch all live below this reserve.
 * Keep the linked BSS (including the weapon decode scratch/SAT shadow that follows
 * the zone) below the 2 MiB RAM ceiling with real descending-stack headroom.  The
 * renderer's cache-shaped hot section added 6 KiB to the resident image; retaining
 * the old zone sizes put the end of the weapon state at 0x2002b0, outside RAM. */
/* Trimmed 1498 -> 1492 KB (2026-07-26) to pay for the CD asset integrity checks in
 * w_wad.c (checksum verify + re-read from the disc's duplicate copy, so an imperfect
 * burn can no longer render corrupt sprites). That is ~6 KiB of resident code plus a
 * ~5 KiB resident per-lump checksum table, and the stack gap the linker script asserts
 * had only ~1 KiB spare. Verified after the trim: E1M6 — the heaviest map, the one this
 * zone size was tuned for — still fits its whole asset pack in ONE arena chunk
 * (w_narena == 1, w_pack_loaded == 1 in a headless E1M6 warp run), so no map falls to
 * the slow scattered path. Re-check that when changing this number. */
/* Left at 1492 KB after a 2026-07-27 attempt to grow it. DO NOT repeat that attempt
 * without reading this first.
 *
 * The motivation was real: E1M6 has only ~320 KiB free at R_LitFlatLevelInit and needs
 * to stay above a MEASURED ~199 KiB free-heap cliff (see RENDER_HEAP_FLOOR in
 * src/r_hotpath.c) where whole-frame time jumps 50 -> 71 ms. ~34 KiB of resident image
 * was successfully recovered -- packing state_t 28 -> 16 bytes and mobjinfo_t 92 -> 60
 * (17.3 KiB), storing only the exactly odd-symmetric first half of finetangent[]
 * (8 KiB), and two more -Os waves (9.8 KiB) -- and all of it was reverted, because on
 * this port image space is NOT free to move:
 *
 *   change                       zone won   E1M6 ms   E1M1 ms
 *   (baseline)                        --     50.33     65.74
 *   finetangent half table         8 KiB    +0.76     +1.51
 *   info.c struct packing         17 KiB    +0.05     +0.96
 *   -Os over the per-tic files   3.2 KiB    +0.07     -0.15
 *   -Os over load/HUD files      6.6 KiB    +0.80     +0.41
 *
 * Every mechanism that yields meaningful .rodata/.text pays for it in the 1 KiB
 * instruction cache: shrinking anything relocates the cold render callers that the
 * placed hot drawers in platform/pcfx_hot.ld were tuned against, and a full re-sweep of
 * the five hot slots recovered only part of the loss. The cache the heap would have
 * bought is not worth it either -- see R_LitFlatLevelInit. */
#if defined(DEV_CD_MATRIX)
/* Matrix self-test build: the harness adds resident code/data and the game
 * never runs, so give the link back 32 KiB.  The zone is only borrowed as
 * raw read scratch (Z_DevScratch) and still dwarfs MTX_BYTES. */
#define ZONE_SIZE_KB 1466
#elif defined(SERIAL_LOG)
#define ZONE_SIZE_KB 1490
#else
#define ZONE_SIZE_KB 1492
#endif
const unsigned int maxHeapSize = (ZONE_SIZE_KB * 1024);

#ifndef GBA
    static int running_count = 0;
#endif

typedef struct memblock_s
{
    unsigned int size:24;	// including the header and possibly tiny fragments
    unsigned int tag:4;	// purgelevel
    void**		user;	// NULL if a free block
    struct memblock_s*	next;
    struct memblock_s*	prev;
} memblock_t;


typedef struct
{
    // start / end cap for linked list
    memblock_t	blocklist;
    memblock_t*	rover;
} memzone_t;

memzone_t*	mainzone;

//
// Z_Init
//
// The zone lives in a fixed static reserve (in .bss/RAM), not on a libc heap.
// The PC-FX is bare-metal with no working malloc/_sbrk of this size, and a
// static buffer makes the RAM budget deterministic (linker fails the build if
// it doesn't fit, rather than Z_Init failing at runtime).
// Enlarged now that the IWAD is streamed from CD (Phases 2+3) rather than baked
// into the program image: the freed ~800 KB of .rodata becomes zone cache, big
// enough to hold the whole-run PU_STATIC lumps plus one level's PU_LEVEL graphics.
static byte s_zonemem[ZONE_SIZE_KB * 1024] __attribute__((aligned(16)));

#ifdef DEV_CD_MATRIX
/* The CD matrix self-test build runs instead of the game and never calls
 * Z_Init, so it may borrow the zone reserve as its RAM-path read buffer
 * rather than adding its own BSS (which overflows the link budget). */
byte* Z_DevScratch(void) { return s_zonemem; }
#endif

void Z_Init (void)
{
    memblock_t*	block;

    unsigned int heapSize = sizeof(s_zonemem);

    mainzone = (memzone_t*)s_zonemem;

    lprintf(LO_INFO,"Z_Init: Heapsize is %d bytes.", heapSize);

    // set the entire zone to one free block
    mainzone->blocklist.next =
    mainzone->blocklist.prev =
    block = (memblock_t *)( (byte *)mainzone + sizeof(memzone_t) );

    mainzone->blocklist.user = (void *)mainzone;
    mainzone->blocklist.tag = PU_STATIC;
    mainzone->rover = block;

    block->prev = block->next = &mainzone->blocklist;

    // NULL indicates a free block.
    block->user = NULL;

    block->size = heapSize - sizeof(memzone_t);
}

// Shrink an in-use block to `newsize`, returning the freed tail to the heap. Used
// by the precache: the graphics arena is over-reserved as one contiguous block
// (while the heap is empty enough to fit it), filled, then trimmed to exactly what
// it holds — so the slack goes back to the evictable decompressed working set
// instead of being pinned. The tail is spliced in as its own block and freed so it
// coalesces with the following free space.
void Z_TrimBlock(void *ptr, unsigned newsize)
{
    if (!ptr) return;
    memblock_t *block = (memblock_t *)((byte *)ptr - sizeof(memblock_t));
    unsigned want = ((newsize + 3u) & ~3u) + (unsigned)sizeof(memblock_t);
    if (block->size < want + (unsigned)sizeof(memblock_t) + MINFRAGMENT)
        return;                                   // nothing worth trimming

    memblock_t *frag = (memblock_t *)((byte *)block + want);
    frag->size = block->size - want;
    frag->tag  = block->tag;
    frag->user = (void *)2;                       // in-use/unowned so Z_Free splices it
    frag->prev = block;
    frag->next = block->next;
    frag->next->prev = frag;
    block->next = frag;
    block->size = want;

    Z_Free((byte *)frag + sizeof(memblock_t));    // frees + coalesces with neighbour
}

// Largest contiguous free block right now (for sizing the precache arena to the
// space actually available as one piece).
unsigned Z_LargestFreeBlock(void)
{
    unsigned big = 0;
    for (memblock_t *b=mainzone->blocklist.next; b!=&mainzone->blocklist; b=b->next)
        if (!b->user && b->size > big) big = b->size;
    return big > sizeof(memblock_t) ? big - sizeof(memblock_t) : 0;
}

// Total free (unowned) bytes across all fragments — the sum a series of allocations
// could draw from (vs Z_LargestFreeBlock, one contiguous block). Used by the arena
// grow to stop before crossing the gameplay reserve.
unsigned Z_TotalFree(void)
{
    unsigned free = 0;
    for (memblock_t *b=mainzone->blocklist.next; b!=&mainzone->blocklist; b=b->next)
        if (!b->user) free += b->size;
    return free;
}

// Dump a zone-usage breakdown (for tuning the precache/zone budget).
void Z_DumpUsage(const char *tag)
{
    unsigned free=0,big=0,us=0,lv=0,ca=0,nblk=0;
    for (memblock_t *b=mainzone->blocklist.next; b!=&mainzone->blocklist; b=b->next)
    {
        nblk++;
        if (!b->user) { free+=b->size; if(b->size>big) big=b->size; }
        else if (b->tag==PU_STATIC) us+=b->size;
        else if (b->tag==PU_LEVEL)  lv+=b->size;
        else ca+=b->size;
    }
    lprintf(LO_INFO,"Z@%s free=%u big=%u static=%u level=%u cache=%u blks=%u",
            tag,free,big,us,lv,ca,nblk);
}


//
// Z_Free
//
void Z_Free (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;

    if(ptr == NULL)
        return;

    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->user > (void **)0x100)
    {
        // smaller values are not pointers
        // Note: OS-dependend?

        // clear the user's mark
        *block->user = 0;
    }

    // mark as free
    block->user = NULL;
    block->tag = 0;


#ifdef ZONE_TRACE
    running_count -= block->size;
    printf("Free: %d\n", running_count);
#endif

    other = block->prev;

    if (!other->user)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == mainzone->rover)
            mainzone->rover = other;

        block = other;
    }

    other = block->next;
    if (!other->user)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == mainzone->rover)
            mainzone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//

// When set, Z_Malloc returns NULL instead of I_Error on genuine OOM (after the
// full purge/coalesce scan). Set only for the duration of a Z_TryMalloc call, so
// callers that can degrade gracefully (e.g. the intermission CD background) don't
// take the whole game down on a fragmented heap. Not reentrant, but the only
// interrupt handler (the timer ISR) never allocates, so there is no race.
static boolean z_nofail = false;

void* Z_Malloc(int size, int tag, void **user)
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;

    size = (size + 3) & ~3;

    // scan through the block list,
    // looking for the first free block
    // of sufficient size,
    // throwing out any purgable blocks along the way.

    // account for size of block header
    size += sizeof(memblock_t);

    // if there is a free block behind the rover,
    //  back up over them
    base = mainzone->rover;

    if (!base->prev->user)
    base = base->prev;

    rover = base;
    start = base->prev;

    do
    {
        if (rover == start)
        {
            // scanned all the way around the list
            unsigned free=0,big=0,us=0,lv=0,ca=0,nblk=0;
            for (memblock_t *b=mainzone->blocklist.next; b!=&mainzone->blocklist; b=b->next)
            {
                nblk++;
                if (!b->user) { free+=b->size; if(b->size>big) big=b->size; }
                else if (b->tag==PU_STATIC) us+=b->size;
                else if (b->tag==PU_LEVEL) lv+=b->size;
                else ca+=b->size;
            }
            lprintf(LO_ERROR,"Z_OOM want=%i free=%u big=%u static=%u level=%u cache=%u blks=%u",
                    size,free,big,us,lv,ca,nblk);
            if (z_nofail)
                return NULL;   // caller opted into graceful failure (Z_TryMalloc)
            I_Error ("Z_Malloc: failed on allocation of %i bytes", size);
        }

        if (rover->user)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                // hit a block that can't be purged,
                //  so move base past it
                base = rover = rover->next;
            }
            else
            {
                // free the rover block (adding the size to base)

                // the rover can be the base block
                base = base->prev;
                Z_Free ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
            rover = rover->next;

    } while (base->user || base->size < size);


    // found a block big enough
    extra = base->size - size;

    if (extra >  MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;

        // NULL indicates free block.
        newblock->user = NULL;
        newblock->tag = 0;
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }

    if (user)
    {
        // mark as an in use block
        base->user = user;
        *(void **)user = (void *) ((byte *)base + sizeof(memblock_t));
    }
    else
    {
        if (tag >= PU_PURGELEVEL)
            I_Error ("Z_Malloc: an owner is required for purgable blocks");

        // mark as in use, but unowned
        base->user = (void *)2;
    }

    base->tag = tag;

    // next allocation will start looking here
    mainzone->rover = base->next;

#ifdef ZONE_TRACE
    running_count += base->size;
    printf("Alloc: %d (%d)\n", base->size, running_count);
#endif

    return (void *) ((byte *)base + sizeof(memblock_t));
}

// Non-fatal Z_Malloc: same purge/coalesce scan, but returns NULL instead of
// I_Error when the heap genuinely can't satisfy the request. For callers that
// can fall back (e.g. skip the intermission CD background) rather than crash.
void* Z_TryMalloc(int size, int tag, void **user)
{
    void *p;
    z_nofail = true;
    p = Z_Malloc(size, tag, user);
    z_nofail = false;
    return p;
}

void* Z_Calloc(size_t count, size_t size, int tag, void **user)
{
    const size_t bytes = count * size;
    void* ptr = Z_Malloc(bytes, tag, user);

    if(ptr)
        memset(ptr, 0, bytes);

    return ptr;
}

char* Z_Strdup(const char* s)
{
    const unsigned int len = strlen(s);

    if(!len)
        return NULL;

    char* ptr = Z_Malloc(len+1, PU_STATIC, NULL);

    if(ptr)
        strcpy(ptr, s);

    return ptr;
}

void* Z_Realloc(void *ptr, size_t n, int tag, void **user)
{
    void *p = Z_Malloc(n, tag, user);

    if (ptr)
    {
        memblock_t *block = (memblock_t *)((char *) ptr - sizeof(memblock_t));

        memcpy(p, ptr, n <= block->size ? n : block->size);

        Z_Free(ptr);

        if (user) // in case Z_Free nullified same user
            *user = p;
    }
    return p;
}

//
// Z_FreeTags
//
void Z_FreeTags(int lowtag, int hightag)
{
    memblock_t*	block;
    memblock_t*	next;

    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist ;
         block = next)
    {
        // get link before freeing
        next = block->next;

        // free block?
        if (!block->user)
            continue;

        if (block->tag >= lowtag && block->tag <= hightag)
            Z_Free ( (byte *)block+sizeof(memblock_t));
    }
}

//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
    memblock_t*	block;

    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
        if (block->next == &mainzone->blocklist)
        {
            // all blocks have been hit
            break;
        }

        if ( (byte *)block + block->size != (byte *)block->next)
        {
#ifdef SERIAL_LOG
            /* DBG: report corrupt block tag/size, actual gap, prev-block tag */
            unsigned int gap = (unsigned int)((byte*)block->next - (byte*)block);
            *(volatile unsigned short*)0x0C059000 = 0x8000 | (block->tag << 8) | (block->user ? 0x10 : 0x00);
            *(volatile unsigned short*)0x0C059000 = 0x7000 | (block->size & 0xFFF);
            *(volatile unsigned short*)0x0C059000 = 0x6000 | ((block->size >> 12) & 0xFFF);
            *(volatile unsigned short*)0x0C059000 = 0x5000 | (gap & 0xFFF);
            *(volatile unsigned short*)0x0C059000 = 0x4000 | ((gap >> 12) & 0xFFF);
            *(volatile unsigned short*)0x0C059000 = 0x3000 | (block->prev->tag << 8) | (block->prev->user ? 0x10 : 0x00);
#endif
            I_Error ("Z_CheckHeap: block size does not touch the next block\n");
        }

        if ( block->next->prev != block)
            I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

        if (!block->user && !block->next->user)
            I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}
