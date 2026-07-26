/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by id Software, Chi Hoang, Lee Killough, Jim Flynn,
 *  Rand Phares, Ty Halderman; 1999-2006 by the PrBoom team.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 * DESCRIPTION:
 *      WAD header, directory, and lump I/O — CD-streamed IWAD for the PC-FX.
 *
 *      The full doom1.wad is too big to keep resident in 2 MB RAM, so it lives
 *      on the CD as a sector-aligned blob (tools/bake_wad.py --cd-wad). Only the
 *      lump directory is resident; lump *data* is streamed from the disc into the
 *      zone cache on demand (docs/cd-streaming-plan.md, Phases 2+3 merged).
 *
 *      Cache lifetime is by lump class, chosen so no held pointer ever dangles
 *      (this engine stores raw lump pointers in globals/structs and reads them
 *      every frame, and has no texture-composite cache — so blanket PU_CACHE
 *      eviction is unsafe):
 *        - PU_LEVEL for per-level bulk (wall patches, flats, sprites, map lumps,
 *          full-screen pictures). Freed by Z_FreeTags(PU_LEVEL) at each level
 *          load and re-streamed; pinned (not purgeable) while a level is live, so
 *          every lump touched during a level stays valid for that whole level.
 *        - PU_STATIC for the small whole-run set (COLORMAP, PLAYPAL, PNAMES,
 *          TEXTURE1/2, fonts, HUD, menu) that is loaded once at startup and held
 *          for the entire session.
 *      The peak resident set is therefore ~(whole-run statics) + (one level's
 *      graphics) — which fits 2 MB, unlike the ~1.8 MB of all episode graphics.
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#include <ctype.h>
#endif
#include <ctype.h>
#include <string.h>

#include "doomstat.h"
#include "d_net.h"
#include "doomtype.h"
#include "i_system.h"
#include "z_zone.h"

#ifdef __GNUG__
#pragma implementation "w_wad.h"
#endif
#include "w_wad.h"
#include "lprintf.h"
#include "global_data.h"

#include "pcfx_wad.h"      /* pcfx_wad_read: CD -> RAM, owns the blob's base LBA */
#include "pcfx_mappack.h"  /* pcfx_mappack_read: per-map contiguous asset pack blob */
#include "pcfx_boot.h"     /* boot percentage + current CD-loading operation */
extern unsigned g_cd_read_cmds, g_cd_read_sectors, g_cd_seek_ms;  /* CD diagnostics (pcfx_wad.c) */
static unsigned  w_pc_cmds0, w_pc_sect0, w_pc_seek0; /* CD counters at load start */
static int       w_pc_maplump = -1;                  /* this load's map marker lump */
static int       w_pack_loaded;                      /* 1 = this level's arena came from a CD pack */
static int       w_pack_avail;                       /* 1 = a pack exists for w_pc_maplump */
static unsigned  w_pack_nlumps, w_pack_hdr_off, w_pack_pay_off, w_pack_pay_bytes; /* cached */
static unsigned  w_pack_n_geo, w_pack_gfx_bytes;     /* leading geometry lumps; arena (gfx) bytes */
static unsigned  w_pack_hdr_sum;                     /* checksum of this map's pack header         */
static byte     *w_pack_index;                       /* cached pack index sector (read once)       */
#include "lz4_depack.h"    /* lz4_depack: per-lump LZ4 decode (see W_CacheLumpNum) */
#include "pcfx_sum32.h"    /* pcfx_sum32: the CD asset integrity check (see below)  */

#define SECTOR 2048u

//
// GLOBALS
//
void ExtractFileBase (const char *path, char *dest)
{
    const char *src = path + strlen(path) - 1;
    int length;

    // back up until a \ or the start
    while (src != path && src[-1] != ':' // killough 3/22/98: allow c:filename
           && *(src-1) != '\\'
           && *(src-1) != '/')
    {
        src--;
    }

    // copy up to eight characters
    memset(dest,0,8);
    length = 0;

    while ((*src) && (*src != '.') && (++length<9))
    {
        *dest++ = toupper(*src);
        src++;
    }
}

//
// CD-streamed WAD state (resident: header, directory, per-lump cache + tags).
//
static int          w_numlumps;
static filelump_t  *w_dir;         /* [w_numlumps], read from CD once at W_Init  */
static void       **lumpcache;     /* [w_numlumps], DECOMPRESSED data ptr or NULL */
static byte        *lumptag;       /* [w_numlumps], PU_STATIC / PU_LEVEL / PU_CACHE */
static unsigned     w_max_cache_decomp; /* biggest single PU_CACHE lump (decompressed) */
static void       **w_comp;        /* [w_numlumps], RESIDENT COMPRESSED payload   */
                                   /* (evictable graphics only; NULL otherwise)   */
static unsigned    *w_disclen;     /* [w_numlumps] on-disc byte len | (LZ4 flag<<31),  */
                                   /* or NULL when the blob is not per-lump compressed */

/* One permanent CD-read staging buffer, sized to the largest LZ4-compressed
 * lump. LZ4 lumps are read here then decompressed to their exact-size cache/comp
 * block; RAW lumps skip it (read straight into a sector-rounded destination), so
 * this need only cover the biggest *compressed* payload — much smaller than the
 * biggest uncompressed lump. Reused for every read (boot statics, map lumps,
 * precache, and the rare precache-miss) so per-read transient staging never
 * interleaves with — and fragments — the permanent cache blocks. */
static byte        *w_stage;
static unsigned     w_stage_bytes;
static void        *w_protect;     /* holds a contiguous gameplay-decompress block
                                    * hostage from the arena; freed at W_BeginGameplay */
static byte        *w_pc_mark;     /* [w_numlumps] precache priority per lump, 0=none */
static int         *w_pc_seq;      /* marked lumps in CALL order (R_PrecacheLevel's   */
static int          w_pc_nseq;     /* order): the arena RESERVES slots in this order  */
                                   /* so an overflow drops exactly what the old per-   */
                                   /* lump fill did; bytes are then read in DISC order */
                                   /* (coalesced) — see W_PrecacheEnd.                 */

/* Set once precache is done and gameplay has started: any CD read taken after
 * this point is a PRECACHE MISS (an asset the level uses that we failed to pull
 * resident) — the whole point of this cache is that gameplay never touches the
 * CD, so we log it loudly so the precache set can be widened. */
static int          w_in_gameplay;

#ifdef GEN_BOOTPACK_MANIFEST
/* Ground-truth boot capture: record every lump the engine REQUESTS via
 * W_CacheLumpNum from W_Init to the first title frame, in call order. That set is
 * the authoritative BOOT pack contents (the whole-run statics the engine actually
 * touches + TITLEPIC), dumped by W_BootManifestDump. Only compiled for manifest gen. */
static int  *w_boot_seq;      /* requested lumps, call order */
static byte *w_boot_seen;     /* [w_numlumps] dedup           */
static int   w_boot_nseq;
static int   w_boot_capturing;
static int   w_boot_dumped;
#endif

// Staging is only needed while LOADING (boot statics, map lumps, precache). It's
// ~43 KB of dead weight during gameplay, and in the tight per-level budget that
// 43 KB is worth freeing for the evictable decompressed working set — so we drop
// it when gameplay starts and re-make it at the next level load (W_PrecacheReserve,
// when the heap is emptiest). A precache-miss mid-play stages transiently instead.
static void w_stage_ensure(void)
{
    if (!w_stage && w_stage_bytes)
        w_stage = (byte *)Z_Malloc((int)w_stage_bytes, PU_STATIC, NULL);
}
void W_BeginGameplay(void)  { w_in_gameplay = 1;
                              if (w_stage)   { Z_Free(w_stage);   w_stage = 0; }
                              if (w_protect) { Z_Free(w_protect); w_protect = 0; } }
void W_EndGameplay(void)    { w_in_gameplay = 0; }

/* On-disc byte length of a lump (compressed size when LZ4'd, else == its size). */
static unsigned lump_disc_len(int i)
{
    return w_disclen ? (w_disclen[i] & 0x7fffffffu) : (unsigned)LONG(w_dir[i].size);
}
/* True if the lump's on-disc payload is an LZ4 block to be lz4_depack'd. */
static int lump_is_lz4(int i)
{
    return w_disclen && (w_disclen[i] >> 31);
}

static unsigned round_sectors(unsigned bytes)
{
    return (bytes + (SECTOR - 1)) & ~(SECTOR - 1);
}

// ---------------------------------------------------------------------------
// CD asset integrity — build-time checksums + DUPLICATE on-disc copies.
//
// An imperfect burn hands the drive sectors that read with GOOD SCSI status and hold
// the WRONG bytes. libpcfx's retries never fire (nothing reported an error) and the
// engine cannot tell a corrupt asset from a legitimately odd one, so it renders it —
// the field report was garbled enemy sprites on real hardware.
//
// So the asset build (tools/bake_wad.py, tools/gen_pcfx_packs.py) stamps a pcfx_sum32
// on every lump, on each map-pack header and on the pack index, and writes each blob
// TWICE. The duplicate sits megabytes further down the disc, past the reach of the
// local defect that spoiled the first copy. Every load-time read goes through
// cd_read_checked() below, which verifies it and, on a mismatch, re-reads from the
// duplicate — then once more from the primary, since a transient SCSI glitch usually
// clears. Only when all three attempts fail does the asset drop out, and then it drops
// CLEANLY: the lump is left un-resident (drawn as nothing / re-read from the other blob
// later) or blanked, never decoded, so lz4_depack can't inflate a corrupt block past
// the end of its cache slot.
//
// This is all LOAD-time work (~5 ops per 4 bytes, ~40 ms over a level's ~500 KB) —
// nothing here runs during gameplay. An asset blob built before this existed carries
// no checksums and no duplicate; every check then passes and behaviour is unchanged.
//
// Kept deliberately compact (one repair core, two verify callbacks, two shared log
// strings): the resident image has ~1 KB of slack against the stack gap the linker
// script asserts, so a per-call-site copy of this logic would not fit.
// ---------------------------------------------------------------------------
static unsigned  *w_lumpsum;      /* [w_numlumps] checksum of each lump's on-disc bytes, or NULL */
static unsigned   w_wad_dup_sec;  /* sector delta to the IWAD blob's duplicate copy (0 = none)   */
static unsigned   w_pack_dup_sec; /* sector delta to the map-pack blob's duplicate copy          */
static unsigned   w_dup_repairs;  /* reads that a retry (usually the duplicate) put right        */
static unsigned   w_dup_lost;     /* reads still corrupt after every retry -> asset dropped      */

static unsigned mp_u32(const byte *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }

/* 1 if the lump's on-disc bytes at `p` match the build-time checksum (or the blob
 * carries no checksums, in which case there is nothing to check). */
static int lump_sum_ok(int lump, const byte *p)
{
    return !w_lumpsum || pcfx_sum32(p, lump_disc_len(lump)) == w_lumpsum[lump];
}

/* What a read is checked against. `bad` returns 1 when the buffer is wrong; the two
 * shapes are a WAD window (lumps [first,last] at their directory offsets) and a pack
 * window (pack-header entries [first,last] at their payload offsets). `bytes`/`want`
 * cover the third shape: a whole region against a single checksum. */
typedef struct {
    unsigned    base;       /* WAD window: byte offset of the window in the blob     */
    const byte *hdr;        /* pack window: the map's pack header                    */
    unsigned    first, last;
    int         only_comp;  /* WAD window: check only lumps with an arena slot        */
    unsigned    bytes, want;/* whole region: length and expected checksum            */
} cd_check;

static int wad_window_bad(const byte *buf, const cd_check *c)
{
    if (!w_lumpsum) return 0;
    for (unsigned k = c->first; k <= c->last; k++)
    {
        /* W_PrecacheEnd reads THROUGH unreserved lumps, whose bytes can run past the
         * window end — checking those would fail on a perfectly good read. */
        if (c->only_comp && !w_comp[k]) continue;
        unsigned pos = (unsigned)LONG(w_dir[k].filepos);
        if (pos < c->base) continue;                    /* not carried by this window */
        if (!lump_sum_ok((int)k, buf + (pos - c->base)))
            return 1;
    }
    return 0;
}

