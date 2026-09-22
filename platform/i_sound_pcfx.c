/* i_sound_pcfx.c — NEC PC-FX sound backend for GBADoom.
 *
 * SFX: Doom's DS* lumps are baked offline into a single KING ADPCM bank
 * (tools/gen_pcfx_sfx.py -> src/generated/pcfx_sfx.h), indexed by Doom sfx id.
 * At init the bank is streamed into KRAM hardware page 1 (base KRAM_ADPCM_WORD);
 * I_StartSound programs one of KING's two ADPCM voices to play one sample.
 *
 * Voice allocation: ch1 is reserved for what the player emits (weapon fire, pain
 * and death grunts, pickups) and ch0 carries the world (monsters, doors, lifts).
 * With both sharing one voice, shooting a monster made its scream cut the shotgun
 * blast that caused it. Two voices, split by origin, keeps the pair audible; a
 * new sound still cuts the previous one *within* its own group.
 *
 * Music: Red Book CD-DA tracks (the 3DO Doom soundtrack), driven through
 * libpcfx's CD-DA manager. See the Music section at the bottom of this file.
 *
 * KRAM: page 0 = framebuffer, page 1 = RAINBOW sky + this ADPCM bank. The bank
 * base (0x20000) sits well above the sky stream so they never collide.
 */
#include "i_sound.h"

#include <pcfx/types.h>
#include <pcfx/sound.h>
#include <eris/cd.h>
#include <eris/cdda.h>

#include "pcfx.h"
#include "pcfx_kram.h"
#include "pcfx_time.h"    /* g_ms_irq: the IRQ ms clock voices are timed against */
#include "pcfx_boot.h"    /* pcfx_boot_progress_tick: boot-bar nudge for this read */
#include "pcfx_sfx.h"      /* generated: pcfx_sfx_meta[], counts (bank is on the CD) */
#include "pcfx_sky.h"      /* generated: sector-rounded sky/SFX overlap check */
#include "i_system_e32.h" /* engine prototypes shared with the sound backend */

/* The ADPCM bank is a CD asset (cdlink `append`s pcfx_sfx.bin) so it is NOT baked
 * into the RAM image — that reclaims ~80 KB of .rodata for the zone heap. Its base
 * LBA is emitted by the CD linker into lbas.h (same mechanism as the sky/IWAD); a
 * placeholder keeps code size stable before the first link pass. It's DMA'd
 * straight to KRAM (pcfx_king_dma_cd_to_kram) — no RAM staging. */
#ifdef HAVE_GENERATED_LBAS
#include "lbas.h"
#endif
#ifndef BINARY_LBA_SRC_GENERATED_PCFX_SFX_BIN
#define BINARY_LBA_SRC_GENERATED_PCFX_SFX_BIN 350u
#endif
#define SFX_LBA (BINARY_LBA_SRC_GENERATED_PCFX_SFX_BIN)

/* KING ADPCM playback rate for the bank.  The hardware rate field is two bits,
 * so only 32000/16000/8000/4000 exist.  This MUST match DST_RATE in
 * tools/gen_pcfx_sfx.py: a rate mismatch between the baked bank and the
 * clocked channel makes every sound play back at the wrong speed. */
const int snd_samplerate = 8000;

static int g_adpcm_ready = 0;

/* KING ADPCM register indices (data via king_reg16/32). One CTRL register covers
 * both voices: bit0/bit1 are the ch0/ch1 play-enables and bits[3:2] the rate,
 * which the two voices share (fine -- the whole bank is baked at one rate). */
#define ADPCM_CTRL   0x50u   /* bits[1:0] play-enable (edge start), bits[3:2]=rate */
/* REG.51/REG.52 (C6272_1: "Selects ring or sequential operation ..., ENABLES
 * TRANSFER, and controls half/end event generation"): D0 ring/sequential,
 * D1 transfer enable, D2 half/end event enable.
 *
 * This was 0 -- sequential, and the transfer-enable bit CLEAR. Both retail
 * discs caught mid-playback set D1: Team Innocent holds 0x0002 on both channels
 * (sequential one-shots, exactly our use), Miraculum 0x0007 (ring + enable +
 * event, its streamed-refill model). 0x0002 is the retail value for the way
 * this port uses the engine. */
