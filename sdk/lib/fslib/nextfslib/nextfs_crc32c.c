/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_crc32c.c
 * PURPOSE:     CRC32C (Castagnoli) for every NextFS on-disk structure
 * LICENSE:     GPL-3.0-or-later
 */

#include "nextfs_format.h"

static nfs_u32 g_NfsCrcTable[256];
static int g_NfsCrcReady = 0;

static void
NfsCrcInit(void)
{
    nfs_u32 i, j, c;

    for (i = 0; i < 256; i++)
    {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_NfsCrcTable[i] = c;
    }
    g_NfsCrcReady = 1;
}

nfs_u32
NfsCrc32c(nfs_u32 Seed, const void *Buffer, size_t Length)
{
    const nfs_u8 *p = (const nfs_u8 *)Buffer;
    nfs_u32 crc = ~Seed;

    if (!g_NfsCrcReady)
        NfsCrcInit();

    while (Length--)
        crc = g_NfsCrcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);

    return ~crc;
}