// Map-pack blob layout (tools/gen_pcfx_packs.py). Index sector: 'MPK2', nmaps,
// mirror_off, blob_bytes, index_sum, reserved, then PACK_ENT-byte entries; per-map
// header: PACK_HDRENT-byte entries. Sector 1 holds a duplicate of the index sector.
#define PACK_INDEX_HDR 24u
#define PACK_ENT       (8u + 4u*7u)   // name8 + nlumps,hdr_off,pay_off,pay_bytes,n_geo,gfx_bytes,hdr_sum
#define PACK_HDRENT    20u            // lumpnum, payload_off, disclen, lz4, checksum
#define PK_LUMP(hdr,j) mp_u32((hdr) + (j)*PACK_HDRENT)
#define PK_POFF(hdr,j) mp_u32((hdr) + (j)*PACK_HDRENT + 4)
#define PK_DLEN(hdr,j) mp_u32((hdr) + (j)*PACK_HDRENT + 8)
#define PK_SUM(hdr,j)  mp_u32((hdr) + (j)*PACK_HDRENT + 16)

/* 1 if pack lump `j`'s on-disc bytes at `p` match the checksum in the pack header. */
static int pack_lump_ok(const byte *hdr, unsigned j, const byte *p)
{
    return pcfx_sum32(p, PK_DLEN(hdr, j)) == PK_SUM(hdr, j);
}

/* `base` here is the payload offset the window starts at. */
static int pack_window_bad(const byte *buf, const cd_check *c)
{
    for (unsigned k = c->first; k <= c->last; k++)
        if (!pack_lump_ok(c->hdr, k, buf + (PK_POFF(c->hdr, k) - c->base)))
            return 1;
    return 0;
}

static int region_bad(const byte *buf, const cd_check *c)
{
    return pcfx_sum32(buf, c->bytes) != c->want;
}

/* Shared repair core. Reads `bytes` at `sec` from a blob (`rd`, whose duplicate copy is
 * `dup` sectors further on), verifies with `bad`, and on a mismatch re-reads from the
 * duplicate, then once more from the primary. Leaves the best attempt in `buf`.
 * 1 = verified; 0 = every attempt failed and the caller must not trust the data. */
static int cd_read_checked(void (*rd)(unsigned, void *, unsigned), unsigned dup,
                          unsigned sec, byte *buf, unsigned bytes,
                          int (*bad)(const byte *, const cd_check *),
                          const cd_check *c)
{
    rd(sec, buf, bytes);
    if (!bad(buf, c)) return 1;
    if (dup)
    {
        rd(sec + dup, buf, bytes);                      /* the disc's second copy */
        if (!bad(buf, c)) goto repaired;
    }
    rd(sec, buf, bytes);                                /* transient glitch? again */
    if (bad(buf, c))
    {
        w_dup_lost++;
        lprintf(LO_WARN, "W_CD: sector %u (%u bytes) corrupt on both copies", sec, bytes);
        return 0;
    }
repaired:
    w_dup_repairs++;
    lprintf(LO_WARN, "W_CD: sector %u repaired on retry", sec);
    return 1;
}

/* Read a window of IWAD lumps [first,last] (see cd_read_checked). */
static int wad_read_window(unsigned base, byte *buf, unsigned span,
                           int first, int last, int only_comp)
{
    cd_check c = { base, NULL, (unsigned)first, (unsigned)last, only_comp, 0, 0 };
    return cd_read_checked(pcfx_wad_read, w_wad_dup_sec, base / SECTOR, buf, span,
                           wad_window_bad, &c);
}

/* Read a whole region of one blob against a single checksum. `have_sum` off (an asset
 * blob built before this existed) means read it and accept it, as before. */
static int wad_read_region(unsigned ofs, void *buf, unsigned bytes, unsigned want,
                           int have_sum)
{
    cd_check c = { 0, NULL, 0, 0, 0, bytes, want };
    if (!have_sum)
    {
        pcfx_wad_read(ofs / SECTOR, buf, bytes);
        return 1;
    }
    return cd_read_checked(pcfx_wad_read, w_wad_dup_sec, ofs / SECTOR, (byte *)buf,
                           bytes, region_bad, &c);
}

/* 1 if the blob header carries bake_wad.py's DUP1 extension and its own checksum
 * matches — i.e. the offsets, table checksums and duplicate-copy pointer in it can be
 * trusted. An older blob (no extension) reports 0: no checks, original behaviour. */
static int wad_hdr_ok(const byte *h)
{
    return h[28] == 'D' && h[29] == 'U' && h[30] == 'P' && h[31] == '1' &&
           pcfx_sum32(h, 44) == mp_u32(h + 44);
}

//
// Lump classification (run once at W_Init). A lump is PER-LEVEL (PU_LEVEL, freed
// and re-streamed each level) when it is bulk, level-specific data; everything
// else is a small whole-run resource kept resident (PU_STATIC).
//
static int name_is(const char *n, const char *s)   /* n is 8 bytes, not NUL-term */
{
    int i = 0;
    for (; i < 8 && s[i]; i++)
    {
        char a = n[i], b = s[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b)
            return 0;
    }
    return i == 8 || n[i] == 0;    // s exhausted -> n must end here too (8-char field)
}

static int is_map_marker(const char *n)
{
    // ExMy (episode/map) or MAPxx.
    if (n[0] == 'E' && n[2] == 'M' && isdigit((unsigned char)n[1]) &&
        isdigit((unsigned char)n[3]) && n[4] == 0)
        return 1;
    if (n[0]=='M' && n[1]=='A' && n[2]=='P' && isdigit((unsigned char)n[3]) &&
        isdigit((unsigned char)n[4]) && n[5] == 0)
        return 1;
    return 0;
}

static int is_map_lump(const char *n)
{
    static const char *const names[] = {
        "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES", "SEGS", "SSECTORS",
        "NODES", "SECTORS", "REJECT", "BLOCKMAP", 0 };
    for (int i = 0; names[i]; i++)
        if (name_is(n, names[i]))
            return 1;
    return 0;
}

static int is_big_picture(const char *n)
{
    // Full-screen pictures shown transiently (title/help/intermission/finale):
    // let them live and die with the level cache instead of pinning 64 KB each.
    static const char *const names[] = {
        "TITLEPIC", "INTERPIC", "HELP1", "HELP2", "CREDIT", "VICTORY2",
        "PFUB1", "PFUB2", "ENDPIC", "BOSSBACK", 0 };
    for (int i = 0; names[i]; i++)
        if (name_is(n, names[i]))
            return 1;
    return 0;
}

static void classify_lumps(void)
{
    int in_p = 0, in_f = 0, in_s = 0;
    for (int i = 0; i < w_numlumps; i++)
    {
        const char *n = w_dir[i].name;
        // Namespace markers: everything between P/F/S _START and _END is
        // per-level bulk (wall patches / flats / sprites).
        if (name_is(n, "P_START") || name_is(n, "PP_START")) in_p = 1;
        else if (name_is(n, "P_END") || name_is(n, "PP_END")) in_p = 0;
        else if (name_is(n, "F_START") || name_is(n, "FF_START")) in_f = 1;
        else if (name_is(n, "F_END") || name_is(n, "FF_END")) in_f = 0;
        else if (name_is(n, "S_START") || name_is(n, "SS_START")) in_s = 1;
        else if (name_is(n, "S_END") || name_is(n, "SS_END")) in_s = 0;

        // FLATS, SPRITES and WALL PATCHES are PU_CACHE — evictable under memory
        // pressure and re-streamed on next use — so only the recently-used ones
        // stay resident instead of every one the level has ever shown (the
        // accumulation that blew the zone; wall textures were the last, dominant
        // accumulator on texture-heavy maps like E1M2). Flats are self-contained
        // (raw 4096 bytes) drawn immediately; sprites' vissprite re-caches its
        // patch at draw time (r_hotpath.c R_DrawSprite). Wall patches (P): the
        // texture_t is immutable PU_STATIC metadata and no longer holds a raw
        // patch pointer — it stores the patch LUMP NUMBER, and R_GetColumn /
        // R_ComposeColumn re-fetch via W_CacheLumpNum(lump) at each draw, never
        // holding a patch pointer across a Z_Malloc, so an eviction re-streams
        // cleanly. Map lumps + big pictures stay PU_LEVEL.
#ifdef PCFX_TEXTURE_PAGES
        const int pcfx_page = (n[0] == 'P' && (n[1] == 'W' || n[1] == 'F') &&
                               n[2] == 'T');
#endif
        if (in_f || in_s || in_p
#ifdef PCFX_TEXTURE_PAGES
            || pcfx_page
#endif
           )
        {
            lumptag[i] = PU_CACHE;
            // Track the biggest single graphic the renderer will decompress during
            // play. The arena grow must leave a contiguous free block at least this
            // large, or a gameplay decompress can OOM even with ample total free.
            unsigned sz = (unsigned)LONG(w_dir[i].size);
            if (sz > w_max_cache_decomp) w_max_cache_decomp = sz;
        }
        else if (is_map_marker(n) || is_map_lump(n) || is_big_picture(n))
            lumptag[i] = PU_LEVEL;
        else
            lumptag[i] = PU_STATIC;
    }
}

static const void *W_EmptyLump(void);   // shared transparent lump for in-gameplay misses

