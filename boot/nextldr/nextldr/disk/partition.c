/*
 * PROJECT:     FreeLoader
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 *              or MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Block Device partition management
 * COPYRIGHT:   Copyright 2002-2003 Brian Palmer <brianp@sginet.com>
 *              Copyright 2019 Stanislav Motylkov <x86corez@gmail.com>
 *              Copyright 2025-2026 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#ifndef _M_ARM
#include <nextldr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(DISK);

#define MaxDriveNumber 0xFF
static PARTITION_STYLE DiskPartitionType[MaxDriveNumber + 1];

#include "part_mbr.h" // FIXME: For MASTER_BOOT_RECORD

BOOLEAN
DiskReadBootRecord(
    IN UCHAR DriveNumber,
    IN ULONGLONG LogicalSectorNumber,
    OUT PMASTER_BOOT_RECORD BootRecord)
{
    /* Read master boot record */
    if (!MachDiskReadLogicalSectors(DriveNumber, LogicalSectorNumber, 1, DiskReadBuffer))
    {
        return FALSE;
    }
    RtlCopyMemory(BootRecord, DiskReadBuffer, sizeof(MASTER_BOOT_RECORD));

    /* Check the partition table magic value */
    return (BootRecord->MasterBootRecordMagic == 0xaa55);
}

#include "part_mbr.c"
#include "part_gpt.c"
#include "part_brfr.c"

/*
 * Does this sector describe any partition at all? A type byte of zero means
 * the entry is unused, and a zero sector count says the same thing less
 * clearly; anything else is an entry that somebody wrote on purpose. An entry
 * that starts past the end of the disk, or that claims more sectors than the
 * disk has, describes nothing that exists here -- which is what the bytes of a
 * boot record read as a table usually look like.
 */
