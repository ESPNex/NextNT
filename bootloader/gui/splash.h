/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The startup screen: black, with the NextNT chevrons in the middle
 *
 * The screen is black. In the middle sit the two chevrons, drawn from the same
 * mask as the reference artwork, and the only thing that moves is the gradient
 * inside them: it slides through the strokes while the shape stays put.
 *
 * Pixels are 0x00RRGGBB, one per pixel, rows top to bottom, as everywhere else in
 * this UI. The file is freestanding C++: no allocation, no static constructor, no
 * exception, no RTTI.
 */

#ifndef __BOOTGUI_SPLASH_H__
#define __BOOTGUI_SPLASH_H__

#ifdef __cplusplus
extern "C" {
#endif

/* One frame at sixty frames a second, rounded down. The caller advances its own
   clock: the renderer is given the time and does not look for one. */
#define SPLASH_FRAME_MS 16

/* Rows [Top, Bottom) are painted: black, plus the chevrons when they fall in
   those rows. Rows outside the range are left untouched, which is what lets the
   animation repaint only the upper part of the screen and keep the countdown
   text that lives near the bottom. */
void SplashStartup(unsigned long *Pixels, unsigned long Width, unsigned long Height,
                   unsigned long Top, unsigned long Bottom, unsigned long TimeMs);

#ifdef __cplusplus
}
#endif

#endif /* __BOOTGUI_SPLASH_H__ */