//
// W_Init — read the WAD header + directory from the CD into resident RAM and set
// up the lump cache. The blob's base LBA lives in platform/pcfx_wad.c.
//
void W_Init(void)
{
    // The IWAD header is at the very start of the blob (sector 0). One sector is
    // plenty for the 12-byte wadinfo_t (the blob pads each lump to a sector).
    pcfx_boot_progress_set_label("WAD HEADER");
    byte *hdrsec = (byte *)Z_Malloc(SECTOR, PU_STATIC, NULL);
    pcfx_wad_read(0, hdrsec, SECTOR);

    // Integrity extension (bake_wad.py 'DUP1'): [20] lump-checksum table offset,
    // [24] offset of the blob's DUPLICATE copy, [28] 'DUP1', [32] directory checksum,
    // [36] disc-length table checksum, [40] checksum-table checksum, [44] checksum of
    // the header's own first 44 bytes. Every check below hangs off these fields, so the
    // header sector is verified FIRST — and if it is bad, re-read from the duplicate
    // copy that its (possibly corrupt) pointer names, which either verifies or doesn't.
    if (!wad_hdr_ok(hdrsec))
    {
        unsigned cand = mp_u32(hdrsec + 24);
        if (cand && (cand % SECTOR) == 0)
        {
            pcfx_wad_read(cand / SECTOR, hdrsec, SECTOR);
            if (wad_hdr_ok(hdrsec))
            {
                w_dup_repairs++;
                lprintf(LO_WARN, "W_INIT: WAD header repaired from the disc's second copy");
            }
        }
    }
    int have_sums = wad_hdr_ok(hdrsec);
    unsigned sum_ofs = 0, dir_sum = 0, dl_sum = 0, st_sum = 0;
    if (have_sums)
    {
        sum_ofs       = mp_u32(hdrsec + 20);
        w_wad_dup_sec = mp_u32(hdrsec + 24) / SECTOR;
        dir_sum       = mp_u32(hdrsec + 32);
        dl_sum        = mp_u32(hdrsec + 36);
        st_sum        = mp_u32(hdrsec + 40);
    }

    const wadinfo_t *header = (const wadinfo_t *)hdrsec;
    if (strncmp(header->identification, "IWAD", 4))
        I_Error("W_Init: CD IWAD id missing (got %.4s)", header->identification);

    w_numlumps = LONG(header->numlumps);
    unsigned dirofs = (unsigned)LONG(header->infotableofs);   // sector-aligned
    unsigned dirbytes = (unsigned)w_numlumps * sizeof(filelump_t);

    // Optional per-lump LZ4: bake_wad --cd-lz4 extends the header with
    // [disclen_ofs u32 @12][magic 'LZ4C' @16]. Absent magic -> every lump is raw
    // (w_disclen stays NULL and lump_disc_len() falls back to the directory size).
    unsigned disclen_ofs = 0;
    if (hdrsec[16] == 'L' && hdrsec[17] == 'Z' && hdrsec[18] == '4' && hdrsec[19] == 'C')
        disclen_ofs = (unsigned)LONG(*(const int *)(hdrsec + 12));
    Z_Free(hdrsec);

    // Directory: read the whole thing (sector-rounded) into a resident buffer. A bad
    // directory is not a glitched sprite, it is a game that cannot find its lumps at
    // all ("W_GetNumForName: TEXTURE1 not found"), so say what actually happened.
    pcfx_boot_progress_set_label("WAD DIRECTORY");
    w_dir = (filelump_t *)Z_Malloc((int)round_sectors(dirbytes), PU_STATIC, NULL);
    if (!wad_read_region(dirofs, w_dir, dirbytes, dir_sum, have_sums))
        I_Error("W_Init: CD WAD directory corrupt on both copies (bad disc?)");

    // Per-lump on-disc lengths + LZ4 flags (sector-aligned table at the blob end).
    if (disclen_ofs)
    {
        unsigned dlbytes = (unsigned)w_numlumps * sizeof(unsigned);
        pcfx_boot_progress_set_label("WAD LENGTHS");
        w_disclen = (unsigned *)Z_Malloc((int)round_sectors(dlbytes), PU_STATIC, NULL);
        if (!wad_read_region(disclen_ofs, w_disclen, dlbytes, dl_sum, have_sums))
            I_Error("W_Init: CD WAD length table corrupt on both copies (bad disc?)");
    }

    // Per-lump checksums: the table every later read is verified against. If IT is the
    // thing that came back wrong, drop it and load unverified (as builds before this
    // did) rather than refusing to boot — one bad table is not a reason to stop.
    if (have_sums && sum_ofs)
    {
        unsigned stbytes = (unsigned)w_numlumps * sizeof(unsigned);
        pcfx_boot_progress_set_label("WAD CHECKSUMS");
        w_lumpsum = (unsigned *)Z_Malloc((int)round_sectors(stbytes), PU_STATIC, NULL);
        if (!wad_read_region(sum_ofs, w_lumpsum, stbytes, st_sum, have_sums))
        {
            Z_Free(w_lumpsum);
            w_lumpsum = NULL;
            lprintf(LO_WARN, "W_INIT: lump checksum table unreadable — loading unverified");
        }
    }

    lumpcache = (void **)Z_Calloc(w_numlumps, sizeof(void *), PU_STATIC, NULL);
    w_comp    = (void **)Z_Calloc(w_numlumps, sizeof(void *), PU_STATIC, NULL);
    lumptag   = (byte *)Z_Malloc(w_numlumps, PU_STATIC, NULL);
    w_pc_mark = (byte *)Z_Malloc(w_numlumps, PU_STATIC, NULL);
    w_pc_seq  = (int  *)Z_Malloc(w_numlumps * (int)sizeof(int), PU_STATIC, NULL);
    classify_lumps();

    // Staging buffer: big enough for the largest LZ4 on-disc payload (raw lumps
    // are read straight into their destination, so they don't size it).
    unsigned maxlz4 = SECTOR;
    for (int i = 0; i < w_numlumps; i++)
        if (lump_is_lz4(i))
        {
            unsigned d = lump_disc_len(i);
            if (d > maxlz4) maxlz4 = d;
        }
    // +1 sector of slack: the map-pack fill (W_LoadMapPack) reads payload windows from
    // a SECTOR floor, so a window can carry up to ~1 sector of lead-in before the first
    // lump on top of the biggest lump itself; the slack keeps that within w_stage.
    w_stage_bytes = round_sectors(maxlz4) + SECTOR;
    w_stage = (byte *)Z_Malloc((int)w_stage_bytes, PU_STATIC, NULL);

    // Build the shared empty lump now (whole-run PU_STATIC, packed low with the rest of the
    // boot set) so an in-gameplay precache miss never allocates — see W_EmptyLump.
    (void)W_EmptyLump();

    lprintf(LO_INFO, "W_Init: %d lumps from CD (stage %u, %s, dup %s).",
            w_numlumps, w_stage_bytes,
            w_lumpsum ? "verified" : "unverified",
            w_wad_dup_sec ? "yes" : "no");

#ifdef GEN_BOOTPACK_MANIFEST
    // Begin ground-truth boot capture: every lump requested until the first title
    // frame is a boot-resident lump (see W_CacheLumpNum / W_BootManifestDump).
    w_boot_seq  = (int  *)Z_Malloc(w_numlumps * (int)sizeof(int), PU_STATIC, NULL);
    w_boot_seen = (byte *)Z_Calloc(w_numlumps, 1, PU_STATIC, NULL);
    w_boot_nseq = 0; w_boot_capturing = 1; w_boot_dumped = 0;
#endif

    // Pull the whole boot-resident set from its pre-assembled contiguous CD pack in
    // one ~0-seek sweep (no-op under GEN_BOOTPACK_MANIFEST, and if the pack has no
    // BOOT entry — then boot falls back to the scattered preload_static_run).
    W_LoadBootPack();
}

//
// Directory lookups (search the resident directory copy).
//
static const filelump_t *FindLumpByNum(int num)
{
    if (num < 0 || num >= w_numlumps)
        return NULL;
    return &w_dir[num];
}

int W_CheckNumForName(const char *name)
{
    // 8-byte, zero-padded, case-folded key (PNAMES patch names can be lowercase).
    char key[8];
    memset(key, 0, 8);
    for (int k = 0; k < 8 && name[k]; k++)
        key[k] = toupper((unsigned char)name[k]);

    // Search backwards so a later lump overrides an earlier one of the same name.
    for (int i = w_numlumps - 1; i >= 0; i--)
        if (memcmp(key, w_dir[i].name, 8) == 0)
            return i;
    return -1;
}

int W_GetNumForName(const char* name)
{
    int i = W_CheckNumForName(name);
    if (i == -1)
        I_Error("W_GetNumForName: %.8s not found", name);
    return i;
}

const char* W_GetNameForNum(int lump)
{
    const filelump_t *l = FindLumpByNum(lump);
    return l ? l->name : NULL;
}

int W_NumLumps(void)
{
    return w_numlumps;
}

//
// W_LumpLength
//
int W_LumpLength(int lump)
{
    const filelump_t *l = FindLumpByNum(lump);
    if (l)
        return LONG(l->size);
    I_Error("W_LumpLength: %i >= numlumps", lump);
    return 0;
}

//
// Decode a lump's on-disc payload — already resident in RAM at `src` (exactly
// lump_disc_len bytes) — into a fresh, EXACT-uncompressed-size cache block owned
// by lumpcache[lump]: lz4_depack when LZ4, else a plain copy. Caching at the
// exact size (not sector-rounded) keeps hundreds of small lumps from wasting
// ~1.5 KB each. Used for the RAM->RAM graphics decode (src = w_comp[lump]).
//
static void decode_mem_into_cache(int lump, const byte *src)
{
    int   size = LONG(w_dir[lump].size);          // UNCOMPRESSED size
    void *buf  = Z_Malloc(size > 0 ? size : 1, lumptag[lump], &lumpcache[lump]);
    if (size <= 0)
        return;
    if (lump_is_lz4(lump))
        lz4_depack(src, buf);                     // [2-byte clen][block] -> size bytes
    else
        memcpy(buf, src, (unsigned)size);
}

//
// Cache a lump as BLANK (a zeroed block of its uncompressed size). Used when its
// on-disc bytes are still corrupt after every retry: a blank graphic is a visible
// but harmless defect, whereas lz4_depack'ing a garbage block writes an arbitrary
// length past the end of the cache slot and takes the heap with it.
//
static void cache_blank_lump(int lump)
{
    int size = LONG(w_dir[lump].size);
    Z_Calloc(size > 0 ? size : 1, 1, lumptag[lump], &lumpcache[lump]);
}

//
// Read one lump's on-disc bytes from the CD and decode them into its cache block.
// LZ4 lumps stage through w_stage (compressed) then inflate; RAW lumps are read
// straight into a SECTOR-ROUNDED cache block (so the whole-sector CD read can't
// overrun) — this is why w_stage need only cover the biggest *compressed* lump,
// not the biggest raw one (the huge LINEDEFS/HELP lumps are raw). Used for the
// one-time DECOMPRESSED lumps (PU_STATIC at boot, PU_LEVEL map/pictures at load)
// that are never evicted, so this is off every gameplay hot path.
//
static void load_from_cd_into_cache(int lump)
{
    int      size    = LONG(w_dir[lump].size);
    unsigned filepos = (unsigned)LONG(w_dir[lump].filepos);
    unsigned disc    = lump_disc_len(lump);

    if (size <= 0) { decode_mem_into_cache(lump, NULL); return; }

    if (lump_is_lz4(lump))
    {
        if (w_stage)
        {
            // disc <= w_stage_bytes. Verified (and repaired from the disc's duplicate
            // copy if need be) before it is inflated — see the integrity block above.
            if (wad_read_window(filepos, w_stage, disc, lump, lump, 0))
                decode_mem_into_cache(lump, w_stage);
            else
                cache_blank_lump(lump);
        }
        else
        {
            // Staging is freed during gameplay (see W_BeginGameplay). A precache
            // MISS that lands here stages transiently for this one read.
            byte *tmp = (byte *)Z_Malloc((int)round_sectors(disc), PU_STATIC, NULL);
            if (wad_read_window(filepos, tmp, disc, lump, lump, 0))
                decode_mem_into_cache(lump, tmp);
            else
                cache_blank_lump(lump);
            Z_Free(tmp);
        }
    }
    else
    {
        // Raw: read directly into a sector-rounded block (no staging). Consumers
        // use W_LumpLength() (exact size); the rounding tail is just unused slack.
        void *buf = Z_Malloc((int)round_sectors((unsigned)size), lumptag[lump],
                             &lumpcache[lump]);
        // Verified in place (the destination IS the cache block), so a repair re-reads
        // straight over it; a lump corrupt on both copies is zeroed rather than shown.
        if (!wad_read_window(filepos, (byte *)buf, disc, lump, lump, 0))
            memset(buf, 0, round_sectors((unsigned)size));
    }
}

//
// Batch-load the maximal contiguous run of not-yet-cached PU_STATIC lumps at
// `first` that fits one w_stage read. Boot loads long runs of tiny PU_STATIC
// lumps (STCFN* font, 63; ST* HUD/face, 76) — one CD seek each dominates the
// black-screen boot — so collapsing them into a handful of reads cuts boot by ~an
// order of magnitude. Only 1-sector PU_STATIC lumps are coalesced (a big or RAW
// neighbour ends the run), so a run is always an all-LZ4 (or tiny-raw) span that
// fits w_stage and never drags a big lump into the batch.
//
static void preload_static_run(int first)
{
    const unsigned base = (unsigned)LONG(w_dir[first].filepos);   // sector-aligned
    int last = first;

    while (last + 1 < w_numlumps)
    {
        int nx = last + 1;
        if (lumptag[nx] != PU_STATIC || lumpcache[nx])
            break;
        if (lump_disc_len(nx) > SECTOR)                  // coalesce 1-sector lumps only
            break;
        unsigned nx_pos = (unsigned)LONG(w_dir[nx].filepos);
        unsigned nx_end = nx_pos + lump_disc_len(nx);
        if (nx_pos < base)                               // directory not disc-monotonic
            break;
        if (round_sectors(nx_end - base) > w_stage_bytes)     // would overflow staging
            break;
        last = nx;
    }

    unsigned span = (unsigned)LONG(w_dir[last].filepos) + lump_disc_len(last) - base;
    int ok = wad_read_window(base, w_stage, span, first, last, 0);

    for (int i = first; i <= last; i++)
    {
        const byte *src = w_stage + ((unsigned)LONG(w_dir[i].filepos) - base);
        // Every lump in the run MUST end up cached (W_CacheLumpNum returns the pointer
        // straight to a caller that will dereference it), so one corrupt on both copies
        // is cached BLANK rather than left NULL or inflated from garbage.
        if (ok || lump_sum_ok(i, src))
            decode_mem_into_cache(i, src);
        else
            cache_blank_lump(i);
    }
}

