/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The startup screen: black, with the NextNT chevrons in the middle
 *
 * The shape is not described in code and not simplified: it comes from
 * assets/nextnt-mark.alpha, the coverage mask taken from the reference artwork
 * (142 x 122, one byte per pixel, brightest channel of the drawing), which
 * native-bin2c turns into bootui_mark. What is described in code is the colour:
 * a gradient that runs through the strokes and slides with the clock.
 *
 * Scaling is nearest neighbour on purpose. The mask is a hard-edged drawing, the
 * screen is only ever a couple of hundred pixels larger or smaller than it, and
 * a filtered scale of a two-colour drawing softens exactly the edges that give
 * the mark its shape.
 *
 * Freestanding C++: no headers beyond the two local ones, no allocation, no
 * static objects with constructors, no exceptions, no RTTI.
 */

#include "splash.h"
#include "bootui_mark.h"

namespace
{

/* The mask's own size. It is fixed by the artwork, and native-bin2c carries the
   bytes without dimensions, so they are written down here once. */
const unsigned long MaskWidth = 142;
const unsigned long MaskHeight = 122;

/* The mark occupies a third of the screen's height, but never more than the
   screen can hold with room around it. */
unsigned long
MarkHeight(unsigned long Width, unsigned long Height)
{
    unsigned long ByHeight = Height / 3;
    unsigned long ByWidth = (Width / 2) * MaskHeight / MaskWidth;

    if (ByHeight > ByWidth)
        ByHeight = ByWidth;
    if (ByHeight < 24)
        ByHeight = 24;
    if (ByHeight > MaskHeight * 6)
        ByHeight = MaskHeight * 6;
    return ByHeight;
}

inline unsigned long
Channel(unsigned long Color, unsigned int Shift)
{
    return (Color >> Shift) & 0xFF;
}

inline unsigned long
Pack(unsigned long Red, unsigned long Green, unsigned long Blue)
{
    if (Red > 255) Red = 255;
    if (Green > 255) Green = 255;
    if (Blue > 255) Blue = 255;
    return (Red << 16) | (Green << 8) | Blue;
}

/* The gradient inside the mark: two blues with a bright band that travels
   diagonally across the strokes, one full crossing every three seconds. The
   position argument is 0..255 within the mark, the phase is 0..255 per cycle. */
unsigned long
StrokeColor(unsigned long Position, unsigned long Phase, unsigned long Depth)
{
    unsigned long Along = (Position + Phase) & 0xFF;
    unsigned long Light = Along < 128 ? Along * 2 : (255 - Along) * 2;
    unsigned long Red, Green, Blue;

    /* Deeper strokes are darker: the same band reads differently on the two
       chevrons, which is what gives the mark its two-tone look. */
    /* Squared light: the bright part of the band stays narrow, the rest of the
       stroke keeps the deep blue of the artwork. */
    Light = (Light * Light) / 255;

    Red = 10 + (Light * (Depth ? 52 : 34)) / 255;
    Green = 62 + (Light * (Depth ? 150 : 122)) / 255;
    Blue = 158 + (Light * (Depth ? 96 : 78)) / 255;
    return Pack(Red, Green, Blue);
}

} /* namespace */

extern "C" void
SplashStartup(unsigned long *Pixels, unsigned long Width, unsigned long Height,
              unsigned long Top, unsigned long Bottom, unsigned long TimeMs)
{
    unsigned long Y, X, MarkSize, ScaleX, ScaleY, MarkLeft, MarkTop, Phase;

    if (!Pixels || !Width || !Height)
        return;

    if (Bottom > Height)
        Bottom = Height;

    /* The screen is black, always. */
    for (Y = Top; Y < Bottom; ++Y)
    {
        unsigned long *Row = Pixels + Y * Width;
        for (X = 0; X < Width; ++X)
            Row[X] = 0;
    }

    MarkSize = MarkHeight(Width, Height);
    ScaleX = (MaskWidth << 8) / MarkSize;
    ScaleY = (MaskHeight << 8) / MarkSize;
    MarkLeft = (Width - MarkSize) / 2;
    MarkTop = (Height - (MarkSize * MaskHeight / MaskWidth)) / 2;

    /* A full crossing of the band every three seconds, in 0..255 units. */
    Phase = (unsigned long)((TimeMs * 256ull) / 3000ull) & 0xFF;

    for (Y = 0; Y < MarkSize * MaskHeight / MaskWidth; ++Y)
    {
        unsigned long ScreenY = MarkTop + Y;
        unsigned long MaskY, *Row;

        if (ScreenY < Top || ScreenY >= Bottom)
            continue;

        MaskY = (Y * ScaleY) >> 8;
        if (MaskY >= MaskHeight)
            MaskY = MaskHeight - 1;

        Row = Pixels + ScreenY * Width;
        for (X = 0; X < MarkSize; ++X)
        {
            unsigned long ScreenX = MarkLeft + X;
            unsigned long MaskX = (X * ScaleX) >> 8;
            unsigned long Coverage, Position, Color;

            if (MaskX >= MaskWidth)
                MaskX = MaskWidth - 1;

            Coverage = bootui_mark[MaskY * MaskWidth + MaskX];
            if (!Coverage)
                continue;

            /* Position inside the mark, diagonally: the band travels along it. */
            Position = ((X * 255) / (MarkSize ? MarkSize : 1) +
                        (Y * 255) / (MarkSize * MaskHeight / MaskWidth + 1)) / 2;

            /* The two chevrons are told apart by where they sit: the left one
               uses the darker of the two ramps. */
            Color = StrokeColor(Position, Phase, ScreenX > MarkLeft + MarkSize / 2);

            if (Coverage >= 255)
            {
                Row[ScreenX] = Color;
            }
            else
            {
                unsigned long Backdrop = Row[ScreenX];
                Row[ScreenX] = Pack(Channel(Backdrop, 16) + (Channel(Color, 16) - Channel(Backdrop, 16)) * Coverage / 255,
                                    Channel(Backdrop, 8) + (Channel(Color, 8) - Channel(Backdrop, 8)) * Coverage / 255,
                                    Channel(Backdrop, 0) + (Channel(Color, 0) - Channel(Backdrop, 0)) * Coverage / 255);
            }
        }
    }
}