static BOOLEAN
DiskMbrHasAnyEntry(
    _In_ UCHAR DriveNumber,
    _In_ PMASTER_BOOT_RECORD MasterBootRecord)
{
    GEOMETRY Geometry;
    ULONGLONG DiskSectors = 0;
    ULONG Index;

    if (MachDiskGetDriveGeometry(DriveNumber, &Geometry))
        DiskSectors = Geometry.Sectors;

    for (Index = 0; Index < 4; Index++)
    {
        PPARTITION_TABLE_ENTRY Entry = &MasterBootRecord->PartitionTable[Index];
        ULONGLONG End;

        if ((Entry->SystemIndicator == PARTITION_ENTRY_UNUSED) ||
            (Entry->PartitionSectorCount == 0) ||
            ((Entry->BootIndicator & ~0x80) != 0))
        {
            continue;
        }

        End = (ULONGLONG)Entry->SectorCountBeforePartition + Entry->PartitionSectorCount;
        if ((DiskSectors == 0) || (End <= DiskSectors))
        {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * The unpartitioned case: the disk is one volume. There is no table to read,
 * so the entry is arithmetic -- offset zero, length the whole disk. This is
 * what a NextFS volume formatted in place looks like, and what the installer
 * makes when it is given a disk rather than a partition.
 */
static BOOLEAN
DiskGetRawPartitionEntry(
    _In_ UCHAR DriveNumber,
    _In_ ULONG SectorSize,
    _In_ ULONG PartitionNumber,
    _Out_opt_ PPARTITION_INFORMATION PartitionEntry)
{
    GEOMETRY Geometry;
    PARTITION_INFORMATION TempEntry;

    if (PartitionNumber != 1)
        return FALSE;

    if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
        return FALSE;

    if (SectorSize == 0)
        SectorSize = Geometry.BytesPerSector;

    if (!PartitionEntry)
        PartitionEntry = &TempEntry;

    PartitionEntry->StartingOffset.QuadPart = 0;
    PartitionEntry->PartitionLength.QuadPart =
        (ULONGLONG)Geometry.Sectors * Geometry.BytesPerSector;
    PartitionEntry->HiddenSectors = 0;
    PartitionEntry->PartitionNumber = 1;
    PartitionEntry->PartitionType = PARTITION_NEXTFS;
    PartitionEntry->BootIndicator = TRUE;
    PartitionEntry->RecognizedPartition = TRUE;
    PartitionEntry->RewritePartition = FALSE;

    return TRUE;
}

VOID
DiskDetectPartitionType(
    _In_ UCHAR DriveNumber)
{
    MASTER_BOOT_RECORD MasterBootRecord;

    /* Probe for Master Boot Record */
    if (DiskReadBootRecord(DriveNumber, 0, &MasterBootRecord))
    {
        /* A sector with 0xAA55 in its last two bytes is not necessarily a
           partition table: a NextFS volume formatted in place has its boot
           record there, and the four "entries" are then whatever the boot
           code keeps in that area. Ask for an entry that could describe a
           partition, and if there is none, the file system is the whole disk
           -- a state this file supports anyway, under PARTITION_STYLE_RAW. */
        if (!DiskMbrHasAnyEntry(DriveNumber, &MasterBootRecord))
        {
            DiskPartitionType[DriveNumber] = PARTITION_STYLE_RAW;
            TRACE("Drive 0x%X partition type RAW, no partition table\n", DriveNumber);
            return;
        }

#if 0
        ULONG Index, PartitionCount = 0;
        BOOLEAN GPTProtect = FALSE;
#else
        GPT_TABLE_HEADER GptHeader;
#endif

        DiskPartitionType[DriveNumber] = PARTITION_STYLE_MBR;

#if 0
        /* Check for GUID Partition Table */
        for (Index = 0; Index < 4; Index++)
        {
            PPARTITION_TABLE_ENTRY PartitionTableEntry = &MasterBootRecord.PartitionTable[Index];

            if (PartitionTableEntry->SystemIndicator != PARTITION_ENTRY_UNUSED)
            {
                PartitionCount++;
                if (Index == 0 && PartitionTableEntry->SystemIndicator == PARTITION_GPT)
                    GPTProtect = TRUE;
            }
        }

        if (PartitionCount == 1 && GPTProtect)
#else
        if (DiskReadGptHeader(DriveNumber, &GptHeader))
#endif
        {
            DiskPartitionType[DriveNumber] = PARTITION_STYLE_GPT;
        }
        TRACE("Drive 0x%X partition type %s\n", DriveNumber, DiskPartitionType[DriveNumber] == PARTITION_STYLE_MBR ? "MBR" : "GPT");
        return;
    }

    /* Probe for Xbox-BRFR partitioning */
    if (DiskIsBrfr(DriveNumber))
    {
        DiskPartitionType[DriveNumber] = PARTITION_STYLE_BRFR;
        TRACE("Drive 0x%X partition type Xbox-BRFR\n", DriveNumber);
        return;
    }

    /* Failed to detect partitions, assume non-partitioned disk */
    DiskPartitionType[DriveNumber] = PARTITION_STYLE_RAW;
    TRACE("Drive 0x%X partition type unknown\n", DriveNumber);
}

// FIXME: This function is specific to BIOS-based PC platform.
BOOLEAN
DiskGetBootPartitionEntry(
    _In_ UCHAR DriveNumber,
    _Out_opt_ PPARTITION_INFORMATION PartitionEntry,
    _Out_ PULONG BootPartition)
{
#if 0
    GEOMETRY Geometry;
    if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
        return FALSE;
#endif

    switch (DiskPartitionType[DriveNumber])
    {
        case PARTITION_STYLE_MBR:
        {
            return DiskGetActivePartitionEntry(DriveNumber,
            /* MBR partition table always uses 512-byte sectors per specification */
                                               512, // Geometry.BytesPerSector
                                               PartitionEntry, BootPartition);
        }
        case PARTITION_STYLE_GPT:
        {
            FIXME("DiskGetBootPartitionEntry() unimplemented for GPT\n");
            return FALSE;
        }
        case PARTITION_STYLE_RAW:
        {
            /* One volume, no table: partition 1 is the disk. */
            if (!DiskGetRawPartitionEntry(DriveNumber, 512, 1, PartitionEntry))
                return FALSE;
            *BootPartition = 1;
            return TRUE;
        }
        case PARTITION_STYLE_BRFR:
        {
            PARTITION_INFORMATION TempPartitionEntry;
            if (!PartitionEntry)
                PartitionEntry = &TempPartitionEntry;
            if (DiskGetBrfrPartitionEntry(DriveNumber,
                                          512, // Geometry.BytesPerSector
                                          FATX_DATA_PARTITION,
                                          PartitionEntry))
            {
                *BootPartition = FATX_DATA_PARTITION;
                return TRUE;
            }
            return FALSE;
        }
        default:
        {
            ERR("Drive 0x%X partition type = %d, should not happen!\n", DriveNumber, DiskPartitionType[DriveNumber]);
            ASSERT(FALSE);
        }
    }
    return FALSE;
}

BOOLEAN
DiskGetPartitionEntry(
    _In_ UCHAR DriveNumber,
    _In_opt_ ULONG SectorSize,
    _In_ ULONG PartitionNumber,
    _Out_ PPARTITION_INFORMATION PartitionEntry)
{
    if (SectorSize == 0)
    {
        GEOMETRY Geometry;
        if (!MachDiskGetDriveGeometry(DriveNumber, &Geometry))
            return FALSE;
        SectorSize = Geometry.BytesPerSector;
    }
    if (SectorSize < 512)
    {
        ERR("Drive 0x%X: Invalid sector size %lu\n", DriveNumber, SectorSize);
        return FALSE;
    }

    switch (DiskPartitionType[DriveNumber])
    {
        case PARTITION_STYLE_MBR:
        {
            return DiskGetMbrPartitionEntry(DriveNumber,
            /* MBR partition table always uses 512-byte sectors per specification */
                                            512, // SectorSize
                                            PartitionNumber,
                                            PartitionEntry);
        }
        case PARTITION_STYLE_GPT:
        {
            return DiskGetGptPartitionEntry(DriveNumber,
                                            SectorSize,
                                            PartitionNumber,
                                            PartitionEntry);
        }
        case PARTITION_STYLE_RAW:
        {
            return DiskGetRawPartitionEntry(DriveNumber, SectorSize, PartitionNumber,
                                            PartitionEntry);
        }
        case PARTITION_STYLE_BRFR:
        {
            return DiskGetBrfrPartitionEntry(DriveNumber,
                                             512, // SectorSize
                                             PartitionNumber,
                                             PartitionEntry);
        }
        default:
        {
            ERR("Drive 0x%X partition type = %d, should not happen!\n", DriveNumber, DiskPartitionType[DriveNumber]);
            ASSERT(FALSE);
        }
    }
    return FALSE;
}

#endif