#define ADPCM_CHCFG(ch) (0x51u + (unsigned)(ch))        /* value: ADPCM_CHCFG_ONESHOT */
#define ADPCM_CHCFG_ONESHOT 0x0002u  /* sequential + transfer enable, no event   */
#define ADPCM_SAL(ch)   (0x58u + ((unsigned)(ch) << 2)) /* start addr / 256 (10b)  */
#define ADPCM_END(ch)   (0x59u + ((unsigned)(ch) << 2)) /* end word addr (18b)     */
#define ADPCM_RATE   ADPCM_RATE_8000   /* == DST_RATE in tools/gen_pcfx_sfx.py */
#define ADPCM_RATEBITS (ADPCM_RATE << 2)

/* The two voices, by what they carry. */
#define HW_WORLD   0
#define HW_PLAYER  1

/* When each voice's sample finishes, on the g_ms_irq clock, and which Doom
 * logical channel last drove it (-1 = idle) so I_StopSound can find it. */
static uint32_t g_hw_end_ms[2];
static int      g_hw_owner[2] = { -1, -1 };

/* Is the voice still decoding? Signed difference, so it survives the ms wrap.
 * The guard makes the answer err towards "free": g_ms_irq is whole milliseconds
 * and its tick is 1.0001 ms, so a booked end time can sit up to ~1 ms behind the
 * hardware's real end. Reporting busy when the voice has in fact finished is the
 * expensive mistake -- see ctrl_keep() -- so give that window away. */
#define HW_BUSY_GUARD_MS 2
static int hw_busy(int ch)
{
    return (int32_t)(g_hw_end_ms[ch] - g_ms_irq) > HW_BUSY_GUARD_MS;
}

/* CTRL bits to write for an operation on `ch`: the rate, plus the *other* voice's
 * enable restated at its current value. Every CTRL write restates both bits, and
 * the enable is edge-triggered -- writing a 1 for a voice the hardware has already
 * finished restarts it from its SAL, replaying the whole sound. So the bit comes
 * from hw_busy(), which models the decode off the ms clock instead of a shadow
 * copy that can't see the hardware's own end-of-sample clear. Modelling it a hair
 * early only clips a millisecond of tail; a hair late would replay a sound. */
static uint16_t ctrl_keep(int ch)
{
    const int other = ch ^ 1;
    return (uint16_t)(ADPCM_RATEBITS | (hw_busy(other) ? (1u << other) : 0u));
}

/* The bank is ONE contiguous run in KRAM page-1 BANK A, words
 * PCFX_SFX_KRAM_BASE_WORD .. 0x1FFFF, anchored at the top and sector-aligned.
 *
 * It was briefly split in two to skip words 0x10000..0x1FFFF, which read back
 * as open bus (0x5555) on the 2026-07-24 burns and turned 44% of the audio into
 * garbage -- the "glitchy PCM". That half is populated; the console was simply
 * in 1-MBIT mode, where the Hudson map leaves it as the dotted expansion half
 * and all page selectors are required to be zero (so this bank was not even on
 * page 1). platform/i_system_pcfx.c king_video_init() now programs 4-Mbit mode
 * before any KRAM access, which makes the whole run real and page 1 distinct.
 *
 * Bank A also satisfies the older constraint that made this a single run in the
 * first place: the KING auto-increment (CD->KRAM DMA and ADPCM playback alike)
 * wraps inside the 17-bit address field with bit 17 -- the -A/B bank select --
 * held, and the run never crosses it. C6272_1 1.3.
 *
 * Negative-array idiom, not _Static_assert: gnu99 with GCC 4.9.4. */
typedef char pcfx_sfx_bank_fits_kram_page1[
    (PCFX_SFX_KRAM_BASE_WORD + ((PCFX_SFX_BANK_BYTES + 2047u) & ~2047u) / 2u
         <= PCFX_SFX_BANK_END_WORD) ? 1 : -1];