//
// Precache: pull every EVICTABLE (graphics) lump the level uses into resident RAM
// as ONE contiguous compressed ARENA, so the renderer inflates each from RAM —
// never the CD — for the rest of the level. A single big PU_LEVEL block (not ~600
// per-lump blocks) is deliberate: the zone allocator's purge is not LRU, so ~600
// pinned blocks scattered through the heap would fragment the free space the
// evictable DECOMPRESSED cache churns in, and a mid-frame decompress could then
// fail to find a contiguous hole (an OOM). One arena keeps all the pinned
// compressed data out of the way, leaving the rest of the zone contiguous for the
// decompressed working set. w_comp[lump] points INTO the arena; the arena is
// PU_LEVEL and is freed with the level (W_LevelGraphicsFreed clears w_comp[]).
//
// R_PrecacheLevel brackets its W_PrecacheLumpNum() marks with Begin/End: Begin
// opens a mark set, each LumpNum flags a lump, End sizes+allocates the arena and
// fills it in one pass.
//
/* The compressed-graphics pool is held in up to a few contiguous PU_LEVEL CHUNKS.
 * Chunk 0 is reserved BEFORE the map loads (W_PrecacheReserve) — when the heap is
 * emptiest, so it can be one big block. On a heavy map chunk 0 can't hold the whole
 * pool (the map-load reserve, below, keeps a big hole free so map/texture load won't
 * OOM), so when it fills during R_PrecacheLevel we GROW: allocate another chunk from
 * the free space the load-time reserve no longer needs — leaving only the smaller
 * gameplay decompressed floor. This recovers ~(MAP_LOAD - GAMEPLAY) reserve for the
 * heaviest maps' pools, closing their in-game CD reads. */
#define W_ARENA_MAX_CHUNKS 16
static byte     *w_arena[W_ARENA_MAX_CHUNKS];      /* chunk bases                  */
static unsigned  w_arena_cap[W_ARENA_MAX_CHUNKS];  /* chunk sizes                  */
static unsigned  w_arena_off[W_ARENA_MAX_CHUNKS];  /* bytes filled per chunk       */
static int       w_narena;                         /* chunks in use                */
static int       w_arena_full; /* logged-once flag when the pool overflows        */
static int       w_arena_purged; /* map-load PU_CACHE reclaimed for growth (once) */

/* Room held FREE before the map loads, ON TOP of this map's estimated PU_LEVEL
 * structs (the 1.6x term below). Sized for the map's LOAD-time peak, which needs a
 * big CONTIGUOUS hole (a ~37 KB map array / thing pool) plus working room for the
 * texture-width patch caches P_LoadSideDefs2 decompresses. Must NOT be shrunk to grow
 * the arena — below ~260 KB the load hole fragments and E1M6's load OOMs. The arena is
 * grown AFTER load instead (w_arena_grow), where the reclaimed space (the purgeable
 * texture PU_CACHE, ~260 KB) needn't stay contiguous. */
#define MAP_LOAD_RESERVE (285u * 1024u)

/* Room the FILLED arena must leave free for gameplay: the evictable DECOMPRESSED
 * working set the renderer churns during play, plus fragmentation slack. Measured
 * E1M1 peak ~222 KB, but w_stage (~43 KB) is freed at W_BeginGameplay right after
 * precache, so a ~180 KB floor at fill time yields ~223 KB in play. */
#define GAMEPLAY_RESERVE (180u * 1024u)

static unsigned w_precache_used(void)
{
    unsigned t = 0;
    for (int i = 0; i < w_narena; i++) t += w_arena_off[i];
    return t;
}

// Reserve the compressed-graphics arena as ONE contiguous block, from P_SetupLevel
// right after P_FreeLevelData — when the heap is emptiest (only the whole-run
// PU_STATIC set resident, the rest one big hole) so it can be a single block;
// reserving after map load can't find a big enough contiguous hole. `maplump` is
// this level's ExMy/MAPxx marker: we estimate the map's PU_LEVEL footprint from
// its 10 lumps and hold that (plus the decompressed reserve) back, so a heavy map
// shrinks the arena (it still loads, the graphics pool just partly CD-streams)
// while a light map keeps a big arena (whole pool resident, zero in-game CD).
/* Verify an index sector in place. Its checksum covers the index bytes with its own
 * field held at 0, so blank the field, sum, and put it back (we own the buffer). */
static int pack_index_ok(byte *idx)
{
    if (idx[0]!='M'||idx[1]!='P'||idx[2]!='K'||idx[3]!='2') return 0;
    unsigned nmaps = mp_u32(idx + 4);
    unsigned bytes = PACK_INDEX_HDR + nmaps * PACK_ENT;
    if (bytes > SECTOR) return 0;
    unsigned want = mp_u32(idx + 16), got;
    idx[16] = idx[17] = idx[18] = idx[19] = 0;
    got = pcfx_sum32(idx, bytes);
    idx[16] = (byte)want;         idx[17] = (byte)(want >> 8);
    idx[18] = (byte)(want >> 16); idx[19] = (byte)(want >> 24);
    return got == want;
}

/* Read a pack payload window (lumps [first,last], the window starting at payload offset
 * `floor_poff`) and make it good — see cd_read_checked. */
static int pack_read_window(unsigned sec_off, byte *buf, unsigned bytes,
                            const byte *hdr, unsigned first, unsigned last,
                            unsigned floor_poff)
{
    cd_check c = { floor_poff, hdr, first, last, 0, 0, 0 };
    return cd_read_checked(pcfx_mappack_read, w_pack_dup_sec, sec_off, buf, bytes,
                           pack_window_bad, &c);
}

/* Read a map/BOOT pack header and verify it against the checksum in the index entry.
 * 0 = unusable, and the caller falls back to the scattered path — which streams the
 * same lumps from the IWAD blob, verified in its own right. */
static int pack_read_header(unsigned hdr_off, byte *hdr, unsigned bytes, unsigned want)
{
    cd_check c = { 0, NULL, 0, 0, 0, bytes, want };
    return cd_read_checked(pcfx_mappack_read, w_pack_dup_sec, hdr_off / SECTOR, hdr,
                           bytes, region_bad, &c);
}

// Peek the map-pack index for w_pc_maplump's map. Sets w_pack_avail + the cached
// location (nlumps/hdr_off/pay_off) so both P_LoadSideDefs2 (to skip eager texture
// building) and W_LoadMapPack (to load) can use it without re-reading the index.
//
// The index (blob sector 0) is READ ONCE and cached resident (w_pack_index): it is the
// same 1 sector for every level load, and re-reading it each load cost an extra CD
// command AND a seek (the head had to jump to the blob's front and then BACK to this
// map's header — measured ~200 ms of the residual load seek). With it cached, a level
// load's first CD access is the map's header itself, so the head makes a single jump to
// this map's region and then streams the whole level gap-free. Shared with W_LoadBootPack.
//
// The index is also the bootstrap for every other repair in this file's pack paths — it
// is what names the duplicate copy's offset — so it cannot itself be repaired FROM that
// pointer alone. Hence gen_pcfx_packs.py duplicates the index sector at sector 1, and we
// try, in order: sector 0, sector 1, then both again inside the mirror region (using the
// mirror offset from whichever of the two sectors carried a plausible one). All four bad
// means no pack this run: every map falls back to the scattered IWAD path.
static const byte *w_pack_index_load(void)
{
    if (w_pack_index) return w_pack_index;

    byte *buf = (byte *)Z_Malloc((int)SECTOR, PU_STATIC, NULL);
    if (!buf) return NULL;

    pcfx_mappack_read(0, buf, SECTOR);
    unsigned cand = mp_u32(buf + 8);                   /* mirror offset candidate */
    if (!pack_index_ok(buf))
    {
        pcfx_mappack_read(1, buf, SECTOR);             /* the duplicate index sector */
        int ok = pack_index_ok(buf);
        if (ok)
        {
            w_dup_repairs++;
            lprintf(LO_WARN, "W_PACK: index repaired from its duplicate sector");
        }
        else
        {
            if (!cand || (cand % SECTOR))
                cand = mp_u32(buf + 8);                /* try the other sector's value */
            unsigned dsec = (cand && (cand % SECTOR) == 0) ? cand / SECTOR : 0;
            if (dsec)
            {
                pcfx_mappack_read(dsec, buf, SECTOR);
                ok = pack_index_ok(buf);
                if (!ok)
                {
                    pcfx_mappack_read(dsec + 1, buf, SECTOR);
                    ok = pack_index_ok(buf);
                }
            }
            if (!ok)
            {
                w_dup_lost++;
                lprintf(LO_WARN, "W_PACK: index unreadable — scattered load for every map");
                Z_Free(buf);
                return NULL;
            }
            w_dup_repairs++;
            lprintf(LO_WARN, "W_PACK: index repaired from the disc's second copy");
        }
    }

    /* The mirror is a byte image of the primary region, so a repair read is just the
     * same offset plus this delta — including when the index we ended up using IS the
     * mirror's (its fields are the primary's offsets either way). */
    unsigned mirror = mp_u32(buf + 8);
    w_pack_dup_sec = ((mirror % SECTOR) == 0) ? mirror / SECTOR : 0;
    w_pack_index = buf;
    return w_pack_index;
}

/* Find `name`'s entry in the index sector, or NULL. */
static const byte *pack_find(const byte *idx, const char *name)
{
    unsigned nmaps = mp_u32(idx + 4);
    for (unsigned m = 0; m < nmaps; m++)
    {
        const byte *e = idx + PACK_INDEX_HDR + m * PACK_ENT;
        int match = 1;
        for (int c = 0; c < 8; c++)
        { char a = name[c], b = (char)e[c]; if (a!=b) { match=0; break; } if (!a) break; }
        if (match) return e;
    }
    return NULL;
}

static void w_pack_lookup(void)
{
    w_pack_avail = 0;
#ifdef GEN_MAPPACK_MANIFEST
    return;   // manifest gen: ignore any existing pack, force eager build + marking
#endif
    if (w_pc_maplump < 0) return;
    pcfx_boot_progress_set_label("MAP PACK INDEX");
    const byte *idx = w_pack_index_load();
    if (!idx) return;
    const byte *e = pack_find(idx, w_dir[w_pc_maplump].name);
    if (!e) return;
    w_pack_nlumps    = mp_u32(e + 8);
    w_pack_hdr_off   = mp_u32(e + 12);
    w_pack_pay_off   = mp_u32(e + 16);
    w_pack_pay_bytes = mp_u32(e + 20);
    w_pack_n_geo     = mp_u32(e + 24);
    w_pack_gfx_bytes = mp_u32(e + 28);
    w_pack_hdr_sum   = mp_u32(e + 32);
    w_pack_avail = (w_pack_nlumps > 0 && w_pack_pay_bytes > 0);
}

// Size the SCATTERED-path precache arena's chunk 0. The largest free block (`avail`)
// bounds a single chunk, but the map's own PU_LEVEL working set (`floor`) need NOT
// share that block — exactly like the pack fit-gate in W_PrecacheReserve, it is many
// small allocations that fit OTHER free regions, and the arena may span several chunks
// (w_arena_grow fills the rest during R_PrecacheLevel). So take chunk 0 from the largest
// block while only requiring TOTAL free to still cover `floor` afterward. The old
// `avail - floor` demanded `floor` sit in the SAME block as the arena's leftover, so a
// fragmented level TRANSITION (largest block < floor yet plenty free overall) reserved NO
// arena at all — and with w_narena==0 R_PrecacheLevel precaches nothing, dropping the
// WHOLE level to per-graphic in-game CD reads (the multi-second render freeze this port
// exists to remove). Returning >0 lets precache run; grow()+overflow-drop handle the rest.
static unsigned w_scattered_arena_budget(unsigned avail, unsigned maptotal)
{
    unsigned floor = (maptotal * 16u) / 10u + MAP_LOAD_RESERVE;

    // Healthy heap: reserve from the largest block, leaving `floor` behind IN that block.
    // Identical to the original formula, so nothing changes on a non-fragmented load.
    if (avail > floor)
        return avail - floor;

    // Fragmentation rescue (the fix). The largest single block can't leave `floor` behind,
    // so the original formula returned 0 -> w_narena==0 -> R_PrecacheLevel precaches nothing
    // -> the WHOLE level does per-graphic in-game CD reads (the multi-second render freeze).
    // But `floor` is the map's PU_LEVEL working set: many SMALL allocations that need not
    // share one block. If TOTAL free still covers `floor` AFTER taking (most of) the largest
    // block, reserve it as chunk 0 anyway and let w_arena_grow add chunks / overflow-drop the
    // lowest-priority sprites. A partial arena keeps the level CD-free for its resident set;
    // only the genuinely-tight case (no room for both) still returns 0.
    unsigned total = Z_TotalFree();
    if (avail >= 4096u && total > floor + avail)
        return avail - (avail >> 3);   // keep 1/8 of the block as local slack for the map load
    return 0;
}

