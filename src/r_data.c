/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2002 by
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
 *      Preparation of data for rendering,
 *      generation of lookups, caching, retrieval by name.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "w_wad.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_sky.h"
#include "i_system.h"
#include "r_things.h"
#include "p_tick.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "p_tick.h"

#include "global_data.h"

//
// Graphics.
// DOOM graphics for walls and sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
//

//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//

typedef struct
{
  short originx;
  short originy;
  short patch;
  short stepdir;         // unused in Doom but might be used in Phase 2 Boom
  short colormap;        // unused in Doom but might be used in Phase 2 Boom
} PACKEDATTR mappatch_t;


typedef struct
{
  char       name[8];
  char       pad2[4];      // unused
  short      width;
  short      height;
  char       pad[4];       // unused in Doom but might be used in Boom Phase 2
  short      patchcount;
  mappatch_t patches[1];
} PACKEDATTR maptexture_t;

#ifdef PCFX_TEXTURE_PAGES
static int R_PCFLumpNum(char kind, int index)
{
    char name[9] = { 'P', kind, 'T', '0', '0', '0', '0', '0', 0 };
    for (int p = 7; p >= 3; p--)
    {
        name[p] = (char)('0' + index % 10);
        index /= 10;
    }
    return W_CheckNumForName(name);
}
#endif

// A maptexturedef_t describes a rectangular texture, which is composed
// of one or more mappatch_t structures that arrange graphic patches.


static const texture_t* R_LoadTexture(int texture_num)
{
    const byte* pnames = W_CacheLumpName("PNAMES");

    //Skip to list of names.
    pnames += 4;

    const int  *maptex1, *maptex2;
    int  numtextures1, numtextures2;
    const int *directory1, *directory2;


    maptex1 = W_CacheLumpName("TEXTURE1");
    numtextures1 = LONG(*maptex1);
    directory1 = maptex1+1;


    if (W_CheckNumForName("TEXTURE2") != -1)
    {
        maptex2 = W_CacheLumpName("TEXTURE2");
        numtextures2 = LONG(*maptex2);
        directory2 = maptex2+1;
    }
    else
    {
        maptex2 = NULL;
        numtextures2 = 0;
        directory2 = NULL;
    }

    int offset = 0;
    const int  *maptex = maptex1;

    if(texture_num < numtextures1)
    {
        offset = LONG(directory1[texture_num]);
    }
    else if(maptex2 && ((texture_num-numtextures1) < numtextures2) )
    {
        maptex = maptex2;
        offset = LONG(directory2[texture_num-numtextures1]);
    }
    else
    {
        I_Error("R_LoadTexture: Texture %d not in range.", texture_num);
    }

    const maptexture_t *mtexture = (const maptexture_t *) ((const byte *)maptex + offset);

    /* Texture definitions are immutable IWAD metadata. Keep them for the whole
     * run and build all of them contiguously at startup (R_InitTextures), rather
     * than pinning tiny descriptors between purgeable patch blocks in gameplay. */
    texture_t* texture = Z_Malloc(sizeof(const texture_t) + sizeof(const texpatch_t)*(SHORT(mtexture->patchcount)-1), PU_STATIC, (void**)&textures[texture_num]);

    texture->width = SHORT(mtexture->width);
    texture->height = SHORT(mtexture->height);
    texture->patchcount = SHORT(mtexture->patchcount);
    texture->name = mtexture->name;
#ifdef PCFX_TEXTURE_PAGES
    texture->pcfx_lump = R_PCFLumpNum('W', texture_num);
    texture->pcfx_xscale = texture->width > 0
        ? (fixed_t)(((unsigned)32 << FRACBITS) / (unsigned)texture->width) : FRACUNIT;
    texture->pcfx_yscale = texture->height > 0
        ? (fixed_t)(((unsigned)64 << FRACBITS) / (unsigned)texture->height) : FRACUNIT;
#endif

    texpatch_t* patch = texture->patches;
    const mappatch_t* mpatch = mtexture->patches;

    /* Multiple-patch textures use the safe composition path. Determining exact
     * overlap used to decompress each patch just to read its two-byte width,
     * interleaving those cache blocks with the pinned descriptors. */
    texture->overlapped = texture->patchcount > 1;

    for (int j=0 ; j < texture->patchcount ; j++, mpatch++, patch++)
    {
        patch->originx = SHORT(mpatch->originx);
        patch->originy = SHORT(mpatch->originy);

        char pname[9];
        strncpy(pname, (const char*)&pnames[SHORT(mpatch->patch) * 8], 8);
        pname[8] = 0;

        patch->lump = W_GetNumForName(pname);
    }

    int w;

    for (w=1; w*2 <= texture->width; w<<=1)
        ;
    texture->widthmask = w-1;

    textureheight[texture_num] = texture->height<<FRACBITS;

    texturetranslation[texture_num] = texture_num;

    textures[texture_num] = texture;

    return texture;
}