/* Both CD loads round up to whole sectors; compare extents, not just bases. */
typedef char pcfx_sky_fits_below_adpcm[
    (KRAM_RAINBOW_WORD + ((PCFX_SKY_BYTES + 2047u) & ~2047u) / 2u
         <= PCFX_SFX_KRAM_BASE_WORD) ? 1 : -1];

/* Programming a voice is a RUN of KING 0x600/0x604 register-select/data pairs,
 * and by the rule in pcfx.h every such run must be interrupt-atomic: a firing
 * ~1 ms interval-timer IRQ that lands between a select and its data corrupts
 * the access on silicon (it does not on pcfxemu, so none of this reproduces
 * there). These runs were unguarded, which put a ~1 ms window over eight
 * register writes on EVERY sound trigger.
 *
 * A corrupted write here is not a dropped sound -- it is a voice pointed at the
 * wrong KRAM word, so it decodes whatever is next in the bank as ADPCM: a burst
 * of the wrong sound, or noise, at the right moment. The damage scales with how
 * often a sound retriggers, which is why the shotgun/pistol -- the most-fired
 * sounds in the game -- were where it was heard. */
/* Hold an asserted ADPCM software reset until the hardware has actually taken it.
 *
 * C6230 3.2: "R10 can reset ADPCM channels 1 and 2 independently. THE SETTING
 * TAKES EFFECT ON THE FALLING EDGE OF THE NEXT -HSYNC." The reset is not an
 * action, it is a level sampled once per horizontal period (~63.6 us).
 *
 * Every reset in this file used to be an assert immediately followed by a
 * release -- two I/O writes microseconds apart, inside one horizontal period.
 * The edge that samples R10 therefore only ever saw the RELEASED value and the
 * decompressor was never reset at all, so every new sound inherited the
 * previous sound's predictor P(N) and scale S(N) instead of the documented
 * 200H/16/0 start state (3.2; 4.3: "either reset the channel first or ensure
 * that the new stream is a true continuation of the old one").
 *
 * That is exactly why the two voices behaved differently on silicon. The PLAYER
 * voice replays the same few weapon samples, so the state it inherits is the
 * state that same sample ends on -- a small error, and the gunfire sounded
 * right. The WORLD voice cycles through unrelated monster sight/pain/DEATH
 * samples back to back, each one starting from the tail state of a different
 * sound: wrong DC level, wrong step size, and 4.1's `dt' = (A(N)+1)*S(N-1)/8`
 * carries that wrong scale forward through the whole sample.
 *
 * The Tetsu raster counter steps once per horizontal period, so one observed
 * change is one -HSYNC. Wait for TWO, so the assert cannot lose a race against
 * the edge it was written next to. Interrupts stay ENABLED here: the voice is
 * muted and its buffer disabled for the duration, which is the state we want it
 * in anyway, and the ~130 us must not be spent with the ms clock stopped. The
 * spin is bounded so a stalled raster can never wedge a sound trigger. */
static void adpcm_reset_settle(void)
{
    unsigned prev  = pcfx_tetsu_raster_stable();
    unsigned edges = 0;

    for (unsigned spins = 0; spins < 4000u; spins++)
    {
        unsigned r = pcfx_tetsu_raster_stable();
        if (r == prev) continue;
        prev = r;
        if (++edges >= 2u) return;
    }
}

static void adpcm_reset(void)
{
    uint32_t psw = pcfx_irq_save();

    adpcm_set_volume(0, 0, 0);
    adpcm_set_volume(1, 0, 0);
    adpcm_set_control(ADPCM_RATE, 1, 1, 1, 1);   /* assert BOTH resets          */
    king_reg16(ADPCM_CTRL, 0);
    for (int ch = 0; ch < 2; ch++)
    {
        king_reg16(ADPCM_CHCFG(ch), ADPCM_CHCFG_ONESHOT);
        king_reg16(ADPCM_SAL(ch), 0);
        king_reg32(ADPCM_END(ch), 0);
    }

    pcfx_irq_restore(psw);

    /* A SIMULTANEOUS two-channel reset is also the only thing that re-establishes
     * the HuC6272->HuC6230 transfer cycle (C6230 4.2 rules 2 and 6), and it is
     * worth nothing unless it survives an -HSYNC. */
    adpcm_reset_settle();

    psw = pcfx_irq_save();
    adpcm_set_control(ADPCM_RATE, 1, 1, 0, 0);   /* release both                */
    pcfx_irq_restore(psw);

    for (int ch = 0; ch < 2; ch++)
    {
        g_hw_end_ms[ch] = g_ms_irq;
        g_hw_owner[ch]  = -1;
    }
}

