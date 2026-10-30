/*
 * PROJECT:     NextNT
 * FILE:        boot/nextldr/nextfsldr/div64.c
 * PURPOSE:     The only 64-bit arithmetic the first stage needs to provide
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 *
 * nextfs_core.c divides 64-bit file offsets by the cluster size. A freestanding
 * build has no libgcc to call, and this is a boot loader, so the division is the
 * plain restoring one -- slow, sixty-four shifts, and completely irrelevant next
 * to the sector reads that surround it.
 */

#include "nextfsldr.h"

typedef unsigned long long u64_t;
typedef long long s64_t;

u64_t __udivmoddi4(u64_t Dividend, u64_t Divisor, u64_t *Remainder)
{
    u64_t Quotient = 0;
    u64_t Rest = 0;
    int Bit;

    if (Divisor == 0)
    {
        if (Remainder != 0)
            *Remainder = 0;
        return 0;
    }

    for (Bit = 63; Bit >= 0; Bit--)
    {
        Rest = (Rest << 1) | ((Dividend >> Bit) & 1ull);
        if (Rest >= Divisor)
        {
            Rest -= Divisor;
            Quotient |= (1ull << Bit);
        }
    }

    if (Remainder != 0)
        *Remainder = Rest;

    return Quotient;
}

u64_t __udivdi3(u64_t Dividend, u64_t Divisor)
{
    return __udivmoddi4(Dividend, Divisor, 0);
}

u64_t __umoddi3(u64_t Dividend, u64_t Divisor)
{
    u64_t Remainder = 0;
    __udivmoddi4(Dividend, Divisor, &Remainder);
    return Remainder;
}

s64_t __divdi3(s64_t Dividend, s64_t Divisor)
{
    u64_t Quotient;
    int Negative = 0;

    if (Dividend < 0)
    {
        Dividend = -Dividend;
        Negative ^= 1;
    }
    if (Divisor < 0)
    {
        Divisor = -Divisor;
        Negative ^= 1;
    }

    Quotient = __udivmoddi4((u64_t)Dividend, (u64_t)Divisor, 0);
    return Negative ? -(s64_t)Quotient : (s64_t)Quotient;
}

s64_t __moddi3(s64_t Dividend, s64_t Divisor)
{
    u64_t Remainder = 0;
    int Negative = (Dividend < 0);

    if (Dividend < 0)
        Dividend = -Dividend;
    if (Divisor < 0)
        Divisor = -Divisor;

    __udivmoddi4((u64_t)Dividend, (u64_t)Divisor, &Remainder);
    return Negative ? -(s64_t)Remainder : (s64_t)Remainder;
}