const texture_t* R_GetTexture(int texture)
{
    if(texture >= _g->numtextures)
        return NULL;

    if(textures[texture])
        return textures[texture];

    const texture_t* t = R_LoadTexture(texture);

    textures[texture] = t;

    return t;
}

// ---------------------------------------------------------------------------
// PC-FX: R_PrecacheLevel — pull every graphic the level can show into resident
// RAM at level-load, so the renderer decompresses assets from RAM and NEVER
// reads the CD during gameplay. On-demand CD streaming mid-frame was the cause
// of the in-game freezes: a single PIO CD read blocks for tens of frames, and
// the evictable graphics thrashed the disc. Now the CD is touched only here,
// behind the loading bar. The compressed copies are PU_LEVEL (freed + re-cached
// each level); the renderer's decompressed copies stay evictable but re-inflate
// from RAM — cheap — instead of the disc. (Mirrors vanilla Doom's precache.)
// ---------------------------------------------------------------------------

// Walk a state chain from `st` (following nextstate), precaching each state's
// sprite. `statemark` breaks cycles / avoids re-walking shared states.
static void precache_sprite(int spr, int prio)
{
    if (spr < 0 || spr >= _g->numsprites)
        return;
    const spritedef_t *sd = &_g->sprites[spr];
    for (int f = 0; f < sd->numframes; f++)
    {
        const spriteframe_t *sf = &sd->spriteframes[f];
        const int rots = sf->rotate ? 8 : 1;
        for (int r = 0; r < rots; r++)
            W_PrecacheLumpNum(sf->lump[r] + _g->firstspritelump, prio);
    }
}

static void precache_state_chain(int st, byte *statemark, int prio)
{
    while (st > 0 && st < NUMSTATES && !statemark[st])
    {
        statemark[st] = 1;
        precache_sprite(states[st].sprite, prio);
        st = states[st].nextstate;
    }
}

static void precache_mobjtype(int t, byte *statemark, int prio)
{
    const mobjinfo_t *mi = &mobjinfo[t];
    precache_state_chain(mi->spawnstate,   statemark, prio);
    precache_state_chain(mi->seestate,     statemark, prio);
    precache_state_chain(mi->painstate,    statemark, prio);
    precache_state_chain(mi->meleestate,   statemark, prio);
    precache_state_chain(mi->missilestate, statemark, prio);
    precache_state_chain(mi->deathstate,   statemark, prio);
    precache_state_chain(mi->xdeathstate,  statemark, prio);
    precache_state_chain(mi->raisestate,   statemark, prio);
}

// Projectiles / puffs / fog are spawned at RUNTIME (not present as map things),
// so their sprites must be precached explicitly or they'd CD-miss mid-fight.
// This is the superset an E1 fight can produce (hitscan puff/blood, imp & baron
// fireballs, rockets, teleport fog).
static const short precache_effect_types[] = {
    MT_PUFF, MT_BLOOD, MT_ROCKET, MT_TROOPSHOT, MT_BRUISERSHOT, MT_TFOG, MT_IFOG
};