/* Read the ADPCM bank from the CD in small chunks and stream each into KRAM page 1
 * (16-bit words). This runs at I_InitSound — BEFORE Z_Init — so it can't use the
 * zone; a tiny static sector-staging buffer avoids both the zone and keeping the
 * ~80 KB bank resident in main RAM. The CD (and KRAM) are already up by here
 * (I_PreInitGraphics ran first). */
static void adpcm_load_bank(void)
{
    if (PCFX_SFX_BANK_BYTES == 0) { g_adpcm_ready = 0; return; }
    /* libpcfx's read now retries with a full SCSI bus reset between attempts
     * (the recovery probe 027 proved works on marginal CD-R hardware), so this
     * effectively always succeeds.  As a last-resort backstop, if it still
     * fails, boot WITHOUT sound rather than fatal-blinking the console: a
     * silent game beats a bricked one, and every later I_StartSound already
     * bails on !g_adpcm_ready. */
    if (!pcfx_king_dma_cd_to_kram(SFX_LBA, PCFX_SFX_KRAM_BASE_WORD,
                                  PCFX_SFX_BANK_BYTES)) {
        g_adpcm_ready = 0;
        return;
    }
    /* This is the very first CD read of the whole program (I_InitSound runs
     * before Z_Init/D_DoomMainSetup, so the boot progress bar has no per-read
     * count for it) — nudge it once here so a clean, no-retry load still shows
     * visible movement instead of a dead pause before W_Init's own ticks start. */
    pcfx_boot_progress_tick();
    g_adpcm_ready = 1;
}

void I_InitSound(void)
{
    /* This is the first SCSI/CD command the whole program issues, right after
     * the BIOS hands off — normalize whatever bus/phase state the BIOS's own
     * boot-loader read left behind rather than trusting it matches the idle
     * state libpcfx's retry logic assumes. */
    eris_cd_reset();
    adpcm_reset();
    adpcm_load_bank();
    /* After the bank load: that DMA is a CD read, and this leaves the CD-DA
     * manager's read accounting in sync with the drive's actual state. */
    eris_cdda_music_init();
}

/* Returns the channel (stored by s_sound.c as the handle), or -1 if not started.
 * `is_player` picks the voice: set for sounds the player emits or that have no
 * world origin at all (menu, intermission), clear for everything else. */
