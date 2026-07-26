/* pcfx_devsel.c — pre-title save-device chooser (internal BackupRAM vs FX-BMP).
 *
 * Runs during D_DoomMainSetup, before G_LoadSettings and the title. It is drawn
 * with the same DOOM red hu_font the in-game menus use, on the HuC6270 VDC BG
 * text layer (platform/pcfx_text.c). That layer is already initialised in main()
 * (I_PreInitGraphics -> I_InitScreen_e32 -> pcfx_text_init), and bg7up is on, so
 * the font is available and displayed here — no separate boot-only font is needed.
 *
 * Before drawing, PLAYPAL is uploaded (only the two boot-bar grays exist until
 * now, so the font would otherwise be colourless) and the KING framebuffer page 0
 * is cleared, so the boot loading bar is not left showing underneath the chooser.
 *
 * The screen only appears when an FX-BMP card actually answers the probe
 * (PCFX_SaveDeviceAvailable(1)); with no card the choice is forced to internal
 * BackupRAM with no boot-flow interruption, so the common case is unchanged.
 *
 * And when it DOES appear it can no longer stop the boot: it counts down and takes
 * internal BackupRAM on its own unless the player touches UP/DOWN, which cancels
 * the countdown and waits for RUN/I. Two reasons. The probe answers "card present"
 * on a machine with no card (pcfx-headless does, and a real console did too), and
 * an unconditional wait then holds boot at ~99% for a prompt the player never asked
 * for. Worse, it did that INVISIBLY: a pre-title palette upload is only STAGED, and
 * nothing flushes it to the VCE before the title, so this screen's red font drew
 * black on black and looked exactly like a boot hang (fixed below with
 * pcfx_text_palette_now, but a screen that gates boot on input must not be able to
 * fail this way twice).
 *
 * Frame pacing uses the stable tetsu raster read (pcfx.h): FXVCE_STATUS' vblank
 * bit is NOT safe to poll (it latches once the weapon VDCs come up — see
 * pcfx_support.c), and a single raw raster read can return a bogus value.
 */
#include <pcfx/contrlr.h>

#include "pcfx.h"
#include "pcfx_kram.h"
#include "pcfx_boot.h"
#include "pcfx_save.h"
#include "pcfx_text.h"
#include "v_video.h"        /* V_SetPalette: upload PLAYPAL so the font shows red */

/* Active display is lines 0..239 in the 262-line mode; vblank is raster >= 240. */
#define PCFX_VBLANK_RASTER 240

/* Wait one full frame: leave vblank, then re-enter it. */
static void wait_frame(void)
{
    while (pcfx_tetsu_raster_stable() >= PCFX_VBLANK_RASTER) { }
    while (pcfx_tetsu_raster_stable() <  PCFX_VBLANK_RASTER) { }
}

/* Black out the visible KING framebuffer page 0 (the page shown throughout boot),
 * removing the boot loading bar so the chooser sits on a clean background. */
static void fb_clear_page0(void)
{
    /* Interrupt-atomic sliced fill: an unsliced king_kram_fill runs long enough
     * for a timer IRQ to move the KRAM cursor out from under it, leaving stale
     * bands of the boot bar behind on real hardware. See pcfx_kram.h. */
    pcfx_kram_fill_guarded(pcfx_fb_page_base(0), 0, 240u * KFB_ROW_WORDS);
}

/* Text layout on the 32x30 cell grid (8x8 cells). Rows chosen to sit centred. */
#define ROW_TITLE 10
#define ROW_OPT0  13
#define ROW_OPT1  15
#define ROW_HINT  19
#define OPT_COL    8              /* left edge of both option rows (incl. cursor) */

/* Column that centres a `len`-char string on the 32-cell width. */
static int center_col(int len) { return (PCFX_TEXT_COLS - len) / 2; }

/* `secs` >= 0 shows the countdown to the automatic internal-memory default;
 * < 0 means the player took the wheel and we now wait for RUN/I. */
static void build_screen(int sel, int secs)
{
    pcfx_text_begin();
    pcfx_text_puts(center_col(11), ROW_TITLE, "SAVE DEVICE");
    pcfx_text_puts(OPT_COL, ROW_OPT0, sel == 0 ? "> INTERNAL MEMORY" : "  INTERNAL MEMORY");
    pcfx_text_puts(OPT_COL, ROW_OPT1, sel == 1 ? "> FX-BMP CARD"      : "  FX-BMP CARD");
    if (secs < 0)
        pcfx_text_puts(center_col(16), ROW_HINT, "UP/DOWN   RUN:OK");
    else
    {
        char hint[17] = "INTERNAL IN 0...";
        hint[12] = (char)('0' + (secs > 9 ? 9 : secs));
        pcfx_text_puts(center_col(16), ROW_HINT, hint);
    }
}

/* Frames the screen waits before defaulting to internal BackupRAM. ~4 s at 60 Hz:
 * long enough to read the two options and reach for the pad, short enough that an
 * unattended boot (or one on a machine whose probe cries card) is not held up. */
#define DEVSEL_AUTO_FRAMES 240

void PCFX_SaveDeviceSelect(void)
{
    int sel = 0;
    int chosen = 0;                       /* UP/DOWN pressed: wait for RUN/I, no timeout */
    int frames;
    uint32_t prev;

    PCFX_SaveSetDevice(0);
    if (PCFX_SaveInit() < 0)
        return;                          /* saves are disabled anyway     */
    if (!PCFX_SaveDeviceAvailable(1))
        return;                          /* no card: internal, no screen  */

    /* PLAYPAL -> VCE so the red DOOM font renders in colour, then wipe the boot
     * loading bar off the framebuffer so the chooser is on a clean page.
     * V_SetPalette only STAGES the upload; pcfx_text_palette_now is what actually
     * reaches the VCE before the title exists (see pcfx_text.h). */
    V_SetPalette(0);
    pcfx_text_palette_now();
    fb_clear_page0();

    prev = contrlr_pad_read(0) & 0x0FFFu; /* swallow held-at-boot buttons */
    for (frames = 0; ; frames++)
    {
        uint32_t held, down;
        int left = (DEVSEL_AUTO_FRAMES - frames + 59) / 60;   /* whole seconds left */

        build_screen(sel, chosen ? -1 : (left > 0 ? left : 0));
        wait_frame();                     /* flush the tilemap during vblank */
        pcfx_text_present();

        held = contrlr_pad_read(0) & 0x0FFFu;
        down = held & ~prev;
        prev = held;

        if (down & (JOY_UP | JOY_DOWN))
        {
            sel ^= 1;
            chosen = 1;                   /* player is choosing: stop the countdown */
        }
        if (down & (JOY_RUN | JOY_I))
            break;
        if (!chosen && frames >= DEVSEL_AUTO_FRAMES)
            break;                        /* nobody wants to choose: internal, carry on */
    }

    PCFX_SaveSetDevice(sel);

    /* Clear the chooser text and restart the boot bar's frame so the remaining
     * pre-title streaming (I_InitGraphics, W_PreloadStatics) shows progress on the
     * now-black page. */
    pcfx_text_begin();
    pcfx_text_present();
    pcfx_boot_progress_begin();
}