void R_PrecacheLevel(void)
{
    // Fast path: if this map has a pre-assembled contiguous CD pack, it was already
    // streamed into the arena+lumpcache in one ~0-seek sweep by W_LoadMapPack, called
    // EARLY in P_SetupLevel (before the geometry/sky reads, so those are RAM hits too;
    // texture compositing is deferred to draw time, decompressing from the resident
    // pack). Nothing left to precache. Falls through to the scattered marking path for
    // maps without a pack.
    if (W_MapPackLoaded())
    {
        W_ReportLoadCost();
        return;
    }

    W_PrecacheBegin();

    // --- Flats: those the level's sectors use, expanded over animation ranges so
    // animated flats (NUKAGE/LAVA/...) don't CD-miss when they cycle. Precaching a
    // flat is a single self-contained lump (no texture build), so this is cheap and
    // churn-free. ---
    {
        byte *flatused = (byte *)Z_Malloc(_g->numflats, PU_STATIC, NULL);
        memset(flatused, 0, _g->numflats);
        for (int s = 0; s < _g->numsectors; s++)
        {
            int fp = _g->sectors[s].floorpic, cp = _g->sectors[s].ceilingpic;
            if (fp >= 0 && fp < _g->numflats) flatused[fp] = 1;
            if (cp >= 0 && cp < _g->numflats) flatused[cp] = 1;
        }
        for (const anim_t *a = _g->anims; a < _g->lastanim; a++)
        {
            if (a->istexture) continue;
            int used = 0;
            for (int p = a->basepic; p <= a->picnum && p < _g->numflats; p++)
                if (p >= 0 && flatused[p]) { used = 1; break; }
            if (used)
                for (int p = a->basepic; p <= a->picnum && p < _g->numflats; p++)
                    if (p >= 0) flatused[p] = 1;
        }
        for (int i = 0; i < _g->numflats; i++)
            if (flatused[i])
#ifdef PCFX_TEXTURE_PAGES
                W_PrecacheLumpNum(pcfx_flatlumps[i] >= 0
                                  ? pcfx_flatlumps[i] : _g->firstflat + i,
                                  W_PC_FLAT);
#else
                W_PrecacheLumpNum(_g->firstflat + i, W_PC_FLAT);
#endif
        Z_Free(flatused);
    }

    // --- Wall-texture patches. Expand the sidedef set over animation cycles and
    // both states of every switch. A map only names the initial switch texture;
    // omitting its alternate state made activation CD-load SW2* patches in play. ---
    byte *textureused = (byte *)Z_Calloc(_g->numtextures, 1, PU_STATIC, NULL);
#ifdef PCFX_TEXTURE_PAGES
    byte *textureoriginal = (byte *)Z_Calloc(_g->numtextures, 1, PU_STATIC, NULL);
#endif
    for (int i = 0; i < _g->numsides; i++)
    {
        const side_t *sd = &_g->sides[i];
        int tset[3] = { sd->toptexture, sd->midtexture, sd->bottomtexture };
        for (int k = 0; k < 3; k++)
        {
            int t = tset[k];
            if (t > 0 && t < _g->numtextures)
                textureused[t] = 1;
        }
    }

#ifdef PCFX_TEXTURE_PAGES
    // Two-sided midtextures use Doom's post/mask drawer even when their converted
    // page happens to be fully opaque. Keep the original patches resident for
    // that path; ordinary top/bottom/one-sided walls need only the dense page.
    for (int i = 0; i < _g->numlines; i++)
        if (_g->lines[i].sidenum[1] != NO_INDEX)
            for (int s = 0; s < 2; s++)
            {
                int side = _g->lines[i].sidenum[s];
                if (side != NO_INDEX)
                {
                    int t = _g->sides[side].midtexture;
                    if (t > 0 && t < _g->numtextures)
                        textureoriginal[t] = 1;
                }
            }
#endif

    for (const anim_t *a = _g->anims; a < _g->lastanim; a++)
        if (a->istexture)
        {
            int used = 0;
            for (int t = a->basepic; t <= a->picnum && t < _g->numtextures; t++)
                if (t > 0 && textureused[t]) { used = 1; break; }
            if (used)
                for (int t = a->basepic; t <= a->picnum && t < _g->numtextures; t++)
                    if (t > 0) textureused[t] = 1;
#ifdef PCFX_TEXTURE_PAGES
            int original = 0;
            for (int t = a->basepic; t <= a->picnum && t < _g->numtextures; t++)
                if (t > 0 && textureoriginal[t]) { original = 1; break; }
            if (original)
                for (int t = a->basepic; t <= a->picnum && t < _g->numtextures; t++)
                    if (t > 0) textureoriginal[t] = 1;
#endif
        }

    for (int i = 0; i < _g->numswitches * 2; i += 2)
    {
        int a = _g->switchlist[i], b = _g->switchlist[i + 1];
        if (a > 0 && a < _g->numtextures &&
            b > 0 && b < _g->numtextures && (textureused[a] || textureused[b]))
            textureused[a] = textureused[b] = 1;
#ifdef PCFX_TEXTURE_PAGES
        if (a > 0 && a < _g->numtextures &&
            b > 0 && b < _g->numtextures && (textureoriginal[a] || textureoriginal[b]))
            textureoriginal[a] = textureoriginal[b] = 1;
#endif
    }

    for (int t = 1; t < _g->numtextures; t++)
        if (textureused[t])
        {
            const texture_t *tex = textures[t];
#ifdef PCFX_TEXTURE_PAGES
            if (tex->pcfx_lump >= 0)
                W_PrecacheLumpNum(tex->pcfx_lump, W_PC_PATCH);
            if (tex->pcfx_lump < 0 || textureoriginal[t])
#endif
                for (int p = 0; p < tex->patchcount; p++)
                    W_PrecacheLumpNum(tex->patches[p].lump, W_PC_PATCH);
        }
#ifdef PCFX_TEXTURE_PAGES
    Z_Free(textureoriginal);
#endif
    Z_Free(textureused);
    // The sky is NOT precached: on PC-FX it is the HuC6271 RAINBOW hardware layer
    // (pcfx_sky.bin, baked offline from SKY1), and the software renderer writes sky
    // columns transparent so the RAINBOW shows through — _g->skytexture is never
    // sampled from the arena. Packing SKY1's patch(es) was dead transfer weight.

    // --- Sprites: everything reachable from the state graph of every mobj type
    // present in the level, plus the weapons the level can actually use and the
    // runtime effect/projectile types above. ---
    byte *statemark = (byte *)Z_Malloc(NUMSTATES, PU_STATIC, NULL);
    memset(statemark, 0, NUMSTATES);

    // Which weapons' in-hand (psprite) sprites to precache. Precaching all 9 wastes
    // ~40-60 KB of the pool on weapons the level can never show (E1 shareware has no
    // SSG/plasma/BFG, and most levels have only a subset). Precache a weapon only if
    // the player already OWNS it or a PICKUP for it is present in the map (detected
    // by the pickup mobj's sprite in the thinker walk below). Fist + pistol always.
    boolean weaponwanted[NUMWEAPONS];
    memset(weaponwanted, 0, sizeof(weaponwanted));
    weaponwanted[wp_fist]   = true;
    weaponwanted[wp_pistol] = true;
    for (int w = 0; w < NUMWEAPONS; w++)
        if (_g->player.weaponowned[w])
            weaponwanted[w] = true;

    // Mobjs use ONE OF THREE thinker functions: P_MobjThinker (monsters &
    // projectiles), P_MobjBrainlessThinker (animated items/decorations — health/
    // armor bonuses, blinking pickups) or NULL (fully static decorations, e.g.
    // columns). The old walk only accepted P_MobjThinker and so never precached
    // any MT_MISC* item sprite → those CD-loaded on first draw. Non-mobj thinkers
    // (sector specials) all carry their own T_* function, so these three + the
    // ->type bounds check safely select every mobj.
    for (thinker_t *th = thinkercap.next; th != &thinkercap; th = th->next)
        if (th->function == P_MobjThinker ||
            th->function == P_MobjBrainlessThinker ||
            th->function == NULL)
        {
            int t = ((mobj_t *)th)->type;
            if (t >= 0 && t < NUMMOBJTYPES)
                precache_mobjtype(t, statemark, W_PC_SPRITE);   // things in the map: kept

            // Weapon pickups (identified by sprite, as P_TouchSpecialThing does):
            // mark that weapon so its in-hand art is precached — the player can grab
            // it this level. E1 shareware only ever has SHOT/MGUN/CSAW/LAUN.
            switch (((mobj_t *)th)->sprite)
            {
                case SPR_SHOT: weaponwanted[wp_shotgun]      = true; break;
                case SPR_SGN2: weaponwanted[wp_supershotgun] = true; break;
                case SPR_MGUN: weaponwanted[wp_chaingun]     = true; break;
                case SPR_LAUN: weaponwanted[wp_missile]      = true; break;
                case SPR_PLAS: weaponwanted[wp_plasma]       = true; break;
                case SPR_BFUG: weaponwanted[wp_bfg]          = true; break;
                case SPR_CSAW: weaponwanted[wp_chainsaw]     = true; break;
                default: break;
            }
        }

    // Runtime effects (puff/blood/projectiles/fog) are the arena's overflow victims
    // (W_PC_EFFECT < W_PC_SPRITE): on a pool that can't fully fit (E1M6), a CD miss on
    // a transient effect is the most tolerable — and some (e.g. BRUISERSHOT) never
    // appear in E1 — whereas dropping a visible map item/monster or the in-hand weapon
    // would hitch on something always on screen.
    for (unsigned i = 0; i < sizeof(precache_effect_types)/sizeof(precache_effect_types[0]); i++)
        precache_mobjtype(precache_effect_types[i], statemark, W_PC_EFFECT);

    for (int w = 0; w < NUMWEAPONS; w++)
    {
        if (!weaponwanted[w])
            continue;
        precache_state_chain(weaponinfo[w].upstate,    statemark, W_PC_SPRITE);  // always drawn
        precache_state_chain(weaponinfo[w].downstate,  statemark, W_PC_SPRITE);
        precache_state_chain(weaponinfo[w].readystate, statemark, W_PC_SPRITE);
        precache_state_chain(weaponinfo[w].atkstate,   statemark, W_PC_SPRITE);
        precache_state_chain(weaponinfo[w].flashstate, statemark, W_PC_SPRITE);
    }
    Z_Free(statemark);

    W_PrecacheEnd();
}