int I_StartSound(int id, int channel, int vol, int sep, int is_player)
{
    (void)sep;
    if (!g_adpcm_ready) return -1;
    if (id < 0 || (unsigned)id >= PCFX_SFX_COUNT) return -1;

    const pcfx_sfx_meta_t *m = &pcfx_sfx_meta[id];
    if (m->word_count == 0) return -1;

    const int ch = is_player ? HW_PLAYER : HW_WORLD;

    uint32_t start = m->start_word;   /* absolute KRAM word (two segments) */
    uint32_t end   = start + m->word_count - 1u;

    /* Doom never actually sends 127: the loudest sound is snd_SfxVolume * 8 with
     * snd_SfxVolume capped at 15, so the real top of the range is 120 (see
     * S_StartSoundAtVolume / S_AdjustSoundParams). The old `vol >> 1` treated the
     * range as 0..127 and so peaked at 60 of KING's 63. That is not the 5% it
     * looks like: this register is logarithmic at ~1.5 dB per step, so those three
     * unused steps threw away 4.5 dB of SFX level against the CD-DA music.
     * Scaling by the range Doom really produces puts a full-volume sound at the
     * register maximum. */
    unsigned v = ((unsigned)vol * 63u) / 120u;
    if (v > 63) v = 63;
    if (v == 0) v = m->volume ? m->volume : 32u;

    /* A trigger is three phases, because the decoder reset in the middle is
     * sampled by the hardware once per -HSYNC (see adpcm_reset_settle()).
     * Phases 1 and 3 are each one interrupt-atomic KING register run: a timer
     * IRQ landing between a 0x600 select and its 0x604 data corrupts the access
     * on silicon (pcfx.h), and a corrupted write here is not a dropped sound but
     * a voice pointed at the wrong KRAM word, decoding whatever is next in the
     * bank. */

    /* Phase 1: mute this voice, stop its buffer, ASSERT its decoder reset.
     * The mute goes first per C6230 4.3 -- "software should first reduce volume
     * to -inf and then reset", or the DC step out of the previous sound's held
     * level clicks. The other voice's reset bit stays 0 throughout, so it plays
     * on untouched; ctrl_keep() restates its enable at its current value. */
    {
        uint32_t psw = pcfx_irq_save();

        adpcm_set_volume((uint8_t)ch, 0, 0);
        king_reg16(ADPCM_CTRL, ctrl_keep(ch));    /* this voice's bit low: arm  */
        adpcm_set_control(ADPCM_RATE, 1, 1, ch == 0, ch == 1);

        pcfx_irq_restore(psw);
    }

    /* Phase 2: let a real -HSYNC latch it. Interrupts on; the voice is silent. */
    adpcm_reset_settle();

    /* Phase 3: release the reset, point the buffer at the sample, restore the
     * volume, and give the enable its 0->1 edge. ctrl_keep() is re-sampled here
     * rather than reused from phase 1 so the other voice's enable reflects the
     * ~130 us that just passed. */
    {
        uint16_t keep = ctrl_keep(ch);
        uint32_t psw  = pcfx_irq_save();

        adpcm_set_control(ADPCM_RATE, 1, 1, 0, 0);
        king_reg16(ADPCM_CHCFG(ch), ADPCM_CHCFG_ONESHOT);
        king_reg16(ADPCM_SAL(ch), (uint16_t)(start >> 8));
        king_reg32(ADPCM_END(ch), end);
        adpcm_set_volume((uint8_t)ch, (uint8_t)v, (uint8_t)v);
        king_reg16(ADPCM_CTRL, (uint16_t)(keep | (1u << ch))); /* 0->1 starts it */

        pcfx_irq_restore(psw);
    }

    /* Four ADPCM nibbles decode per KRAM word, so the sample runs
     * word_count*4 samples at snd_samplerate Hz. Peaks at 512*4000 with the
     * 512-word cap, well inside 32 bits. */
    g_hw_end_ms[ch] = g_ms_irq +
        ((uint32_t)m->word_count * 4000u) / (uint32_t)snd_samplerate;
    g_hw_owner[ch]  = channel;
    return channel;
}

/* Doom stopped a logical channel without starting a replacement. Silence the
 * voice it was driving, if it still owns one. */
void I_StopSound(int channel)
{
    for (int ch = 0; ch < 2; ch++)
    {
        if (g_hw_owner[ch] != channel) continue;

        g_hw_end_ms[ch] = g_ms_irq;    /* idle before restating the enables */
        g_hw_owner[ch]  = -1;

        {   /* interrupt-atomic KING run -- see adpcm_reset() */
            uint32_t psw = pcfx_irq_save();
            adpcm_set_volume((uint8_t)ch, 0, 0);
            king_reg16(ADPCM_CTRL, ctrl_keep(ch));   /* drops this voice's bit */
            pcfx_irq_restore(psw);
        }
    }
}

