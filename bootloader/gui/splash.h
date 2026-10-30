/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Animated gradient background and the NextNT mark
 *
 * The boot menu used to be a flat grey rectangle with a rasterized logo and a
 * rectangular loading bar under it. What is here instead is drawn: a gradient
 * that slides, and two chevrons described by their geometry rather than by a
 * bitmap, so the screen no longer depends on an asset rasterized at one size.
 *
 * The pixels are the same 32-bit values the rest of the UI uses: 0x00RRGGBB,
 * one per pixel, rows top to bottom, no padding between rows.
 *
 * The file is C++ because that is what it was asked for; it is compiled
 * freestanding. There is no allocation, no static constructor, no exception and
 * no template: only functions that take the framebuffer and fill it with
 * integer arithmetic, so the linker never has to look for a C++ runtime.
 */

#ifndef __BOOTGUI_SPLASH_H__
#define __BOOTGUI_SPLASH_H__

#ifdef __cplusplus
extern "C" {
#endif

/* One frame at sixty frames a second, rounded down. The caller advances its own
   clock by this much for each animation step: the renderer is given the time and
   does not go looking for one. */
#define SPLASH_FRAME_MS 16

/* Rows [Top, Bottom) of a Pixels buffer that is Width pixels wide and Height
   rows tall are filled with the gradient of the given moment. Painting the whole
   screen is one call; painting only a strip is what the animation does. */
void SplashFillBackground(unsigned long *Pixels, unsigned long Width,
                          unsigned long Height, unsigned long Top,
                          unsigned long Bottom, unsigned long TimeMs);

/* The NextNT mark -- two chevrons -- centred on (CenterX, CenterY) and Size
   pixels tall, blended over whatever is already there. */
void SplashDrawMark(unsigned long *Pixels, unsigned long Width,
                    unsigned long Height, long CenterX, long CenterY,
                    unsigned long Size, unsigned long TimeMs);

#ifdef __cplusplus
}
#endif

#endif /* __BOOTGUI_SPLASH_H__ */