static int R_GetTextureNumForName(const char* tex_name)
{
    const int  *maptex1, *maptex2;
    int  numtextures1;
    const int *directory1, *directory2;


    //Convert name to uppercase for comparison.
    char tex_name_upper[9];

    strncpy(tex_name_upper, tex_name, 8);
    tex_name_upper[8] = 0; //Ensure null terminated.

    strupr(tex_name_upper);

    if(_g->tex_lookup_last_name && (!strncmp(_g->tex_lookup_last_name, tex_name_upper, 8)))
    {
        return _g->tex_lookup_last_num;
    }

    maptex1 = W_CacheLumpName("TEXTURE1");
    numtextures1 = LONG(*maptex1);
    directory1 = maptex1+1;


    if (W_CheckNumForName("TEXTURE2") != -1)
    {
        maptex2 = W_CacheLumpName("TEXTURE2");
        directory2 = maptex2+1;
    }
    else
    {
        maptex2 = NULL;
        directory2 = NULL;
    }

    const int *directory = directory1;
    const int *maptex = maptex1;

    for (int i=0 ; i<_g->numtextures ; i++, directory++)
    {
        if (i == numtextures1)
        {
            // Start looking in second texture file.
            maptex = maptex2;
            directory = directory2;
        }

        int offset = LONG(*directory);

        const maptexture_t* mtexture = (const maptexture_t *) ( (const byte *)maptex + offset);

        if(!strncmp(tex_name_upper, mtexture->name, 8))
        {
            _g->tex_lookup_last_name = mtexture->name;
            _g->tex_lookup_last_num = i;
            return i;
        }

    }

    return -1;
}

