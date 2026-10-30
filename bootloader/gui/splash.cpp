/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Animated gradient background and the NextNT mark
 *
 * Everything here is integer arithmetic on a 32-bit framebuffer. Two things are
 * worth explaining before the code:
 *
 * 1. The background is a palette. One 256-entry table holds the colours from
 *    near-black navy to sapphire, and a pixel is whatever entry a diagonal ramp
 *    gives it. The ramp is a 16.16 fixed-point accumulator that starts at the
 *    row's offset and is advanced by a constant for every column, so a pixel
 *    costs one add, one shift and one table read. The time only moves the
 *    accumulator's phase, which is what makes the gradient slide.
 *
 * 2. A 256-entry palette over a smooth ramp shows bands on a real display, so
 *    before the table is read the index is nudged by a 4x4 ordered dither
 *    matrix. The dither is periodic in x and y, which is exactly why the
 *    pattern does not crawl when the phase moves.
 *
 * The mark is two chevrons drawn from their geometry: two thick strokes each,
 * measured by the distance to the segment that the stroke lies on, with a
 * one-pixel ramp for antialiasing. Nothing is rasterized ahead of time, so the
 * same code is sharp at any size.
 *
 * Freestanding C++: no headers, no allocation, no static objects with
 * constructors, no exceptions, no RTTI. The palette is filled on the first call
 * and guarded by a flag.
 */

#include "splash.h"