void W_PrecacheReserve(int maplump)
{
    w_pc_maplump = maplump;
    w_pc_cmds0 = g_cd_read_cmds;            // bracket the whole level load (map + precache)
    w_pc_sect0 = g_cd_read_sectors;
    w_pc_seek0 = g_cd_seek_ms;
    w_pack_loaded = 0;
    // NB: staging is deliberately NOT re-made here — see w_stage_ensure() at the end of
    // this function. It must not exist while `avail` is measured below.
    w_pack_lookup();                        // does a pack exist? (P_LoadSideDefs2 checks this)

    // Estimate the map's PU_LEVEL footprint. The engine's structs (sectors, sides,
    // lines, segs, subsectors, nodes, the thing pool) run ~1.6x the raw lump bytes
    // (measured E1M1..M9), so hold back ~1.6x the map lumps' total uncompressed
    // size, plus the decompressed-working-set reserve.
    unsigned maptotal = 0;
    if (maplump >= 0)
        for (int i = maplump + 1; i <= maplump + 10 && i < w_numlumps; i++)
            maptotal += (unsigned)LONG(w_dir[i].size);

    // Purge leftover PU_CACHE before measuring the free heap. On a level TRANSITION the
    // previous level's decompressed graphics (flats/sprites/wall-patches — all PU_CACHE)
    // are still resident: purgeable, but NOT freed by P_FreeLevelData (which frees only
    // PU_LEVEL). They fragment the heap, and Z_LargestFreeBlock counts only truly-free
    // blocks (user==NULL), so `avail` reads ~half the real capacity → the pack fails its
    // one-chunk fit test → the load drops to the SCATTERED seek-storm path (E1M1->E1M2 was
    // ~133 CD cmds / ~24 s). These caches belong to the OUTGOING level and the incoming
    // level re-decompresses its own from the pack, so purge them here to reclaim one big
    // contiguous block (restoring the direct-warp `avail`, so every transition takes the
    // fast one-chunk pack path). Safe: no E1M2 lump is loaded yet (geometry loads in
    // W_LoadMapPack below; sidedef textures later), so nothing live is purged.
    Z_FreeTags(PU_CACHE, PU_CACHE);

    unsigned avail  = Z_LargestFreeBlock();

    // For a PACKED map, size chunk0 to hold the GRAPHICS half of the pack in one
    // contiguous block. The pack's leading geometry lumps (w_pack_n_geo) decode into
    // lumpcache[] (their PU_LEVEL footprint is covered by the 1.6x map estimate above),
    // NOT the arena, so the arena needs only the graphics tail (w_pack_gfx_bytes).
    // Texture descriptors are prebuilt without decompressing patches, so packed map
    // load stays lean: hold back only the map structs + gameplay floor instead of the
    // larger MAP_LOAD_RESERVE. Then W_LoadMapPack
    // fills chunk0 in one sweep with no grow/purge/protector. If even that lean budget
    // can't fit the whole graphics set, drop to the scattered path (w_pack_avail = 0)
    // so the pool uses the existing mark, stream, and grow path. The
    // packs are trimmed of dead assets and the reserve tuned so EVERY E1 map fits here.
    unsigned budget;
    if (w_pack_avail)
    {
        unsigned need = round_sectors(w_pack_gfx_bytes);
        unsigned packhold = (maptotal * 16u) / 10u + GAMEPLAY_RESERVE;
        // Fit test. The arena (need) must fit the LARGEST free block as ONE chunk (the
        // whole point of the fast single-read path), but `hold` — the map's PU_LEVEL
        // structs + the gameplay decompress floor — need NOT be in that same block: they
        // are many smaller allocations that can use the OTHER free regions. So gate on
        // (largest block >= need) AND (TOTAL free >= need + hold), not the old
        // (largest >= need + hold), which demanded ~1.08 MB contiguous and failed on a
        // level TRANSITION where the freed heap is fragmented into ~770 KB + ~510 KB even
        // though ~1.33 MB is free overall — dropping E1M2 etc. to the ~24 s scattered path.
        if (avail >= need && Z_TotalFree() >= need + packhold)
            budget = need;                       // arena = one contiguous chunk, no grow
        else
        { w_pack_avail = 0; budget = w_scattered_arena_budget(avail, maptotal); }  // pack won't fit lean: scattered
    }
    else
    {
        budget = w_scattered_arena_budget(avail, maptotal);
    }

#ifdef GEN_MAPPACK_MANIFEST
    // Manifest gen only needs the MARK set (dumped in W_PrecacheEnd), not a filled arena.
    // Reserve a tiny arena so W_PrecacheLumpNum still marks, but leave the rest of the
    // heap free so map data can load on the heaviest maps. No pool bytes are actually
    // loaded in manifest-generation builds.
    budget = 64u * 1024u;
#endif
    for (int i = 0; i < W_ARENA_MAX_CHUNKS; i++)
        { w_arena[i] = 0; w_arena_cap[i] = w_arena_off[i] = 0; }
    w_narena = 0;
    w_arena_full = 0;
    w_arena_purged = 0;
    w_protect = 0;
    if (budget >= 4096)
    {
        w_arena[0] = (byte *)Z_Malloc((int)budget, PU_LEVEL, NULL);
        w_arena_cap[0] = budget;
        w_narena = 1;
    }

    // Re-make staging LAST — after the purge, the `avail` measurement AND the arena.
    // w_stage is 45 KB of unpurgeable PU_STATIC. Re-made at the TOP of this function it
    // was allocated while the outgoing level's PU_CACHE still filled the heap, so it took
    // whatever 45 KB hole existed — possibly inside the region the purge below was about
    // to coalesce — and was then counted against the very `avail` used to fit the arena.
    // Taking it from the remainder AFTER the arena removes staging placement as an input
    // to the fit test. (Measured neutral on a bot run, where it happened to land clear of
    // the big block; this is about not letting luck decide.) Safe: w_pack_lookup() above
    // stages through its own sector buffer, and the pack's first consumer (W_LoadMapPack)
    // runs after we return. The budget always leaves room — GAMEPLAY_RESERVE is sized
    // with w_stage resident.
    w_stage_ensure();

    lprintf(LO_INFO, "W_PrecacheReserve: avail %u map %u -> arena %u%s",
            avail, maptotal, budget, w_pack_avail ? " [pack]" : "");
}

// Cache every remaining PU_STATIC lump NOW, at boot, while the heap is still one
// contiguous block below the first level's arena.
//
// PU_STATIC lumps are whole-run: nothing ever purges or frees them, so caching one
// lazily on first use costs exactly the same RAM as caching it here — the ONLY
// difference is WHERE it lands. Cached on demand (the first HUD frame, the first
// intermission, the first menu), the zone rover is already deep inside the big free
// region, so the block is stranded mid-heap; freeing the level then leaves it as an
// unpurgeable WALL that permanently splits the free space in two. Every later
// W_PrecacheReserve measures Z_LargestFreeBlock() across that split, so `avail`
// reports a fraction of the real capacity and the map pack fails its one-chunk fit
// test -> the whole level load drops to the scattered seek-storm (39 CD commands vs
// 4, seconds of black loading screen). Loading them here instead puts them beside the
// other boot statics at the bottom of the heap, so every transition sees ONE big
// block. The boot pack already residents most of them; this closes the stragglers.
void W_PreloadStatics(void)
{
    int n = 0;
    for (int i = 0; i < w_numlumps; i++)
        if (lumptag[i] == PU_STATIC && !lumpcache[i] && LONG(w_dir[i].size) > 0)
        {
            W_CacheLumpNum(i);
            n++;
        }
    lprintf(LO_INFO, "W_PreloadStatics: %d late PU_STATIC lumps made resident", n);
}

void W_PrecacheBegin(void)
{
    if (w_pc_mark)
        memset(w_pc_mark, 0, (unsigned)w_numlumps);
    w_pc_nseq = 0;
}

// Grow the arena by one more PU_LEVEL chunk when the current chunks are full. The
// space the map-load reserve no longer needs is mostly the map's texture-patch
// DECOMPRESSED copies (PU_CACHE, cached by P_LoadSideDefs2 to read widths) — those
// re-inflate from the arena during play (never the CD), so we PURGE them ONCE to
// reclaim their bytes, then grow into the freed holes. They're interleaved with the
// pinned map PU_LEVEL structs, so the free space is fragmented: each grow takes the
// largest current hole (capped so total free stays >= GAMEPLAY_RESERVE), and a later
// overflow grabs the next hole — up to W_ARENA_MAX_CHUNKS chunks. Returns the new
// chunk index, or -1 if growing would cross the gameplay floor / the chunk cap.
static int w_arena_grow(unsigned need)
{
    if (w_narena >= W_ARENA_MAX_CHUNKS) return -1;

    if (!w_arena_purged)
    {
        Z_FreeTags(PU_CACHE, PU_CACHE);   // map-load patches re-inflate from the arena
        w_arena_purged = 1;

        // Hold the ENTIRE largest hole hostage so the arena grow below can't grab it.
        // It becomes a permanent contiguous decompress slot for gameplay: the renderer
        // decompresses one graphic at a time into evictable PU_CACHE, so this fixed gap
        // (bounded by pinned arena chunks) recycles and keeps the biggest contiguous
        // block available all level. Without it, grow grabbed every big hole and left
        // the free space fragmented below one lump — the OOM that crashed E1M3 (226 KB
        // free but < 18 KB contiguous, vs a 17560-byte decompress). We preserve the
        // single largest hole unconditionally (it's the best contiguous slot we can
        // give the renderer); freed at W_BeginGameplay.
        unsigned big0 = Z_LargestFreeBlock();
        if (big0 >= 4096)
            w_protect = Z_Malloc((int)big0, PU_LEVEL, NULL);
    }

    unsigned freeleft = Z_TotalFree();
    if (freeleft < GAMEPLAY_RESERVE + need) return -1;

    unsigned want = freeleft - GAMEPLAY_RESERVE;   // grabbable without crossing floor
    unsigned big  = Z_LargestFreeBlock();          // biggest single contiguous hole
    unsigned budget = (want < big) ? want : big;
    if (budget < need) return -1;

    int i = w_narena;
    w_arena[i] = (byte *)Z_Malloc((int)budget, PU_LEVEL, NULL);
    w_arena_cap[i] = budget;
    w_arena_off[i] = 0;
    w_narena++;
    lprintf(LO_INFO, "W_Precache: grew chunk %d = %u (free %u big %u)",
            i, budget, freeleft, big);
    return i;
}

// MARK one graphics lump for precache. The actual CD reads are deferred to
// W_PrecacheEnd, which fills the arena in DISC ORDER: reading the marks one lump
// at a time here (as this used to) issued ~600 separate SCSI commands to scattered
// sectors, and pcfxemu charges a per-command SEEK (33 ms min, 150–283 ms for a real
// jump — see memory/doom-pcfx-cd-seek-model.md) for every non-adjacent read, so the
// level load spent tens of seconds seeking. Marking + a disc-ordered coalesced fill
// turns that into a handful of sequential sweeps whose reads abut (zero seek).
void W_PrecacheLumpNum(int lump, int prio)
{
    if (lump < 0 || lump >= w_numlumps || w_narena == 0) return;
    if (w_comp[lump]) return;
    unsigned disc = lump_disc_len(lump);
    if (disc == 0 || disc > w_stage_bytes) return;
    if (!w_pc_mark || !w_pc_seq) return;
    if (prio < 1) prio = 1;
    if (w_pc_mark[lump] == 0)                    // first mark: record call order
        w_pc_seq[w_pc_nseq++] = lump;
    if ((byte)prio > w_pc_mark[lump])            // keep the highest priority asked
        w_pc_mark[lump] = (byte)prio;
}

// RESERVE an arena slot for one marked lump and point w_comp[lump] at it (bytes are
// filled later, in disc order). Reserving in R_PrecacheLevel's CALL order — flats,
// then patches, then map-thing sprites, then the in-hand weapon, then runtime effects
// — means that when the pool overflows the resident budget, the dropped lumps are the
// last-reserved ones (transient effects), exactly as the old per-lump fill dropped
// them. If every chunk is full and the arena can't grow, the lump stays un-resident
// (w_comp[lump] == NULL) and CD-loads on first use. Returns 1 if reserved.
static int w_arena_reserve(int lump)
{
    if (w_comp[lump]) return 1;
    unsigned disc = lump_disc_len(lump);
    unsigned need = (disc + 3u) & ~3u;

    // Pack into the first chunk with room (minimises tail slack); if none fits, grow.
    int c = -1;
    for (int i = 0; i < w_narena; i++)
        if (w_arena_off[i] + need <= w_arena_cap[i]) { c = i; break; }
    if (c < 0)
        c = w_arena_grow(need);
    if (c < 0)
    {
        if (!w_arena_full)
            lprintf(LO_WARN, "W_Precache: arena full (%u used) — some gfx will CD-load",
                    w_precache_used());
        w_arena_full = 1;
        return 0;
    }

    w_comp[lump] = w_arena[c] + w_arena_off[c];
    w_arena_off[c] += need;
    return 1;
}