/* ----------------------------------------------------------------- Music --- */
/* Red Book CD-DA, played through libpcfx's CD-DA music manager (eris/cdda.h).
 * The two ADPCM voices are spent on SFX and there is no RAM budget for a MUS
 * player, so the score comes off the disc as audio tracks.
 *
 * The soundtrack and its order are 3DO Doom's, as implemented by optidoom3do
 * (source/sound.c SongLookup[] + lib/burger/music.c, which plays "Music/SongN"
 * for song number N). Two indirections get us from a Doom music id to a track:
 *
 *   mus_* id  --doom_song_3do[]-->  3DO song number  --song_track[]-->  CD track
 *
 * The second step exists because cdlink numbers tracks by WAV filename order
 * (Song1, Song10, Song11, ... Song9 -> tracks 2..15), which is not the musical
 * order. It emits a CDDA_TRACK_SONGnn define per stem, so we look up by name and
 * the on-disc ordering stays irrelevant.
 *
 * A CD data read stops the drive's audio engine, so every read site calls
 * eris_cdda_notify_cd_read() and the manager restarts playback from the pump
 * (I_StartFrame). Without that, music would die at the first level load. */
#include "sounds.h"

/* Generated by pcfx-cdlink (`cddaheader` in cdlink.txt) on the first link pass;
 * the second pass compiles against it. Track 0 means "no such track", which the
 * manager treats as silence — that is what the pass-1 build plays. */
#ifdef HAVE_CDDA_TRACKS
#include "cdda_tracks.h"
#endif
#ifndef CDDA_TRACK_SONG1
#define CDDA_TRACK_SONG1  0
#endif
#ifndef CDDA_TRACK_SONG3
#define CDDA_TRACK_SONG3  0
#endif
#ifndef CDDA_TRACK_SONG5
#define CDDA_TRACK_SONG5  0
#endif
#ifndef CDDA_TRACK_SONG6
#define CDDA_TRACK_SONG6  0
#endif
#ifndef CDDA_TRACK_SONG7
#define CDDA_TRACK_SONG7  0
#endif
#ifndef CDDA_TRACK_SONG8
#define CDDA_TRACK_SONG8  0
#endif
#ifndef CDDA_TRACK_SONG9
#define CDDA_TRACK_SONG9  0
#endif
#ifndef CDDA_TRACK_SONG10
#define CDDA_TRACK_SONG10 0
#endif
#ifndef CDDA_TRACK_SONG11
#define CDDA_TRACK_SONG11 0
#endif
#ifndef CDDA_TRACK_SONG12
#define CDDA_TRACK_SONG12 0
#endif
#ifndef CDDA_TRACK_SONG13
#define CDDA_TRACK_SONG13 0
#endif
#ifndef CDDA_TRACK_SONG14
#define CDDA_TRACK_SONG14 0
#endif
#ifndef CDDA_TRACK_SONG15
#define CDDA_TRACK_SONG15 0
#endif
#ifndef CDDA_TRACK_SONG29
#define CDDA_TRACK_SONG29 0
#endif

/* 3DO song number -> CD track. Indices with no song (2, 4, 16..28) stay 0: the
 * 3DO disc has no Song2/Song4 and its SongLookup never asks for them. */
#define PCFX_MAX_3DO_SONG 29
static const uint8_t song_track[PCFX_MAX_3DO_SONG + 1] = {
    [1]  = CDDA_TRACK_SONG1,
    [3]  = CDDA_TRACK_SONG3,
    [5]  = CDDA_TRACK_SONG5,
    [6]  = CDDA_TRACK_SONG6,
    [7]  = CDDA_TRACK_SONG7,
    [8]  = CDDA_TRACK_SONG8,
    [9]  = CDDA_TRACK_SONG9,
    [10] = CDDA_TRACK_SONG10,
    [11] = CDDA_TRACK_SONG11,
    [12] = CDDA_TRACK_SONG12,
    [13] = CDDA_TRACK_SONG13,
    [14] = CDDA_TRACK_SONG14,
    [15] = CDDA_TRACK_SONG15,
    [29] = CDDA_TRACK_SONG29,
};

/* Doom music id -> 3DO song number.
 *
 * Episode 1 is exact: optidoom3do plays SongLookup[Song_e1m1 - 1 + gamemap] for
 * map N, i.e. songs 5..13 for maps 1..9, and this port ships episode 1. The
 * non-level cues are its SongLookup entries too: intro 11, final 12, bunny 3,
 * intermission 5.
 *
 * Episodes 2 and 3 are unreachable here (E1-only IWAD) but are mapped anyway so
 * a bad index can never read past the table: they continue through the same
 * SongLookup rows positionally (3DO maps 10..24), which is the closest thing to
 * "the 3DO order" for levels the 3DO release ordered differently. Entries left 0
 * are silent. */
