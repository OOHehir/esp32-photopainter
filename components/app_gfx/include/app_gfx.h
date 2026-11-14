/* app_gfx -- drawing onto a horizontal band of the panel.
 *
 * No I/O of any kind: this component never touches SPI, GPIO or NVS, which is what lets
 * the whole of it, and app_ui above it, run under plain gcc on the build machine.
 *
 * Surfaces are bands so the same code serves a board without PSRAM. The PhotoPainter has
 * 8 MB of PSRAM, so app_display hands over one band covering all 480 rows (384 000 bytes);
 * the band API costs nothing there.
 *
 * Drawing is a pure function of the surface it is given: rendering several bands must
 * produce exactly the bytes that rendering one whole-panel surface would.
 * test_band_seam_matches_whole_frame asserts it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "app_gfx_fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel geometry, landscape. The one place it is written down. */
#define APP_GFX_WIDTH   800
#define APP_GFX_HEIGHT  480

/* The Spectra 6 panel's six inks, in the controller's own numbering.
 * Codes 4 and 7 are unused by the panel; Waveshare's driver for this board uses the
 * same values. */
typedef enum {
    APP_GFX_BLACK  = 0,
    APP_GFX_WHITE  = 1,
    APP_GFX_YELLOW = 2,
    APP_GFX_RED    = 3,
    APP_GFX_BLUE   = 5,
    APP_GFX_GREEN  = 6,
} app_gfx_colour_t;

/* A window onto `rows` rows of the panel, starting at absolute row `y0`.
 *
 * Callers draw in absolute panel coordinates throughout -- a surface subtracts its own
 * y0. That is deliberate: it means app_ui's layout arithmetic never mentions bands, so
 * the same code renders one band or the whole panel and cannot disagree with itself. */
typedef struct {
    uint8_t *pixels;   /* rows * APP_GFX_WIDTH, one colour index per pixel */
    uint16_t y0;       /* absolute row of pixels[0] */
    uint16_t rows;
} app_gfx_surface_t;

/* True when `surface` is well formed: non-NULL pixels, non-zero rows, and entirely
 * within the panel. Every primitive is a no-op on a surface this rejects, so a caller
 * that gets the arithmetic wrong draws nothing rather than corrupting the heap. */
bool app_gfx_surface_valid(const app_gfx_surface_t *surface);

/* Fill the whole surface with one colour. */
void app_gfx_clear(const app_gfx_surface_t *surface, app_gfx_colour_t colour);

/* Set one pixel, in absolute panel coordinates. Outside the surface, does nothing. */
void app_gfx_pixel(const app_gfx_surface_t *surface, int x, int y, app_gfx_colour_t colour);

/* Filled rectangle, absolute coordinates, clipped to the surface. Negative or zero
 * width or height draws nothing. */
void app_gfx_fill_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                       app_gfx_colour_t colour);

/* One-pixel outline of the same rectangle. */
void app_gfx_rect(const app_gfx_surface_t *surface, int x, int y, int w, int h,
                  app_gfx_colour_t colour);

/* Axis-aligned lines. Endpoints are inclusive. */
void app_gfx_hline(const app_gfx_surface_t *surface, int x0, int x1, int y,
                   app_gfx_colour_t colour);
void app_gfx_vline(const app_gfx_surface_t *surface, int x, int y0, int y1,
                   app_gfx_colour_t colour);

/* Bresenham line, any angle. The primitive most likely to expose a band
 * seam. */
void app_gfx_line(const app_gfx_surface_t *surface, int x0, int y0, int x1, int y1,
                  app_gfx_colour_t colour);

/* --- Text ---------------------------------------------------------------
 *
 * `x` is the pen position and `y` is the BASELINE, not the top of the text. Baselines
 * are what makes two different sizes on one row line up.
 *
 * Text clips like every other primitive, so a line crossing a band boundary renders
 * correctly in both bands -- test_text_across_a_seam_matches_whole_frame asserts it.
 *
 * `text` is UTF-8. The fonts carry ASCII, Latin-1 and common punctuation; an unrecognised or
 * malformed sequence is skipped rather than drawn as a replacement box, because a
 * mystery character on a calendar is worse than a missing one.
 */
void app_gfx_text(const app_gfx_surface_t *surface, int x, int baseline,
                  const app_gfx_font_t *font, const char *text, app_gfx_colour_t colour);

/* Width in pixels the same string would occupy. Needed for right-aligning
 * and centering, which is most of the layout. Does not draw, and needs no surface. */
int app_gfx_text_width(const app_gfx_font_t *font, const char *text);

/* Pack a surface into the panel's wire format: two pixels per byte, the left-hand pixel
 * of each pair in the high nibble.
 *
 * `out` must hold rows * APP_GFX_WIDTH / 2 bytes. Returns the number written, or 0 if
 * the surface is invalid. */
size_t app_gfx_pack(const app_gfx_surface_t *surface, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
