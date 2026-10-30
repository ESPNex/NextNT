/*
 * PROJECT:     NextNT Boot Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The startup screen: black, with the NextNT chevrons in the middle
 *
 * The shape is not described in code and not simplified: it comes from
 * assets/nextnt-mark.alpha, the coverage mask taken from the reference artwork
 * (142 x 122, one byte per pixel, brightest channel of the drawing), which
 * native-bin2c turns into bootui_mark. What is described in code is the colour:
 * a five-stop ramp that runs through the strokes and slides with the clock,
 * with a bright specular edge crossing it the other way, a vertical shade, and
 * a soft halo around the mark so its ends read against the black instead of
 * dissolving into it.
 *
 * Everything is integer arithmetic: fixed point for the sampling, tables for the
 * ramp. No floating point, because the firmware is not promised an FPU here.
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

/* The ramp inside the strokes, from the deep blue of the artwork to a cold
   highlight. Five stops: a two-stop ramp reads as a smear, five read as glass. */
struct RAMPSTOP
{
    unsigned long At;      /* 0..255 along the ramp */
    unsigned long Red;
    unsigned long Green;
    unsigned long Blue;
};

const RAMPSTOP Ramp[] =
{
    {   0,  10,  34,  88 },
    {  64,  18,  74, 160 },
    { 128,  34, 130, 220 },
    { 192,  92, 198, 248 },
    { 255, 150, 232, 255 },
};

const unsigned long RampCount = sizeof(Ramp) / sizeof(Ramp[0]);

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

/* Colour of the ramp at At = 0..255: linear between the two stops around it. */
unsigned long
RampColor(unsigned long At, unsigned long* Red, unsigned long* Green, unsigned long* Blue)
{
    unsigned long i;

    for (i = 1; i + 1 < RampCount; i++)
    {
        if (At <= Ramp[i].At)
            break;
        if (i + 2 == RampCount)
        {
            i = RampCount - 1;
            break;
        }
    }

    if (i >= RampCount - 1)
    {
        *Red = Ramp[RampCount - 1].Red;
        *Green = Ramp[RampCount - 1].Green;
        *Blue = Ramp[RampCount - 1].Blue;
    }
    else
    {
        unsigned long Span = Ramp[i].At - Ramp[i - 1].At;
        unsigned long Part = Span ? (At - Ramp[i - 1].At) : 0;

        *Red = Ramp[i - 1].Red + ((Ramp[i].Red - Ramp[i - 1].Red) * Part) / Span;
        *Green = Ramp[i - 1].Green + ((Ramp[i].Green - Ramp[i - 1].Green) * Part) / Span;
        *Blue = Ramp[i - 1].Blue + ((Ramp[i].Blue - Ramp[i - 1].Blue) * Part) / Span;
    }

    return Pack(*Red, *Green, *Blue);
}

/* The mark occupies a sixth of the screen's height: present, not shouty. The
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

/* One pixel inside the strokes: the ramp sliding one way, the specular edge
   crossing the other way, and a vertical shade so the mark sits in space. */
unsigned long
StrokeColor(unsigned long Across, unsigned long Down, unsigned long Phase)
{
    unsigned long Position, Along, Red, Green, Blue, Shade, Spec;

    /* The ramp travels diagonally, from the upper left to the lower right. */
    Position = (Across * 156) / 255 + (Down * 84) / 255 + Phase;
    Along = Position & 0xFF;
    Along = Along < 128 ? Along * 2 : (255 - Along) * 2;

    RampColor(Along, &Red, &Green, &Blue);

    /* Shade: the top of the strokes a touch brighter than the bottom. */
    Shade = 255 - (Down * 46) / 255;
    Red = (Red * Shade) / 255;
    Green = (Green * Shade) / 255;
    Blue = (Blue * Shade) / 255;

    /* Specular: a narrow, brighter band going the other way. */
    Position = (Across * 200) / 255 + (Down * 128) / 255 + 255 - Phase;
    Spec = Position & 0xFF;
    Spec = Spec < 34 ? (34 - Spec) * 7 : (Spec > 222 ? (Spec - 222) * 7 : 0);
    if (Spec > 120) Spec = 120;

    return Pack(Red + (Spec * 55) / 120,
                Green + (Spec * 100) / 120,
                Blue + (Spec * 115) / 120);
}

} /* namespace */

extern "C" void
SplashBand(unsigned long *Pixels, unsigned long Width, unsigned long Height,
           unsigned long TimeMs, unsigned long *Top, unsigned long *Bottom)
{
    unsigned long MarkH, MarkW, Left, MarkTop, Radius, First, Last, Y, X;
    unsigned long CenterX, CenterY, Radius2, Phase;

    if (Top) *Top = 0;
    if (Bottom) *Bottom = 0;
    if (!Pixels || !Width || !Height)
        return;

    MarkH = MarkHeight(Width, Height);
    MarkW = MarkWidth(MarkH);
    Left = (Width - MarkW) / 2;
    MarkTop = (Height - MarkH) / 2;
    CenterX = Width / 2;
    CenterY = Height / 2;

    /* The halo is what keeps the ends of the strokes from ending in a hard edge
       against the black; it reaches a little further than the mark itself. */
    Radius = (MarkW * 58) / 100;
    Radius2 = Radius * Radius;

    First = MarkTop > Radius ? MarkTop - Radius : 0;
    Last = MarkTop + MarkH + Radius;
    if (Last > Height)
        Last = Height;

    /* One crossing of the ramp every four seconds, in 0..255 units. */
    Phase = (unsigned long)((TimeMs * 256ull) / 4000ull) & 0xFF;

    for (Y = First; Y < Last; Y++)
    {
        unsigned long *Row = Pixels + Y * Width;
        unsigned long Down;
        long DeltaY;
        bool InMark;

        InMark = (Y >= MarkTop && Y < MarkTop + MarkH);
        Down = InMark ? (Y - MarkTop) * 255 / MarkH : 0;
        DeltaY = (long)Y - (long)CenterY;

        for (X = 0; X < Width; X++)
        {
            unsigned long Coverage = 0;
            long DeltaX;

            /* Halo first, on the black. Outside its reach the row stays black. */
            DeltaX = (long)X - (long)CenterX;
            if (DeltaX > -(long)Radius && DeltaX < (long)Radius)
            {
                /* Squashed vertically: a wide, low glow sits behind the mark like
                   light on a black surface, a round one looks like a smudge. */
                long SquashedY = DeltaY + DeltaY / 3;
                unsigned long Distance2 = (unsigned long)(DeltaX * DeltaX) +
                                          (unsigned long)(SquashedY * SquashedY);

                if (Distance2 < Radius2)
                {
                    unsigned long Fall = ((Radius2 - Distance2) * 255) / Radius2;

                    /* Cubed: the glow stays tight and fades out without a rim. */
                    Fall = (Fall * Fall) / 255;
                    Fall = (Fall * Fall) / 255;
                    Row[X] = Pack((Fall * 6) / 255, (Fall * 30) / 255, (Fall * 84) / 255);
                }
                else
                {
                    Row[X] = 0;
                }
            }
            else
            {
                Row[X] = 0;
            }

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
                unsigned long Color = StrokeColor((X - Left) * 255 / MarkW, Down, Phase);
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
