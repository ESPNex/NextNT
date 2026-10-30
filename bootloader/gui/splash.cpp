/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The startup screen: black, with the NextNT chevrons in the middle
 *
 * The shape is not described in code and not simplified: it comes from
 * assets/nextnt-mark.alpha, the coverage mask taken from the reference artwork
 * (142 x 122, one byte per pixel, brightest channel of the drawing), which
 * native-bin2c turns into bootui_mark. What is described in code is the colour:
 * the two blues of the artwork with a reflection travelling through them, and a
 * vertical shade so the mark sits in space.
 *
 * Everything is integer arithmetic, fixed point for sampling and phase. No
 * floating point, because the firmware is not promised an FPU here.
 *
 * Freestanding C++: no headers beyond the two local ones, no allocation, no
 * static objects with constructors, no exceptions, no RTTI.
 */

#include "splash.h"
#include "bootui_mark.h"

namespace
{

/* The mask's own size, fixed by the artwork. */
const unsigned long MaskWidth = 142;
const unsigned long MaskHeight = 122;

/* Sub-samples per axis when scaling the mask down. Three is enough to take the
   staircase out of the ends of the strokes without becoming the slowest thing
   in the boot path. */
const unsigned long SubSamples = 3;

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

inline unsigned long
Clamp(unsigned long Value, unsigned long Low, unsigned long High)
{
    if (Value < Low) return Low;
    if (Value > High) return High;
    return Value;
}

/* The mark occupies a fifth of the screen's height: present, not shouty. The
   limits keep it sensible on a small panel and on a wall of a monitor. */
unsigned long
MarkHeight(unsigned long Width, unsigned long Height)
{
    unsigned long ByHeight = Height / 5;
    unsigned long ByWidth = (Width / 5) * 2 * MaskHeight / MaskWidth;

    if (ByHeight > ByWidth)
        ByHeight = ByWidth;
    return Clamp(ByHeight, 60, 480);
}

unsigned long
MarkWidth(unsigned long Height)
{
    return Height * MaskWidth / MaskHeight;
}

/* Coverage of the mask at mask coordinates (MulX, MulY) in 16.16 fixed point,
   bilinear, 0..255. */
unsigned long
SampleMask(unsigned long MulX, unsigned long MulY)
{
    unsigned long CellX = MulX >> 16;
    unsigned long CellY = MulY >> 16;
    unsigned long FracX = (MulX >> 8) & 0xFF;
    unsigned long FracY = (MulY >> 8) & 0xFF;
    unsigned long X0, Y0, X1, Y1;
    unsigned long Top, Bottom, Value;

    if (CellX >= MaskWidth - 1) CellX = MaskWidth - 2;
    if (CellY >= MaskHeight - 1) CellY = MaskHeight - 2;

    X0 = CellX;
    Y0 = CellY;
    X1 = CellX + 1;
    Y1 = CellY + 1;

    Top = bootui_mark[Y0 * MaskWidth + X0] * (256 - FracX) +
          bootui_mark[Y0 * MaskWidth + X1] * FracX;
    Bottom = bootui_mark[Y1 * MaskWidth + X0] * (256 - FracX) +
             bootui_mark[Y1 * MaskWidth + X1] * FracX;
    Value = (Top * (256 - FracY) + Bottom * FracY) >> 16;

    return Value > 255 ? 255 : Value;
}

/* One pixel inside the strokes. The two blues of the artwork, with a slow
   reflection travelling through them: one full crossing every three seconds,
   continuous -- no triangle wave, no seam where the light turns around, because
   the turnaround is where a gradient stops looking like light and starts looking
   like a moving stripe. Position and phase are both 16.16, so each frame moves
   the reflection by a fraction of a step and the change reads as a flow rather
   than as a jump. */
unsigned long
StrokeColor(unsigned long Across, unsigned long Down, unsigned long Phase, bool Deep)
{
    unsigned long Along, Light, Red, Green, Blue, Shade;

    /* Across and Down are 0..255; the reflection runs diagonally from the upper
       left to the lower right, and Phase is 16.16 of 0..255 per cycle. */
    Along = ((Across * 156) / 255 + (Down * 84) / 255) * 256;
    Along = (Along + Phase) & 0xFFFF;

    /* Light along the stroke, zero at both ends and one at the middle, so the
       crossing joins itself without a visible edge. Squared, so the bright part
       stays narrow and the rest of the stroke keeps the deep blue. */
    Light = Along < 32768 ? Along / 128 : (65536 - Along) / 128;
    Light = (Light * Light) / 255;

    /* The two chevrons are told apart the way the artwork does it: the left one
       a little deeper than the right. */
    Red = 10 + (Light * (Deep ? 34 : 52)) / 255;
    Green = 62 + (Light * (Deep ? 122 : 150)) / 255;
    Blue = 158 + (Light * (Deep ? 78 : 96)) / 255;

    /* A vertical shade, so the mark sits in space instead of floating flat. */
    Shade = 255 - (Down * 34) / 255;
    Red = (Red * Shade) / 255;
    Green = (Green * Shade) / 255;
    Blue = (Blue * Shade) / 255;

    return Pack(Red, Green, Blue);
}

} /* namespace */