void W_PrecacheEnd(void)
{
#ifdef GEN_MAPPACK_MANIFEST
    // Manifest gen: dump the FULL MARKED set (the authoritative per-map asset list) in
    // PRIORITY + call order — this is what the offline pack builder packs, and the pack
    // sizes chunk0 to hold all of it, so nothing is dropped. No arena fill happens here.
    if (w_pc_mark && w_pc_seq)
    {
        const char *mn = (w_pc_maplump >= 0) ? w_dir[w_pc_maplump].name : "????????";
        int n = 0;
        for (int k = 0; k < w_numlumps; k++) if (w_pc_mark[k]) n++;
        lprintf(LO_INFO, "MAPPACK %.8s %d", mn, n);
        for (int prio = W_PC_FLAT; prio >= W_PC_EFFECT; prio--)
            for (int s = 0; s < w_pc_nseq; s++)
            {
                int L = w_pc_seq[s];
                if (w_pc_mark[L] == prio)
                    lprintf(LO_INFO, "MP %d %.8s", L, w_dir[L].name);
            }
    }
    return;
#endif
    // Fill the arena from CD in DISC ORDER, coalescing consecutive marked lumps into
    // one w_stage-sized read each. Lump index == disc order (the bake lays lumps out
    // monotonically) and lumps are physically contiguous, so each read starts where
    // the previous ended: pcfxemu charges ~0 seek for an abutting read but 33–283 ms
    // for a jump. A read window spans from the first marked lump to the last that
    // still fits w_stage; any UNMARKED lumps caught inside are read through (cheaper
    // than the seek that skipping them would cost) but not copied. This replaces the
    // old per-mark seeking read and is the level-load speed fix.
    if (w_pc_mark && w_pc_seq && w_narena > 0)
    {
        // PASS 1 — reserve arena slots. Highest priority first, and within a priority
        // in R_PrecacheLevel's call order (w_pc_seq): this reproduces the old per-lump
        // fill's drop set exactly (an overflow sheds the last-reserved transient-effect
        // sprites), so gameplay stays CD-free on the maps that already were. No I/O.
        for (int prio = W_PC_FLAT; prio >= W_PC_EFFECT; prio--)
            for (int s = 0; s < w_pc_nseq; s++)
            {
                int L = w_pc_seq[s];
                if (w_pc_mark[L] == prio)
                    w_arena_reserve(L);
            }

#ifdef W_PRECACHE_NOCOALESCE
        // PASS 2 baseline: one seeking CD read per reserved lump, in disc order (stage
        // then copy, since the CD read is sector-rounded). Enable with
        // EXTRA="-DW_PRECACHE_NOCOALESCE" to measure the seek cost this coalescing saves.
        for (int i = 0; i < w_numlumps; i++)
            if (w_comp[i])
            {
                unsigned disc = lump_disc_len(i);
                unsigned pos  = (unsigned)LONG(w_dir[i].filepos);
                if (wad_read_window(pos, w_stage, disc, i, i, 0))
                    memcpy(w_comp[i], w_stage, disc);
                else
                    w_comp[i] = NULL;         // corrupt on both copies: don't show garbage
            }
#else
        // PASS 2 — fill the reserved slots' bytes in DISC order as a STREAMING reader.
        // Lumps are physically contiguous on disc and pcfxemu charges a SEEK only when
        // a read does not start where the previous ended (0 for an abutting read, 33–
        // 283 ms for a jump). So each window prefers to start at the CD head (`head`,
        // the sector just past the last read) — reading straight through any small gap
        // of un-reserved lumps, which is far cheaper than the seek that skipping them
        // would cost. Only when the next reserved lump is so far past the head that it
        // (and the intervening gap) would not fit one w_stage window do we JUMP the
        // head to it and pay the seek. Within a dense namespace (flats/patches/sprites)
        // this is one zero-seek sweep; the handful of seeks left are the big jumps
        // between namespaces. Collapses the ~600 seeking per-lump reads to a few sweeps.
        unsigned head = 0xffffffffu;                          // sector past the last read
        int i = 0;
        while (i < w_numlumps)
        {
            if (!w_comp[i]) { i++; continue; }

            unsigned lpos = (unsigned)LONG(w_dir[i].filepos);
            unsigned lend = lpos + lump_disc_len(i);
            // Abut (stream through the gap) when this lump still fits a window measured
            // from the head; otherwise jump the head to this lump.
            unsigned base;
            if (head != 0xffffffffu && lpos >= head * SECTOR &&
                round_sectors(lend - head * SECTOR) <= w_stage_bytes)
                base = head * SECTOR;
            else
                base = lpos;

            int last = i;
            for (int nx = i + 1; nx < w_numlumps; nx++)
            {
                if (!w_comp[nx]) continue;                    // gap lump: read through
                unsigned nx_pos = (unsigned)LONG(w_dir[nx].filepos);
                if (nx_pos < base) break;                     // directory not disc-monotonic
                unsigned nx_end = nx_pos + lump_disc_len(nx);
                if (round_sectors(nx_end - base) > w_stage_bytes) break; // overflow stage
                last = nx;
            }

            unsigned span = (unsigned)LONG(w_dir[last].filepos)
                          + lump_disc_len(last) - base;
            // abutting -> ~0 seek; verified, and repaired from the disc's duplicate
            // copy if a burn defect spoiled this window (only the RESERVED lumps are
            // checked — the gap lumps read through can run past the window end).
            int clean = wad_read_window(base, w_stage, span, i, last, 1);

            for (int k = i; k <= last; k++)
                if (w_comp[k])
                {
                    const byte *src = w_stage + ((unsigned)LONG(w_dir[k].filepos) - base);
                    if (clean || lump_sum_ok(k, src))
                        memcpy(w_comp[k], src, lump_disc_len(k));
                    else
                        // Still wrong after the duplicate + a retry. Leave it un-resident:
                        // W_CacheLumpNum then draws the empty lump (an invisible sprite)
                        // instead of garbage, and the arena slot is simply unused.
                        w_comp[k] = NULL;
                }

            head = base / SECTOR + round_sectors(span) / SECTOR;
            i = last + 1;
        }
        /* One-shot geometry diagnostic: how far the reserved set spans on disc, and
         * how many reads jumped (>6 sectors = a pcfxemu seek). Guides layout work. */
        {
            unsigned lo = 0xffffffffu, hi = 0, nres = 0;
            for (int k = 0; k < w_numlumps; k++)
                if (w_comp[k])
                {
                    unsigned p = (unsigned)LONG(w_dir[k].filepos) / SECTOR;
                    unsigned e = p + round_sectors(lump_disc_len(k)) / SECTOR;
                    if (p < lo) lo = p; if (e > hi) hi = e; nres++;
                }
            lprintf(LO_INFO, "W_Load geom: %u reserved lumps, span %u sectors (lo %u hi %u)",
                    nres, hi - lo, lo, hi);
        }
#endif
    }

    // Return each chunk's over-reserved tail to the heap so it becomes decompressed-
    // cache room instead of pinned slack.
    for (int i = 0; i < w_narena; i++)
        if (w_arena[i] && w_arena_off[i] < w_arena_cap[i])
        {
            Z_TrimBlock(w_arena[i], w_arena_off[i]);
            w_arena_cap[i] = w_arena_off[i];
        }
    lprintf(LO_INFO, "W_Precache: arena %u used (%d chunks)",
            w_precache_used(), w_narena);
    /* Level-load CD cost. Each command that doesn't abut the previous read pays a
     * pcfxemu SEEK (33–283 ms), so the command count is the seek proxy this coalesced
     * fill drives down (was ~1 per precached lump). Sectors ≈ transfer floor. */
    lprintf(LO_INFO, "W_Load: %u CD cmds, %u sectors, ~%u ms seek + ~%u ms xfer",
            g_cd_read_cmds - w_pc_cmds0, g_cd_read_sectors - w_pc_sect0,
            g_cd_seek_ms - w_pc_seek0,
            ((g_cd_read_sectors - w_pc_sect0) * 20u) / 3u);  /* 2048B / 307200Bps ≈ 6.67ms */
}