static const uint8_t doom_song_3do[NUMMUSIC] = {
    [mus_None]   = 0,

    [mus_e1m1]   = 5,  [mus_e1m2]  = 6,  [mus_e1m3]  = 7,
    [mus_e1m4]   = 8,  [mus_e1m5]  = 9,  [mus_e1m6]  = 10,
    [mus_e1m7]   = 11, [mus_e1m8]  = 12, [mus_e1m9]  = 13,

    [mus_e2m1]   = 14, [mus_e2m2]  = 15, [mus_e2m3]  = 5,
    [mus_e2m4]   = 6,  [mus_e2m5]  = 7,  [mus_e2m6]  = 8,
    [mus_e2m7]   = 9,  [mus_e2m8]  = 10, [mus_e2m9]  = 11,

    [mus_e3m1]   = 13, [mus_e3m2]  = 14, [mus_e3m3]  = 15,
    [mus_e3m4]   = 10, [mus_e3m5]  = 12, [mus_e3m6]  = 29,
    [mus_e3m7]   = 1,  [mus_e3m8]  = 1,  [mus_e3m9]  = 1,

    [mus_inter]  = 5,   /* SongLookup[Song_intermission] */
    [mus_intro]  = 11,  /* SongLookup[Song_intro]        */
    [mus_bunny]  = 3,   /* SongLookup[Song_bunny]        */
    [mus_victor] = 12,  /* SongLookup[Song_final]        */
    [mus_introa] = 11,  /* alternate title cue; 3DO has one intro */
    [mus_read_m] = 11,  /* no 3DO equivalent; reuse the title theme */
};

static uint8_t music_track_for(int handle)
{
    uint8_t song;

    if (handle <= mus_None || handle >= NUMMUSIC)
        return CDDA_TRACK_NONE;

    song = doom_song_3do[handle];
    if (song == 0 || song > PCFX_MAX_3DO_SONG)
        return CDDA_TRACK_NONE;

    return song_track[song];
}

/* Called once per frame from I_StartFrame: (re)issues CD-DA once loads settle. */
void pcfx_cdda_pump(void)
{
    eris_cdda_music_pump();
}

/* Silence CD-DA ahead of a burst of CD->KRAM transfers, and keep it silent.
 *
 * A playing CD-DA track means the drive is actively streaming audio, so a SCSI
 * data transfer issued underneath it makes the drive abort the audio and seek
 * mid-stream — one drive, two jobs. Callers use this when they are about to
 * lean on the drive and would rather have it to themselves (the intermission's
 * accelerate keypress, which fires two full-screen background DMAs and then a
 * whole map-pack load).
 *
 * There is deliberately no unmute: this latches the player's pause, and the
 * next eris_cdda_music_play() — i.e. the next S_ChangeMusic, which every level
 * start issues — clears it. So the music comes back on its own at the next
 * place it would have changed anyway, and no caller has to pair this with
 * anything or risk leaving the score off. */
void pcfx_cdda_mute_for_dma(void)
{
    eris_cdda_music_pause();
}

void pcfx_cdda_play_track(int track)
{
    eris_cdda_music_play((uint8_t)track, 1);
}

void pcfx_cdda_stop(void)
{
    eris_cdda_music_stop();
}

/* Doom's music volume is 0..15; KING's CD-DA level is 0..63. */
void I_SetMusicVolume(int volume)
{
    if (volume < 0)  volume = 0;
    if (volume > 15) volume = 15;
    eris_cdda_music_set_volume((uint8_t)((volume * CDDA_VOLUME_MAX) / 15));
}

void I_PauseSong(int handle)
{
    (void)handle;
    eris_cdda_music_pause();
}

void I_ResumeSong(int handle)
{
    (void)handle;
    eris_cdda_music_resume();
}

void I_PlaySong(int handle, int looping)
{
    eris_cdda_music_play(music_track_for(handle), looping);
}

void I_StopSong(int handle)
{
    (void)handle;
    eris_cdda_music_stop();
}