namespace
{

/* The gradient's colour stops. Between two stops the entries are interpolated,
   so the curve is a piecewise linear ramp in three channels at once. */
struct Stop
{
    unsigned char Index;
    unsigned char Red;
    unsigned char Green;
    unsigned char Blue;
};

const Stop Stops[] =
{
    {   0, 0x03, 0x05, 0x0A },   /* the deep end: almost black, a trace of blue */
    {  72, 0x05, 0x0D, 0x1B },
    { 132, 0x08, 0x1C, 0x3C },
    { 190, 0x0F, 0x36, 0x77 },
    { 232, 0x17, 0x55, 0xB4 },
    { 255, 0x2C, 0x86, 0xE8 },   /* the bright end of the sweep */
};

/* Bayer's 4 x 4 matrix, recentred on zero: the values are added to a palette
   index, so they must be able to move it either way. */
const signed char Dither[16] =
{
    -8,  0, -6,  2,
     4, -4,  6, -2,
    -5,  3, -7,  1,
     7, -1,  5, -3
};

unsigned long Palette[256];
bool PaletteReady = false;

void
BuildPalette()
{
    unsigned long Entry = 0;
    unsigned int StopIndex = 0;

    for (StopIndex = 0; StopIndex + 1 < sizeof(Stops) / sizeof(Stops[0]); ++StopIndex)
    {
        const Stop &From = Stops[StopIndex];
        const Stop &To = Stops[StopIndex + 1];
        unsigned int Span = To.Index - From.Index;
        unsigned int Step;

        for (Step = 0; Step <= Span; ++Step)
        {
            unsigned int Remaining = Span - Step;
            unsigned int Red = (From.Red * Remaining + To.Red * Step) / Span;
            unsigned int Green = (From.Green * Remaining + To.Green * Step) / Span;
            unsigned int Blue = (From.Blue * Remaining + To.Blue * Step) / Span;

            Palette[From.Index + Step] = (Red << 16) | (Green << 8) | Blue;
        }

        Entry = To.Index;
    }

    /* The last stop is written by the loop above; anything the stops left out
       at the very top of the range repeats it. */
    while (Entry < 256)
        Palette[Entry++] = Palette[Stops[sizeof(Stops) / sizeof(Stops[0]) - 1].Index];

    PaletteReady = true;
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

void
Plot(unsigned long *Pixels, unsigned long Width, unsigned long Height,
     long X, long Y, unsigned long Color, unsigned long Alpha)
{
    unsigned long *Pixel, Backdrop;
    unsigned long Red, Green, Blue;

    if (!Alpha || X < 0 || Y < 0 || (unsigned long)X >= Width || (unsigned long)Y >= Height)
        return;

    Pixel = &Pixels[(unsigned long)Y * Width + (unsigned long)X];
    if (Alpha >= 255)
    {
        *Pixel = Color;
        return;
    }

    Backdrop = *Pixel;
    Red = (Channel(Backdrop, 16) * (255 - Alpha) + Channel(Color, 16) * Alpha) / 255;
    Green = (Channel(Backdrop, 8) * (255 - Alpha) + Channel(Color, 8) * Alpha) / 255;
    Blue = (Channel(Backdrop, 0) * (255 - Alpha) + Channel(Color, 0) * Alpha) / 255;
    *Pixel = Pack(Red, Green, Blue);
}

/* Distance from a point to a segment, squared, in whole pixels. The projection
   is computed with a division because a chevron is a few hundred pixels wide and
   this runs once per arm, not once per scan line. */
long
SegmentDistanceSquared(long Px, long Py, long Ax, long Ay, long Bx, long By)
{
    long Dx = Bx - Ax;
    long Dy = By - Ay;
    long LengthSquared = Dx * Dx + Dy * Dy;
    long Offset, X, Y;

    if (!LengthSquared)
    {
        X = Px - Ax;
        Y = Py - Ay;
        return X * X + Y * Y;
    }

    Offset = (Px - Ax) * Dx + (Py - Ay) * Dy;
    if (Offset <= 0)
    {
        X = Px - Ax;
        Y = Py - Ay;
        return X * X + Y * Y;
    }
    if (Offset >= LengthSquared)
    {
        X = Px - Bx;
        Y = Py - By;
        return X * X + Y * Y;
    }

    /* Multiply before dividing: the ratio Offset / LengthSquared is almost
       always zero in integers, so scaling first is what makes the projection a
       projection instead of a point. */
    X = Px - (Ax + (Offset * Dx) / LengthSquared);
    Y = Py - (Ay + (Offset * Dy) / LengthSquared);
    return X * X + Y * Y;
}

/* One chevron: a stroke of the given thickness along each of the two arms that
   meet at (CenterX, CenterY). The colour runs from Dark at the top to Light at
   the bottom, which is what gives the mark its lit edge. */
void
Chevron(unsigned long *Pixels, unsigned long Width, unsigned long Height,
        long CenterX, long CenterY, long HalfHeight, long HalfWidth,
        long Thickness, unsigned long Dark, unsigned long Light,
        unsigned long Pulse)
{
    long Left, Right, Top, Bottom, X, Y;

    Left = CenterX - HalfWidth - Thickness;
    Right = CenterX + Thickness;
    Top = CenterY - HalfHeight - Thickness;
    Bottom = CenterY + HalfHeight + Thickness;

    for (Y = Top; Y <= Bottom; ++Y)
    {
        for (X = Left; X <= Right; ++X)
        {
            long Upper, Lower, Nearest, Alpha, Shade;
            unsigned long Color;

            Upper = SegmentDistanceSquared(X, Y, CenterX - HalfWidth, CenterY - HalfHeight,
                                           CenterX, CenterY);
            Lower = SegmentDistanceSquared(X, Y, CenterX, CenterY,
                                           CenterX - HalfWidth, CenterY + HalfHeight);
            Nearest = Upper < Lower ? Upper : Lower;
            if (Nearest > Thickness * Thickness)
                continue;

            /* One pixel of ramp at the stroke's edge, so the chevron is not a
               staircase at small sizes. */
            Alpha = (Thickness - 1) * (Thickness - 1);
            if (Nearest > Alpha)
            {
                long Outer = Thickness * Thickness;
                long Span = Outer - Alpha;
                Alpha = 255 - ((Nearest - Alpha) * 255) / (Span ? Span : 1);
            }
            else
            {
                Alpha = 255;
            }

            /* Vertical shade across the arm plus a slow breathing pulse. */
            Shade = (Y - Top) * 255 / (Bottom - Top + 1);
            Color = Pack(Channel(Dark, 16) + (Channel(Light, 16) - Channel(Dark, 16)) * Shade / 255,
                         Channel(Dark, 8) + (Channel(Light, 8) - Channel(Dark, 8)) * Shade / 255,
                         Channel(Dark, 0) + (Channel(Light, 0) - Channel(Dark, 0)) * Shade / 255);
            Alpha = Alpha * Pulse / 255;

            Plot(Pixels, Width, Height, X, Y, Color, (unsigned long)Alpha);
        }
    }
}

} /* namespace */

extern "C" void
SplashFillBackground(unsigned long *Pixels, unsigned long Width, unsigned long Height,
                     unsigned long Top, unsigned long Bottom, unsigned long TimeMs)
{
    /* Phase: one full turn of the ramp every six seconds, in 16.16 fixed point. */
    unsigned long Phase = (TimeMs * 2796u) & 0xFFFFFFu;
    unsigned long RowStep, ColumnStep;
    unsigned long Y, X;

    if (!Pixels || !Width || !Height || Top >= Bottom)
        return;

    if (!PaletteReady)
        BuildPalette();

    if (Bottom > Height)
        Bottom = Height;

    /* The ramp crosses the whole palette once along the diagonal of the screen:
       5/8 of the range over the width, the rest over the height. */
    ColumnStep = ((256ul * 5ul / 8ul) << 16) / Width;
    RowStep = ((256ul * 3ul / 8ul) << 16) / (Height ? Height : 1);

    for (Y = Top; Y < Bottom; ++Y)
    {
        unsigned long Accumulator = (RowStep * Y + Phase) & 0xFFFFFFul;
        unsigned long *Row = Pixels + Y * Width;
        unsigned int RowDither = (unsigned int)(Y & 3) << 2;

        for (X = 0; X < Width; ++X)
        {
            unsigned int Index = (unsigned int)(Accumulator >> 16);
            signed char Nudge = Dither[RowDither | (X & 3)];

            /* The dither is applied on the index, before the mask, so wrapping
               at the end of the palette stays seamless. */
            Row[X] = Palette[(unsigned int)((int)(Index & 0xFF) + Nudge) & 0xFF];
            Accumulator += ColumnStep;
        }
    }
}

extern "C" void
SplashDrawMark(unsigned long *Pixels, unsigned long Width, unsigned long Height,
               long CenterX, long CenterY, unsigned long Size, unsigned long TimeMs)
{
    long HalfHeight, HalfWidth, Thickness, Gap, Pulse, Phase;

    if (!Pixels || !Size || Size < 8)
        return;

    /* Proportions taken from the mark: each chevron is twice as tall as it is
       wide, the stroke is a sixth of the height, and the two chevrons overlap by
       a fifth of their width. */
    HalfHeight = (long)Size / 2;
    HalfWidth = (long)Size / 4;
    Thickness = (long)Size / 6;
    Gap = (long)Size / 5;

    if (Thickness < 1)
        Thickness = 1;

    /* A slow breath between 82 and 100 per cent, so the mark is alive without
       flickering: one cycle per four seconds. */
    Phase = (long)((TimeMs / 8) % 500);
    if (Phase > 250)
        Phase = 500 - Phase;
    Pulse = 210 + Phase * 45 / 250;

    Chevron(Pixels, Width, Height, CenterX - Gap, CenterY, HalfHeight, HalfWidth,
            Thickness, 0x0A3F8C, 0x1E6FD0, (unsigned long)Pulse);
    Chevron(Pixels, Width, Height, CenterX + Gap, CenterY, HalfHeight, HalfWidth,
            Thickness, 0x1268C8, 0x49B0FF, (unsigned long)Pulse);
}