// ---------------------------------------------------------------------------
// W_LoadMapPack — load this map's PRE-ASSEMBLED contiguous asset pack
// (pcfx_mappacks.bin) instead of the scattered per-lump precache. The pack holds, in
// ONE gap-free region: first the w_pack_n_geo map GEOMETRY lumps (THINGS..BLOCKMAP,
// which P_SetupLevel reads), then every GRAPHICS lump the level precaches — all in
// disc-friendly order. This is called EARLY in P_SetupLevel (before the P_Load*
// geometry reads and R_GetTexture(sky)), so the whole level streams into RAM in a
// single back-to-back sweep whose reads abut (pcfxemu charges ~0 seek) — collapsing
// the residual level-load seek. Geometry lumps decode straight into lumpcache[] (so the
// later P_SetupLevel / sky reads are RAM cache hits, zero CD); graphics lumps are
// copied compressed into the arena (inflated on draw). Returns 1 if the map has a pack
// (and it was loaded), 0 if not (caller falls back to the scattered marking path — e.g.
// a PWAD map, or a build without the pack blob). See tools/gen_pcfx_packs.py.
// ---------------------------------------------------------------------------
int W_LoadMapPack(void)
{
    if (!w_pack_avail || w_narena == 0 || !w_stage) return 0;   // lookup done in W_PrecacheReserve
    unsigned nlumps  = w_pack_nlumps;
    unsigned n_geo   = w_pack_n_geo;
    unsigned hdr_off = w_pack_hdr_off;
    unsigned pay_off = w_pack_pay_off;
    const char *want = w_dir[w_pc_maplump].name;

    // Header: nlumps x {lumpnum, payload_off, disclen, lz4, checksum}, geometry (first
    // n_geo) then graphics, in pack (== disc) order. Reserve an arena slot for every
    // GRAPHICS lump in that order so the chunk packing (and thus which lumps drop if it
    // overflows) is identical to the scattered path — the same set is guaranteed to fit.
    // The geometry lumps go to lumpcache, NOT the arena. Uses chunk0 (from
    // W_PrecacheReserve) + grow/purge/protector machinery, so map-load memory is safe.
    //
    // The header is what locates and validates every lump below, so it is verified (and
    // repaired from the disc's duplicate copy) before anything is read through it; if it
    // cannot be made good we return 0 and the caller streams the level the scattered way.
    unsigned hdrbytes = nlumps * PACK_HDRENT;
    byte *hdr = (byte *)Z_Malloc((int)round_sectors(hdrbytes), PU_STATIC, NULL);
    if (!hdr) return 0;
    if (!pack_read_header(hdr_off, hdr, hdrbytes, w_pack_hdr_sum))
    {
        Z_Free(hdr);
        return 0;
    }

    for (unsigned j = n_geo; j < nlumps; j++)
    {
        unsigned lumpnum = PK_LUMP(hdr, j);
        if (lumpnum < (unsigned)w_numlumps)
            w_arena_reserve((int)lumpnum);
    }

    // FAST PATH — the whole GRAPHICS payload in ONE contiguous read straight into the
    // arena. When the pack fit one chunk (w_narena==1, the case for every E1 map) and the
    // graphics tail starts on a sector boundary (packer sector-aligns geo_end), the on-disc
    // graphics region is a byte-IMAGE of arena chunk 0 (arena_off == disc_off - geo_end,
    // since both pack lumps 4-aligned cumulative in the same reserve order). So we read the
    // ENTIRE graphics payload in a single read directly into w_arena[0] — no w_stage
    // windowing, no per-lump memcpy. The graphics tail is the bulk of the load (E1M1 483 KB
    // of 521 KB), so this is THE win: one ~0-seek read instead of ~7-13 windows (each CD
    // command costs real emulator time — SCSI turnaround — far beyond the per-sector
    // transfer). The geometry PREFIX is decoded into lumpcache separately (its own reads):
    // one w_stage read for small maps, or a few windows for a big map like E1M6 whose
    // 219 KB of geometry exceeds w_stage — decoupled so an oversized geometry prefix no
    // longer disqualifies the graphics single-read (it did, dropping E1M6 to 23 windows).
    // Falls back to the fully-windowed loop below only if the pack grew to >1 chunk or the
    // graphics tail isn't a sector-aligned image of the arena (stale/old blob).
    unsigned base_pay_sec = pay_off / SECTOR;
    unsigned gfx_first = (n_geo < nlumps) ? PK_POFF(hdr, n_geo) : 0;  // == geo_end
    if (w_narena == 1 && n_geo < nlumps && (gfx_first % SECTOR) == 0
        && PK_LUMP(hdr, n_geo) < (unsigned)w_numlumps
        && w_comp[PK_LUMP(hdr, n_geo)] == w_arena[0])
    {
        // Geometry prefix [0, gfx_first): decode into lumpcache in w_stage-sized windows
        // (abutting -> ~0 seek). Usually one read; big maps take a few. A single geometry
        // lump larger than w_stage (rare) is skipped -> read on demand by P_SetupLevel.
        unsigned g = 0;
        while (g < n_geo)
        {
            unsigned first_poff = PK_POFF(hdr, g);
            unsigned first_dlen = PK_DLEN(hdr, g);
            unsigned floor_poff = first_poff & ~(SECTOR - 1u);
            if (round_sectors(first_poff + first_dlen - floor_poff) > w_stage_bytes)
            { g++; continue; }
            unsigned last = g;
            for (unsigned nx = g + 1; nx < n_geo; nx++)
            {
                unsigned nx_end = PK_POFF(hdr, nx) + PK_DLEN(hdr, nx);
                if (round_sectors(nx_end - floor_poff) > w_stage_bytes) break;
                last = nx;
            }
            unsigned end_poff = PK_POFF(hdr, last) + PK_DLEN(hdr, last);
            // Corrupt on both copies: decode nothing from this window and leave those
            // lumps UNcached, so P_SetupLevel's own W_CacheLumpNum streams each from the
            // IWAD blob instead — a different region of the disc, verified in its own
            // right. Never decode garbage: a bad LZ4 block overruns its cache block.
            if (pack_read_window(base_pay_sec + floor_poff / SECTOR, w_stage,
                                 end_poff - floor_poff, hdr, g, last, floor_poff))
                for (unsigned k = g; k <= last; k++)
                {
                    unsigned lumpnum = PK_LUMP(hdr, k);
                    if (lumpnum < (unsigned)w_numlumps && !lumpcache[lumpnum])
                        decode_mem_into_cache((int)lumpnum,
                                              w_stage + (PK_POFF(hdr, k) - floor_poff));
                }
            g = last + 1;
        }
        // Graphics tail: ONE contiguous read straight into the arena chunk (fills it
        // exactly — chunk0 cap == round_sectors(gfx_bytes) from W_PrecacheReserve).
        pcfx_mappack_read(base_pay_sec + gfx_first / SECTOR, w_arena[0], w_pack_gfx_bytes);
        // Verified IN PLACE (the arena is the read destination) but PER LUMP, not as one
        // window: this single read is the whole level's graphics (~600 KB), so a
        // whole-window retry would re-transfer all of it — seconds of load — to recover
        // one bad sprite. Instead each bad lump is re-read on its own (a few KB, via
        // w_stage) from the duplicate copy, and only what no copy can supply is
        // un-reserved so the renderer draws nothing for it rather than garbage.
        for (unsigned k = n_geo; k < nlumps; k++)
        {
            unsigned lumpnum = PK_LUMP(hdr, k);
            unsigned poff = PK_POFF(hdr, k), dlen = PK_DLEN(hdr, k);
            byte *dst = w_arena[0] + (poff - gfx_first);
            if (lumpnum >= (unsigned)w_numlumps || !w_comp[lumpnum]) continue;
            if (pack_lump_ok(hdr, k, dst)) continue;
            unsigned floor_poff = poff & ~(SECTOR - 1u);
            unsigned span = poff + dlen - floor_poff;
            if (round_sectors(span) <= w_stage_bytes &&
                pack_read_window(base_pay_sec + floor_poff / SECTOR, w_stage, span,
                                 hdr, k, k, floor_poff))
                memcpy(dst, w_stage + (poff - floor_poff), dlen);
            else
                w_comp[lumpnum] = NULL;
        }
        goto packed_filled;
    }

    // Fill: stream the gap-free payload in w_stage windows. Each window starts at the
    // SECTOR floor of its first lump and spans as many consecutive pack lumps as fit
    // w_stage; the payload is gap-free and in order, so consecutive windows abut -> ~0
    // seek. For each lump: a GEOMETRY lump (index < n_geo) is DECODED into its lumpcache
    // slot (P_SetupLevel then RAM-hits it); a GRAPHICS lump is copied compressed into its
    // reserved arena slot (skip any dropped). A single lump whose on-disc bytes exceed
    // w_stage is skipped here and left to the normal on-demand CD path.
    unsigned j = 0;
    while (j < nlumps)
    {
        unsigned first_poff = PK_POFF(hdr, j);
        unsigned first_dlen = PK_DLEN(hdr, j);
        unsigned floor_poff = first_poff & ~(SECTOR - 1u);   // sector floor within payload
        if (round_sectors(first_poff + first_dlen - floor_poff) > w_stage_bytes)
        { j++; continue; }                                   // single lump too big for staging
        unsigned last = j;
        for (unsigned nx = j + 1; nx < nlumps; nx++)
        {
            unsigned nx_end = PK_POFF(hdr, nx) + PK_DLEN(hdr, nx);
            if (round_sectors(nx_end - floor_poff) > w_stage_bytes) break;
            last = nx;
        }
        unsigned end_poff = PK_POFF(hdr, last) + PK_DLEN(hdr, last);
        unsigned span = end_poff - floor_poff;
        int clean = pack_read_window(base_pay_sec + floor_poff / SECTOR, w_stage, span,
                                     hdr, j, last, floor_poff);

        for (unsigned k = j; k <= last; k++)
        {
            unsigned lumpnum = PK_LUMP(hdr, k);
            unsigned poff    = PK_POFF(hdr, k);
            unsigned dlen    = PK_DLEN(hdr, k);
            const byte *src  = w_stage + (poff - floor_poff);
            if (lumpnum >= (unsigned)w_numlumps) continue;
            if (!clean && !pack_lump_ok(hdr, k, src))
            {
                // Corrupt on both copies. Geometry: leave it uncached so P_SetupLevel
                // streams it from the IWAD blob. Graphics: un-reserve it so the renderer
                // draws nothing rather than garbage (see W_CacheLumpNum).
                if (k >= n_geo) w_comp[lumpnum] = NULL;
                continue;
            }
            if (k < n_geo)
            {
                if (!lumpcache[lumpnum])
                    decode_mem_into_cache((int)lumpnum, src);   // geometry -> lumpcache
            }
            else if (w_comp[lumpnum])
            {
                memcpy(w_comp[lumpnum], src, dlen);             // graphics -> arena (compressed)
            }
        }
        j = last + 1;
    }

packed_filled:
    Z_Free(hdr);

    // Trim each chunk's over-reserved tail back to the heap (as W_PrecacheEnd does).
    for (int i = 0; i < w_narena; i++)
        if (w_arena[i] && w_arena_off[i] < w_arena_cap[i])
        {
            Z_TrimBlock(w_arena[i], w_arena_off[i]);
            w_arena_cap[i] = w_arena_off[i];
        }
    w_pack_loaded = 1;
    lprintf(LO_INFO, "W_LoadMapPack: %.8s %u lumps (%u geo), %u used (%d chunks)",
            want, nlumps, n_geo, w_precache_used(), w_narena);
    return 1;
}

int W_MapPackLoaded(void) { return w_pack_loaded; }

