/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The startup screen: black, with the NextNT chevrons in the middle
 *
 * The drawing is the reference artwork itself, not an approximation of it. Three
 * assets come out of it, all at its own size (143 x 123), so the first frame of
 * the animation is the artwork: same shape, same colours, same gradient.
 *   nextnt-mark.alpha -- the silhouette, one byte per pixel, for the shape;
 *   nextnt-mark.rgb   -- the colour of every pixel: the artwork's gradient,
 *                        which is not a ramp along any one direction (it is
 *                        brighter along the inner edge and near the bend), so it
 *                        is carried as it is instead of being invented;
 *   nextnt-mark.path  -- how far each pixel is along its stroke, measured from
 *                        the bend, 0..255. Nothing to do with the artwork's
 *                        colour: it is the coordinate a moving light needs.
 * Both non-mask assets are extended just outside the strokes, so a bilinear
 * sample at the edge never reads a hole.
 *
 * The animation is a light travelling along that coordinate, from the bend out to
 * the two ends and back, one lap every ContourPeriodMs. It is a cosine in both
 * space and time, in 16.16 fixed point, so the colour travels continuously and
 * the lap joins itself with no seam; it is added to the artwork's own colours and
 * its strength fades in over the first ContourFadeMs, which is what keeps frame
 * zero identical to the reference.
 *
 * Cost matters as much as the look: this runs on firmware with no frame buffer
 * and no cache to speak of, and an animation that renders at six frames a second
 * looks broken however good the gradient is. So a frame touches only the
 * rectangle the mark occupies, samples the silhouette once per pixel (not nine
 * times), and keeps every operation in integers.
 *
 * Freestanding C++: no headers beyond the local ones, no allocation, no static
 * objects with constructors, no exceptions, no RTTI, no floating point.
 */

#include "splash.h"
#include "bootui_mark.h"
#include "bootui_markrgb.h"
#include "bootui_markpath.h"