int R_LoadTextureByName(const char* tex_name)
{
    if(tex_name[0] == '-')
        return NO_TEXTURE;

    int tnum = R_GetTextureNumForName(tex_name);

    if(tnum == -1)
    {
        printf("texture name: %s not found.\n", tex_name);
        return NO_TEXTURE;
    }


    R_GetTexture(tnum);

    return tnum;
}

//
// R_InitTextures
// Initializes the texture list
//  with the textures from the world map.
//

static void R_InitTextures()
{
    const int* mtex1 = W_CacheLumpName("TEXTURE1");
    int numtextures1 = LONG(*mtex1);   // texture count is little-endian in the WAD

    int numtextures2 = 0;

    if (W_CheckNumForName("TEXTURE2") != -1)
    {
        const int* mtex2 = W_CacheLumpName("TEXTURE2");
        numtextures2 = LONG(*mtex2);
    }

    _g->numtextures = numtextures1 + numtextures2;

    textures = Z_Malloc(_g->numtextures*sizeof*textures, PU_STATIC, 0);
    memset(textures, 0, _g->numtextures*sizeof*textures);

    textureheight = Z_Malloc(_g->numtextures*sizeof*textureheight, PU_STATIC, 0);
    memset(textureheight, 0, _g->numtextures*sizeof*textureheight);

    texturetranslation = Z_Malloc((_g->numtextures+1)*sizeof*texturetranslation, PU_STATIC, 0);

    for (int i=0 ; i<_g->numtextures ; i++)
        texturetranslation[i] = i;

    // The complete descriptor set is only about 1.4 KB for doom1.wad. Building
    // it here keeps metadata contiguous and prevents later render-time allocations
    // from splitting the purgeable decompressed-patch cache into small holes.
    for (int i=0 ; i<_g->numtextures ; i++)
        R_LoadTexture(i);
}