// ---------------------------------------------------------------------------
// W_LoadBootPack — the BOOT edition of W_LoadMapPack, run once at W_Init. Boot's
// whole-run resident set (STCFN* font, ST* HUD/face/number graphics, PLAYPAL,
// COLORMAP, TEXTURE1 — ~140 PU_STATIC lumps) otherwise streams on demand in
// engine-touch order, which is NOT disc order, so even preload_static_run's
// run-coalescing still jumps between disc clusters — a pcfxemu SEEK (33–283 ms) per
// jump, the boot-time seek storm (plan.md "BOOT pack"). The offline builder
// pre-assembles those exact lumps (dumped ground-truth as the "BOOT" manifest entry)
// gap-free and contiguous in pcfx_mappacks.bin, so here we stream the whole payload
// in a handful of ABUTTING reads (~0 seek) and DECODE each lump straight into its
// permanent PU_STATIC cache. Every later W_CacheLumpNum for a boot lump is then a RAM
// cache hit. TITLEPIC is intentionally NOT packed (a single big read, and packing a
// transient PU_LEVEL lump here would fragment level-load headroom — see the capture
// filter in W_CacheLumpNum). No-op when no BOOT entry exists (older blob / PWAD):
// boot falls back to the scattered preload_static_run path, exactly as before.
// ---------------------------------------------------------------------------
void W_LoadBootPack(void)
{
#ifdef GEN_BOOTPACK_MANIFEST
    return;   // manifest gen: capture the ground-truth scattered set, load no pack
#endif
    if (!w_stage || !lumpcache) return;

    // Index (sector 0, see w_pack_index_load — verified and duplicate-repaired there).
    // Find the synthetic "BOOT" entry (packed like any map; n_geo is 0 — every boot lump
    // decodes into lumpcache below). This W_Init call also populates the shared resident
    // index cache (w_pack_index), so no later level load re-reads it.
    pcfx_boot_progress_set_label("BOOT PACK INDEX");
    const byte *idx = w_pack_index_load();
    if (!idx) return;
    const byte *bent = pack_find(idx, "BOOT");
    if (!bent) return;
    unsigned nlumps    = mp_u32(bent + 8);
    unsigned hdr_off   = mp_u32(bent + 12);
    unsigned pay_off   = mp_u32(bent + 16);
    unsigned pay_bytes = mp_u32(bent + 20);
    unsigned hdr_sum   = mp_u32(bent + 32);
    if (nlumps == 0) return;

    // Read the whole gap-free payload in ONE command and decode every boot lump from it,
    // instead of the old ~4 w_stage windows. Each CD command costs real emulator time
    // (SCSI turnaround, far beyond the per-sector transfer), so collapsing commands is the
    // boot-speed lever. The payload (~62 KB) is bigger than w_stage, so it needs its own
    // buffer — but that buffer must not leave a permanent mid-heap hole (the boot lump
    // caches are pinned PU_STATIC, so a temp allocated BEFORE them can't coalesce on free
    // and would steal ~64 KB of level-load headroom — enough to bump a tight map like E1M6
    // off its one-chunk pack fit). To stay footprint-neutral: (1) read the small header
    // into the already-resident w_stage (no new alloc); (2) PRE-ALLOCATE every boot lump's
    // cache block; (3) allocate the payload buffer LAST, so on Z_Free it coalesces straight
    // back into the free pool. Two commands (header + payload), zero permanent cost.
    unsigned nbytes_hdr = nlumps * PACK_HDRENT;
    if (nbytes_hdr <= w_stage_bytes)
    {
        byte *hdr = w_stage;                                 // header -> resident staging
        pcfx_boot_progress_set_label("BOOT PACK HEADER");
        if (!pack_read_header(hdr_off, hdr, nbytes_hdr, hdr_sum))
            return;         // unusable header: boot falls back to preload_static_run

        // Pre-allocate each boot lump's exact-size PU_STATIC cache (no decode yet), so the
        // payload buffer below is the TOP allocation and frees cleanly.
        for (unsigned k = 0; k < nlumps; k++)
        {
            unsigned lumpnum = PK_LUMP(hdr, k);
            if (lumpnum >= (unsigned)w_numlumps || lumpcache[lumpnum]) continue;
            int size = LONG(w_dir[lumpnum].size);
            Z_Malloc(size > 0 ? size : 1, lumptag[lumpnum], &lumpcache[lumpnum]);
        }

        byte *pay = (byte *)Z_Malloc((int)round_sectors(pay_bytes), PU_STATIC, NULL);
        if (pay)
        {
            pcfx_boot_progress_set_label("BOOT PACK DATA");
            // One contiguous read, verified and duplicate-repaired as a single window.
            int clean = pack_read_window(pay_off / SECTOR, pay, pay_bytes,
                                         hdr, 0, nlumps - 1, 0);
            for (unsigned k = 0; k < nlumps; k++)
            {
                unsigned lumpnum = PK_LUMP(hdr, k);
                unsigned poff    = PK_POFF(hdr, k);
                if (lumpnum >= (unsigned)w_numlumps || !lumpcache[lumpnum]) continue;
                int size = LONG(w_dir[lumpnum].size);
                if (size <= 0) continue;
                if (!clean && !pack_lump_ok(hdr, k, pay + poff))
                {
                    // Corrupt on both copies: free the pre-allocated cache block so the
                    // lump streams (verified) from the IWAD blob on first use, and never
                    // inflate a garbage LZ4 block into it.
                    Z_Free(lumpcache[lumpnum]);
                    lumpcache[lumpnum] = 0;
                    continue;
                }
                if (lump_is_lz4((int)lumpnum)) lz4_depack(pay + poff, lumpcache[lumpnum]);
                else                           memcpy(lumpcache[lumpnum], pay + poff, (unsigned)size);
            }
            Z_Free(pay);                                          // top block -> coalesces
            lprintf(LO_INFO, "W_LoadBootPack: %u boot lumps resident", nlumps);
            return;
        }
        // pay alloc failed (essentially impossible — the heap is near-empty here). The
        // pre-allocated caches are still EMPTY, so free them (restoring NULL lumpcache) and
        // let each boot lump stream on demand later. Do NOT fall through to a decode path
        // that would leave them cached-but-garbage.
        for (unsigned k = 0; k < nlumps; k++)
        {
            unsigned lumpnum = PK_LUMP(hdr, k);
            if (lumpnum < (unsigned)w_numlumps && lumpcache[lumpnum])
            { Z_Free(lumpcache[lumpnum]); lumpcache[lumpnum] = 0; }
        }
        return;
    }

    // Fallback: header alone + windowed payload (only if the header didn't fit w_stage —
    // never happens for the ground-truth boot set; kept for robustness / non-BOOT blobs).
    byte *hdr = (byte *)Z_Malloc((int)round_sectors(nlumps * PACK_HDRENT), PU_STATIC, NULL);
    if (!hdr) return;
    pcfx_boot_progress_set_label("BOOT PACK HEADER");
    if (!pack_read_header(hdr_off, hdr, nlumps * PACK_HDRENT, hdr_sum))
    { Z_Free(hdr); return; }
    unsigned base_pay_sec = pay_off / SECTOR;
    unsigned j = 0;
    while (j < nlumps)
    {
        unsigned first_poff = PK_POFF(hdr, j);
        unsigned first_dlen = PK_DLEN(hdr, j);
        unsigned floor_poff = first_poff & ~(SECTOR - 1u);
        if (round_sectors(first_poff + first_dlen - floor_poff) > w_stage_bytes)
        { j++; continue; }                          // single lump too big for staging
        unsigned last = j;
        for (unsigned nx = j + 1; nx < nlumps; nx++)
        {
            unsigned nx_end = PK_POFF(hdr, nx) + PK_DLEN(hdr, nx);
            if (round_sectors(nx_end - floor_poff) > w_stage_bytes) break;
            last = nx;
        }
        unsigned end_poff = PK_POFF(hdr, last) + PK_DLEN(hdr, last);
        unsigned span = end_poff - floor_poff;
        pcfx_boot_progress_set_label("BOOT PACK DATA");
        int clean = pack_read_window(base_pay_sec + floor_poff / SECTOR, w_stage, span,
                                     hdr, j, last, floor_poff);

        for (unsigned k = j; k <= last; k++)
        {
            unsigned lumpnum = PK_LUMP(hdr, k);
            unsigned poff    = PK_POFF(hdr, k);
            const byte *src  = w_stage + (poff - floor_poff);
            if (lumpnum >= (unsigned)w_numlumps || lumpcache[lumpnum]) continue;
            if (!clean && !pack_lump_ok(hdr, k, src))
                continue;              // uncached: streams (verified) from the IWAD blob
            decode_mem_into_cache((int)lumpnum, src);
        }
        j = last + 1;
    }

    Z_Free(hdr);
    lprintf(LO_INFO, "W_LoadBootPack: %u boot lumps resident", nlumps);
}

// -DGEN_BOOTPACK_MANIFEST only: dump the captured boot-resident set (see the capture
// in W_CacheLumpNum) as a `MAPPACK BOOT n` block + `MP <num> <name>` lines — the same
// serial-log format the per-map dump uses, so gen_mappack_manifest.sh's extractor and
// gen_pcfx_packs.py consume it with no special-casing. Dumps once, at the first title
// frame (D_Display), by which point every boot lump — including TITLEPIC — was touched.
void W_BootManifestDump(void)
{
#ifdef GEN_BOOTPACK_MANIFEST
    if (w_boot_dumped || !w_boot_seq) return;
    w_boot_dumped = 1;
    w_boot_capturing = 0;
    lprintf(LO_INFO, "MAPPACK %s %d", "BOOT", w_boot_nseq);
    for (int s = 0; s < w_boot_nseq; s++)
    {
        int L = w_boot_seq[s];
        lprintf(LO_INFO, "MP %d %.8s", L, w_dir[L].name);
    }
#else
    (void)0;
#endif
}

// Log the whole level's CD-load cost (map lumps + pool). Called at the end of
// R_PrecacheLevel — by then P_SetupLevel's map-lump/texture reads and the pool load
// are all done, so this is the total. The seek estimate mirrors pcfxemu's model.
void W_ReportLoadCost(void)
{
    lprintf(LO_INFO, "W_Load: %u CD cmds, %u sectors, ~%u ms seek + ~%u ms xfer%s",
            g_cd_read_cmds - w_pc_cmds0, g_cd_read_sectors - w_pc_sect0,
            g_cd_seek_ms - w_pc_seek0,
            ((g_cd_read_sectors - w_pc_sect0) * 20u) / 3u,
            w_pack_loaded ? " [pack]" : " [scattered]");
    /* Disc health, whole run: reads a retry (usually the duplicate copy) put right, and
     * reads no copy could supply. Both should be 0 on a good burn — a tester seeing
     * repairs > 0 has a marginal disc that WOULD have shown corrupt sprites before. */
    if (w_dup_repairs || w_dup_lost)
        lprintf(LO_WARN, "W_Load: disc integrity: %u read(s) repaired, %u lost",
                w_dup_repairs, w_dup_lost);
}

// Called when the level's PU_LEVEL data (incl. the arena) is freed, so the
// w_comp[] pointers into the now-gone arena are dropped before anything reads them.
void W_LevelGraphicsFreed(void)
{
    if (w_comp)
        memset(w_comp, 0, (unsigned)w_numlumps * sizeof(void *));
    for (int i = 0; i < W_ARENA_MAX_CHUNKS; i++)
        { w_arena[i] = 0; w_arena_cap[i] = w_arena_off[i] = 0; }
    w_narena = 0;
    w_protect = 0;   /* was PU_LEVEL — already freed by the Z_FreeTags(PU_LEVEL) above */
}

//
// Shared "empty" lump returned IN PLACE OF an in-gameplay CD read (see W_CacheLumpNum):
// a valid but fully TRANSPARENT patch (width 1, a single 0xFF-terminated column => no
// posts => draws nothing), laid inside a 4 KB zero-filled buffer so a raw-flat consumer
// (R_DrawSpan reads exactly 4096 bytes) reads solid palette-0 instead of running off the
// end. The renderer projects AND draws it consistently (a precache miss is stable for the
// whole level — w_comp[] doesn't change during play), so there is no sprite dimension
// mismatch between projection and draw. A dropped graphic simply does not appear until (if
// ever) it becomes resident — the game never touches the disc mid-level.
// Allocated ONCE from the zone at W_Init (not a .bss array — RAM below the stack is tight).
static byte *s_empty_lump;
static const void *W_EmptyLump(void)
{
    if (!s_empty_lump)
    {
        // Zone-backed, 4 KB zero-filled. Normally built by W_Init; this lazy path is a safe
        // fallback if something requests it first — a single one-time 4 KB PU_STATIC block.
        s_empty_lump = (byte *)Z_Calloc(4096, 1, PU_STATIC, NULL);
        // patch_t: width, height, leftoffset, topoffset (short LE) then columnofs[width]
        // (int LE). width=1 -> one columnofs at byte 8; its column data begins at byte 12.
        s_empty_lump[0] = 1;                 // width  = 1
        s_empty_lump[2] = 1;                 // height = 1
        s_empty_lump[8] = 12;                // columnofs[0] = 12
        s_empty_lump[12] = 0xFF;             // empty column: immediate post terminator
    }
    return s_empty_lump;
}

// W_CacheLumpNum — return a pointer to the lump's DECOMPRESSED data.
//
//  - Whole-run / per-level lumps (PU_STATIC, PU_LEVEL): loaded once from CD and
//    held decompressed for their lifetime (boot / this level). Cache hit after.
//  - Evictable graphics (PU_CACHE): inflated from the RESIDENT COMPRESSED copy
//    (w_comp[]) — a fast RAM lz4_depack, NEVER a CD read. R_PrecacheLevel made
//    every graphic the level uses resident, so gameplay never touches the disc.
//    A missing compressed copy during PLAY (an overflow-dropped low-priority sprite
//    on a heavy map) returns the shared empty lump — the renderer draws nothing that
//    frame rather than freezing on a disc read. It is a precache bug to widen, never
//    a runtime CD read: gameplay is 100% CD-free by construction.
//
const void* W_CacheLumpNum(int lump)
{
    if (lump < 0 || lump >= w_numlumps)
        return NULL;

#ifdef GEN_BOOTPACK_MANIFEST
    // Capture the WHOLE-RUN (PU_STATIC) boot-resident set only. Transient PU_LEVEL
    // lumps touched at the title (TITLEPIC) are deliberately excluded: TITLEPIC is a
    // single ~41 KB read (not a seek storm), and preloading it early then freeing it
    // at the first level load leaves a mid-heap hole that doesn't coalesce into the
    // largest free block — shrinking the contiguous headroom map-pack reserve needs.
    if (w_boot_capturing && w_boot_seen && !w_boot_seen[lump] &&
        lumptag[lump] == PU_STATIC)
    { w_boot_seen[lump] = 1; w_boot_seq[w_boot_nseq++] = lump; }
#endif

    if (lumpcache[lump])
        return lumpcache[lump];                   // decompressed hit

    if (lumptag[lump] == PU_CACHE)
    {
        if (w_comp[lump])
        {
            decode_mem_into_cache(lump, (const byte *)w_comp[lump]);   // RAM -> RAM
        }
        else if (w_in_gameplay)
        {
            // NEVER read the CD mid-level. A precache miss here is an overflow-dropped
            // low-priority graphic on a heavy map; draw nothing this frame (the shared
            // empty lump) instead of blocking on a multi-second disc read (the freeze).
            // Returned straight — NOT cached — so if it ever becomes resident it retries;
            // returning it every frame is a cheap no-op (no allocation, no I/O).
            return W_EmptyLump();
        }
        else
        {
            // LEVEL LOAD only (w_in_gameplay == 0): a texture patch cached to read its
            // width before precache runs, etc. Decompress from the disc into a cache block.
            load_from_cd_into_cache(lump);
        }
    }
    else if (w_stage && lumptag[lump] == PU_STATIC && lump_disc_len(lump) <= SECTOR)
    {
        // Boot: batch the tiny-static run into the shared staging buffer. Guarded on
        // w_stage: it is freed during gameplay (W_BeginGameplay), and preload_static_run
        // reads straight into w_stage — so a PU_STATIC lump first touched AFTER gameplay
        // (e.g. the intermission's WINUM digits / "you are here" marker on level exit)
        // would read into a NULL buffer and hang the CD. Without staging, fall through
        // to load_from_cd_into_cache, which stages the one lump in a transient Z_Malloc.
        preload_static_run(lump);
    }
    else
    {
        load_from_cd_into_cache(lump);            // big static / PU_LEVEL / no-staging: one read
    }
    return lumpcache[lump];
}