extern "C" void
SplashBand(unsigned long *Pixels, unsigned long Width, unsigned long Height,
           unsigned long TimeMs, unsigned long *Top, unsigned long *Bottom)
{
    unsigned long MarkH, MarkW, Left, MarkTop, First, Last, Y, X;
    unsigned long Phase;

    if (Top) *Top = 0;
    if (Bottom) *Bottom = 0;
    if (!Pixels || !Width || !Height)
        return;

    MarkH = MarkHeight(Width, Height);
    MarkW = MarkWidth(MarkH);
    Left = (Width - MarkW) / 2;
    MarkTop = (Height - MarkH) / 2;

    First = MarkTop;
    Last = MarkTop + MarkH;
    if (Last > Height)
        Last = Height;

    /* One crossing of the reflection every three seconds, 16.16 of 0..255: the
       caller advances a millisecond at a time, so the motion is a flow. */
    Phase = (unsigned long)(((unsigned long long)TimeMs * 256ull * 65536ull / 3000ull) & 0xFFFF);

    for (Y = First; Y < Last; Y++)
    {
        unsigned long *Row = Pixels + Y * Width;
        unsigned long Down;
        bool InMark;

        InMark = (Y >= MarkTop && Y < MarkTop + MarkH);
        Down = InMark ? (Y - MarkTop) * 255 / MarkH : 0;

        for (X = 0; X < Width; X++)
        {
            unsigned long Coverage = 0;

            /* The screen is black; the mark is the only thing on it. */
            Row[X] = 0;

            if (!InMark || X < Left || X >= Left + MarkW)
                continue;

            /* Nine sub-samples of the mask, averaged: the mark stays sharp at any
               size instead of stepping from pixel to pixel. */
            {
                unsigned long SX, SY, Accumulator = 0;
                unsigned long ScaleX = (MaskWidth << 16) / MarkW;
                unsigned long ScaleY = (MaskHeight << 16) / MarkH;
                unsigned long BaseX = (X - Left) * ScaleX;
                unsigned long BaseY = (Y - MarkTop) * ScaleY;

                for (SY = 0; SY < SubSamples; SY++)
                {
                    unsigned long SampleY = BaseY + (ScaleY * (2 * SY + 1)) / (2 * SubSamples);

                    for (SX = 0; SX < SubSamples; SX++)
                    {
                        unsigned long SampleX = BaseX + (ScaleX * (2 * SX + 1)) / (2 * SubSamples);

                        Accumulator += SampleMask(SampleX, SampleY);
                    }
                }

                Coverage = Accumulator / (SubSamples * SubSamples);
            }

            if (!Coverage)
                continue;

            {
                unsigned long Color = StrokeColor((X - Left) * 255 / MarkW, Down, Phase, X < Left + MarkW / 2);
                unsigned long Backdrop = Row[X];

                if (Coverage >= 250)
                {
                    Row[X] = Color;
                }
                else
                {
                    Row[X] = Pack(Channel(Backdrop, 16) + ((Channel(Color, 16) - Channel(Backdrop, 16)) * Coverage) / 255,
                                  Channel(Backdrop, 8) + ((Channel(Color, 8) - Channel(Backdrop, 8)) * Coverage) / 255,
                                  Channel(Backdrop, 0) + ((Channel(Color, 0) - Channel(Backdrop, 0)) * Coverage) / 255);
                }
            }
        }
    }

    if (Top) *Top = First;
    if (Bottom) *Bottom = Last;
}

extern "C" void
SplashStartup(unsigned long *Pixels, unsigned long Width, unsigned long Height,
              unsigned long TimeMs)
{
    unsigned long Y, Top, Bottom;

    if (!Pixels || !Width || !Height)
        return;

    /* Black everywhere, then the band paints itself over it. */
    for (Y = 0; Y < Height; Y++)
    {
        unsigned long *Row = Pixels + Y * Width;
        unsigned long X;

        for (X = 0; X < Width; X++)
            Row[X] = 0;
    }

    SplashBand(Pixels, Width, Height, TimeMs, &Top, &Bottom);
}