//
// R_InitFlats
//
static void R_InitFlats(void)
{
  int i;

  _g->firstflat = W_GetNumForName("F_START") + 1;
  int lastflat  = W_GetNumForName("F_END") - 1;
  _g->numflats  = lastflat - _g->firstflat + 1;

  // Create translation table for global animation.
  // killough 4/9/98: make column offsets 32-bit;
  // clean up malloc-ing to use sizeof

  flattranslation =
    Z_Malloc((_g->numflats+1)*sizeof(*flattranslation), PU_STATIC, 0);
#ifdef PCFX_TEXTURE_PAGES
  pcfx_flatlumps =
    Z_Malloc((_g->numflats+1)*sizeof(*pcfx_flatlumps), PU_STATIC, 0);
#endif

  for (i=0 ; i<_g->numflats ; i++)
  {
    flattranslation[i] = i;
#ifdef PCFX_TEXTURE_PAGES
    pcfx_flatlumps[i] = R_PCFLumpNum('F', i);
#endif
  }
}

//
// R_InitSpriteLumps
// Finds the width and hoffset of all sprites in the wad,
// so the sprite does not need to be cached completely
// just for having the header info ready during rendering.
//
static void R_InitSpriteLumps(void)
{
  _g->firstspritelump = W_GetNumForName("S_START") + 1;
  _g->lastspritelump = W_GetNumForName("S_END") - 1;
  _g->numspritelumps = _g->lastspritelump - _g->firstspritelump + 1;
}

//
// R_InitColormaps
//
// Resident PRE-DOUBLED 16-bit colormap (index | index<<8 per entry) so the render
// inner loops read a ready fat pixel with one ld.h (see lighttable_t in r_defs.h).
// Kept in BSS, NOT the zone: it must not eat the zone budget (E1M6 is razor-tight
// and OOM'd during play once this became a resident PU_STATIC zone block), and it
// must never be purged. Doom's COLORMAP is 34 maps * 256; a modified WAD with more
// is clamped (only the extra light levels would be lost, never a corruption).
#define R_COLORMAP_MAX_ENTRIES (34 * 256)
static lighttable_t s_colormaps[R_COLORMAP_MAX_ENTRIES];

void R_InitColormaps (void)
{
    int lump = W_GetNumForName("COLORMAP");
    const byte *src = (const byte *)W_CacheLumpNum(lump);
    int n = W_LumpLength(lump);   // one 8bpp index per colormap entry
    if (n > R_COLORMAP_MAX_ENTRIES) n = R_COLORMAP_MAX_ENTRIES;
    for (int i = 0; i < n; i++)
        s_colormaps[i] = (lighttable_t)(src[i] | (src[i] << 8));
    colormaps = s_colormaps;   // resident BSS -> costs the zone nothing
}

//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//

void R_InitData(void)
{
  R_InitTextures();
  R_InitFlats();
  R_InitSpriteLumps();
  R_InitColormaps();                    // killough 3/20/98
}

//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
// killough 4/17/98: changed to use ns_flats namespace
//

int R_FlatNumForName(const char *name)    // killough -- const added
{
  int i = W_CheckNumForName(name);

  if (i == -1)
    I_Error("R_FlatNumForName: %.8s not found", name);
  return i - _g->firstflat;
}

//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
// Rewritten by Lee Killough to use hash table for fast lookup. Considerably
// reduces the time needed to start new levels. See w_wad.c for comments on
// the hashing algorithm, which is also used for lump searches.
//
// killough 1/21/98, 1/31/98
//

int PUREFUNC R_CheckTextureNumForName (const char *name)
{
    // "NoTexture" marker.
    if (name[0] == '-')
        return 0;

    return R_GetTextureNumForName(name);
}