namespace
{

/* The assets' own size, fixed by the artwork. */
const unsigned long MaskWidth = 143;
const unsigned long MaskHeight = 123;

/* How long the gradient takes to slide through one full turn, and how long it
   takes to fade in at boot so that the first frame is the reference image. Twenty
   seconds: the slide is meant to be noticed only after looking at it for a
   moment, not a sweep you can follow with your eyes. ContourStrength is how far
   the colours travel, out of 255. */
const unsigned long ContourPeriodMs = 20000;

/* How strongly the gradient slides, out of 255: the artwork's own colours moved
   by up to this much towards the pale blue of its highlight. Low on purpose --
   the mark must stay the mark, with light moving inside it. */
const unsigned long SlideDepth = 96;
const unsigned long ContourFadeMs = 2000;
const unsigned long ContourStrength = SlideDepth;

/* Rows are presented in multiples of this, so a frame is one rectangular blit
   with no ragged edge at the top or the bottom of the band. */
const unsigned long BandAlign = 8;

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

/* Sine over a full turn, 16.16: 65536 is one. Angle is 0..65535 for 0..360
   degrees. The table is a quarter wave; the other three quarters are the same
   numbers read backwards or negated, and those joins are exactly 0 and one, so
   the curve has no corner in it. */
const unsigned long SinTable[65] =
{
    0, 1608, 3216, 4821, 6424, 8022, 9616, 11204,
    12785, 14359, 15924, 17479, 19024, 20557, 22078, 23586,
    25080, 26558, 28020, 29466, 30893, 32303, 33692, 35062,
    36410, 37736, 39040, 40320, 41576, 42806, 44011, 45190,
    46341, 47464, 48559, 49624, 50660, 51665, 52639, 53581,
    54491, 55368, 56212, 57022, 57798, 58538, 59244, 59914,
    60547, 61145, 61705, 62228, 62714, 63162, 63572, 63944,
    64277, 64571, 64827, 65043, 65220, 65358, 65457, 65516,
    65536
};

unsigned long
Quarter(unsigned long Index, unsigned long Frac)
{
    unsigned long Low = SinTable[Index];
    unsigned long High = SinTable[Index + 1];

    return Low + ((High - Low) * Frac) / 256;
}

long
SinQ16(unsigned long Angle)
{
    unsigned long Quadrant = (Angle >> 14) & 3;
    unsigned long Within = Angle & 0x3FFF;
    unsigned long Index = Within >> 8;
    unsigned long Frac = Within & 0xFF;

    switch (Quadrant)
    {
        case 1:
            return (long)Quarter(63 - Index, 255 - Frac);
        case 2:
            return -(long)Quarter(Index, Frac);
        case 3:
            return -(long)Quarter(63 - Index, 255 - Frac);
        default:
            return (long)Quarter(Index, Frac);
    }
}

long
CosQ16(unsigned long Angle)
{
    return SinQ16((Angle + 16384) & 0xFFFF);
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

/* One byte of an asset at a fractional mask coordinate, bilinear, 0..255. */
unsigned long
SampleByte(const unsigned char *Asset, unsigned long MulX, unsigned long MulY)
{
    unsigned long CellX = MulX >> 16;
    unsigned long CellY = MulY >> 16;
    unsigned long FracX = (MulX >> 8) & 0xFF;
    unsigned long FracY = (MulY >> 8) & 0xFF;
    unsigned long NextX, NextY, Top, Bottom;

    /* Clamp the neighbours, not the cell: clamping the cell would shift the last
       row and column of the asset by one pixel. */
    if (CellX >= MaskWidth) CellX = MaskWidth - 1;
    if (CellY >= MaskHeight) CellY = MaskHeight - 1;
    NextX = CellX + 1 < MaskWidth ? CellX + 1 : CellX;
    NextY = CellY + 1 < MaskHeight ? CellY + 1 : CellY;

    Top = Asset[CellY * MaskWidth + CellX] * (256 - FracX) +
          Asset[CellY * MaskWidth + NextX] * FracX;
    Bottom = Asset[NextY * MaskWidth + CellX] * (256 - FracX) +
             Asset[NextY * MaskWidth + NextX] * FracX;

    return ((Top * (256 - FracY) + Bottom * FracY) >> 16) & 0xFF;
}

/* The artwork's colour at mask coordinates (MulX, MulY) in 16.16, bilinear. */
unsigned long
SampleArtwork(unsigned long MulX, unsigned long MulY)
{
    unsigned long CellX = MulX >> 16;
    unsigned long CellY = MulY >> 16;
    unsigned long FracX = (MulX >> 8) & 0xFF;
    unsigned long FracY = (MulY >> 8) & 0xFF;
    unsigned long Row0, Row1, StepX, Red, Green, Blue;
    const unsigned char *Texel = bootui_markrgb;

    if (CellX >= MaskWidth) CellX = MaskWidth - 1;
    if (CellY >= MaskHeight) CellY = MaskHeight - 1;
    StepX = CellX + 1 < MaskWidth ? 3 : 0;

    Row0 = CellY * MaskWidth + CellX;
    Row1 = CellY + 1 < MaskHeight ? Row0 + MaskWidth : Row0;

    Red = (Texel[Row0 * 3 + 0] * (256 - FracX) + Texel[Row0 * 3 + StepX] * FracX) * (256 - FracY) +
          (Texel[Row1 * 3 + 0] * (256 - FracX) + Texel[Row1 * 3 + StepX] * FracX) * FracY;
    Green = (Texel[Row0 * 3 + 1] * (256 - FracX) + Texel[Row0 * 3 + 1 + StepX] * FracX) * (256 - FracY) +
            (Texel[Row1 * 3 + 1] * (256 - FracX) + Texel[Row1 * 3 + 1 + StepX] * FracX) * FracY;
    Blue = (Texel[Row0 * 3 + 2] * (256 - FracX) + Texel[Row0 * 3 + 2 + StepX] * FracX) * (256 - FracY) +
           (Texel[Row1 * 3 + 2] * (256 - FracX) + Texel[Row1 * 3 + 2 + StepX] * FracX) * FracY;

    return Pack(Red >> 16, Green >> 16, Blue >> 16);
}

/* How far the gradient has slid at stroke position Position (0 at one tip, 128
   at the bend, 255 at the other tip), when the slide is at Slide (same scale,
   one turn of Slide = one lap).

   This is not a spotlight travelling along the stroke: that is what produced a
   bright band with two edges crossing the mark, and a band with edges is read as
   a line however smooth its profile is. It is the gradient itself moving: one
   cosine over the whole stroke, so the transition is spread across the entire
   length of the chevron and there is no edge anywhere, only a slow slide from
   deep to light and back. The result is 0..65535, so the caller can dither the
   low bits instead of banding them. */
unsigned long
ContourLight(unsigned long Position, unsigned long Slide)
{
    unsigned long Angle = ((Slide * 65536) / 255 + 16384 - Position * 257) & 0xFFFF;

    return (unsigned long)((65536 + CosQ16(Angle)) / 2);
}

/* Ordered four by four dither, 0..15. Added to the low bits of the light, it
   turns what would be a visible band across a stroke into a fine grain that the
   eye reads as a smooth gradient. */
unsigned long
Dither4x4(unsigned long Y, unsigned long X)
{
    static const unsigned char Table[16] =
    {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5
    };

    return Table[((Y & 3) << 2) | (X & 3)];
}

} /* namespace */

extern "C" void
SplashBand(unsigned long *Pixels, unsigned long Width, unsigned long Height,
           unsigned long TimeMs, unsigned long *Top, unsigned long *Bottom)
{
    unsigned long MarkH, MarkW, Left, MarkTop, First, Last, Y, X;
    unsigned long ScaleX, ScaleY, Phase, Wave, Strength;

    if (Top) *Top = 0;
    if (Bottom) *Bottom = 0;
    if (!Pixels || !Width || !Height)
        return;

    MarkH = MarkHeight(Width, Height);
    MarkW = MarkWidth(MarkH);
    Left = (Width - MarkW) / 2;
    MarkTop = (Height - MarkH) / 2;

    /* A whole number of aligned rows, covering the mark with a little air, so
       the caller presents one rectangle instead of a band with two ragged
       edges -- ragged edges are how a frame ends up looking torn. */
    First = (MarkTop / BandAlign) * BandAlign;
    Last = MarkTop + MarkH;
    if (Last % BandAlign) Last += BandAlign - (Last % BandAlign);
    if (Last > Height) Last = Height;

    ScaleX = (MaskWidth << 16) / MarkW;
    ScaleY = (MaskHeight << 16) / MarkH;

    /* The light sweeps the whole stroke once per lap, from one tip through the
       bend to the other tip. Its position is in 0..255 so it shares the scale of
       the asset, and it is computed in 16.16 so a frame moves it by a fraction of
       a step rather than in jumps. */
    Phase = (unsigned long)(((unsigned long long)(TimeMs % ContourPeriodMs) * 255ull * 65536ull / ContourPeriodMs)) & 0xFFFF;
    Wave = Phase >> 8;

    /* Faded in, so the screen at time zero is the reference frame. */
    Strength = TimeMs >= ContourFadeMs ? ContourStrength : (ContourStrength * TimeMs) / ContourFadeMs;

    for (Y = MarkTop; Y < MarkTop + MarkH && Y < Height; Y++)
    {
        unsigned long *Row = Pixels + Y * Width;
        unsigned long MulY = (Y - MarkTop) * ScaleY;

        /* Only the columns the mark covers: the rest of the screen is black
           already, painted once by SplashStartup, and the mark never moves. */
        for (X = Left; X < Left + MarkW; X++)
        {
            unsigned long MulX = (X - Left) * ScaleX;
            unsigned long Coverage = SampleByte(bootui_mark, MulX, MulY);
            unsigned long Color, Contour, Light;
            long Red, Green, Blue;

            if (!Coverage)
                continue;

            Color = SampleArtwork(MulX, MulY);
            Contour = SampleByte(bootui_markpath, MulX, MulY);

            /* The light in sixteen bits, dithered down to eight: the low bits
               decide between two neighbouring levels, so a slow gradient gets a
               grain instead of a band. */
            Light = (ContourLight(Contour, Wave) * Strength) >> 8;
            Light = (Light >> 8) + (((Light & 0xFF) > Dither4x4(Y, X) * 16) ? 1 : 0);

            /* The light pulls the pixel part of the way towards the highlight,
               the pale blue the artwork already reaches near the bend. */
            Red = (long)Channel(Color, 16) + ((30 - (long)Channel(Color, 16)) * (long)Light) / 255;
            Green = (long)Channel(Color, 8) + ((196 - (long)Channel(Color, 8)) * (long)Light) / 255;
            Blue = (long)Channel(Color, 0) + ((248 - (long)Channel(Color, 0)) * (long)Light) / 255;

            /* The artwork is colour on black; the silhouette says how much of the
               pixel the stroke covers. */
            Row[X] = Pack(((unsigned long)Red * Coverage) / 255,
                          ((unsigned long)Green * Coverage) / 255,
                          ((unsigned long)Blue * Coverage) / 255);
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
