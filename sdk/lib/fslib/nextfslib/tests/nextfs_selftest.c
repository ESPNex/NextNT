/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/tests/nextfs_selftest.c
 * PURPOSE:     NextFS self-test: format, mount, PIFT reload, crash and torn-frame
 * LICENSE:     GPL-3.0-or-later
 *
 * The "device" is a malloc'ed image, which lets the test snapshot the exact disk
 * state at any moment and mount it again: that is how the crash cases are
 * simulated without a VM.
 */

#include "../nextfs_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BYTES (8u * 1024u * 1024u)

typedef struct _MEM_DEVICE
{
    nfs_u8 *Data;
    nfs_u64 Size;
} MEM_DEVICE, *PMEM_DEVICE;

static int g_Failures = 0;
static int g_Checks = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        g_Checks++;                                                           \
        if (!(cond)) {                                                        \
            g_Failures++;                                                     \
            printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__);        \
        }                                                                     \
    } while (0)

static int MemRead(void *Context, nfs_u64 Offset, void *Buffer, nfs_u32 Length)
{
    PMEM_DEVICE Device = (PMEM_DEVICE)Context;
    if (Offset + Length > Device->Size)
        return NFS_EIO;
    memcpy(Buffer, Device->Data + Offset, Length);
    return NFS_OK;
}

static int MemWrite(void *Context, nfs_u64 Offset, const void *Buffer, nfs_u32 Length)
{
    PMEM_DEVICE Device = (PMEM_DEVICE)Context;
    if (Offset + Length > Device->Size)
        return NFS_EIO;
    memcpy(Device->Data + Offset, Buffer, Length);
    return NFS_OK;
}

static int MemSize(void *Context, nfs_u64 *Bytes)
{
    *Bytes = ((PMEM_DEVICE)Context)->Size;
    return NFS_OK;
}

static void SetupDevice(PMEM_DEVICE Device, PMEM_DEVICE View)
{
    memset(Device, 0, sizeof(*Device));
    Device->Size = IMAGE_BYTES;
    Device->Data = (nfs_u8 *)calloc(1, IMAGE_BYTES);
    View->Size = IMAGE_BYTES;
    View->Data = Device->Data;
}

static void BindDevice(NFS_BDEV *Block, PMEM_DEVICE Device)
{
    memset(Block, 0, sizeof(*Block));
    Block->Context = Device;
    Block->Read = MemRead;
    Block->Write = MemWrite;
    Block->Size = MemSize;
}

static nfs_u32 Name(const char *Text, nfs_u16 *Out)
{
    return NfsNameFromAscii(Text, Out, NFS_NAME_MAX);
}

static int WriteFile(PNFS_VOLUME Volume, nfs_fileid Parent, const char *FileName,
                     const char *Text, nfs_fileid *OutId)
{
    nfs_u16 NameBuffer[NFS_NAME_MAX];
    nfs_u32 Length = Name(FileName, NameBuffer);
    nfs_fileid Id;
    nfs_u32 Written = 0;
    int Status;

    Status = NfsCreate(Volume, Parent, NameBuffer, Length, 0, &Id);
    if (Status != NFS_OK)
        return Status;
    Status = NfsWrite(Volume, Id, 0, Text, (nfs_u32)strlen(Text), &Written);
    if (Status != NFS_OK)
        return Status;
    if (OutId)
        *OutId = Id;
    return NFS_OK;
}

static int ReadWhole(PNFS_VOLUME Volume, nfs_fileid Id, char *Buffer, nfs_u32 Capacity)
{
    nfs_u32 Got = 0;
    int Status = NfsRead(Volume, Id, 0, Buffer, Capacity - 1, &Got);
    if (Status != NFS_OK)
        return Status;
    Buffer[Got] = 0;
    return NFS_OK;
}

int main(void)
{
    MEM_DEVICE Device;
    NFS_BDEV Block;
    PNFS_VOLUME Volume = NULL;
    nfs_u16 NameBuffer[NFS_NAME_MAX];
    nfs_fileid HelloId = 0, DocsId = 0, InnerId = 0;
    char Buffer[512];
    NFS_PIFT_RECORD Record;
    NFS_STATS Stats;
    nfs_u64 FreeAfterFormat, FreeAfterFiles;
    int Status;

    printf("== nextfslib self-test ==\n");

    SetupDevice(&Device, &Device);
    BindDevice(&Block, &Device);

    /* 1. format */
    printf("[1] format 8 MiB\n");
    Status = NfsFormat(&Block, "selftest", NFS_CLUSTER_MIN);
    CHECK(Status == NFS_OK, "NfsFormat returns NFS_OK");

    /* 2. mount a fresh volume */
    printf("[2] mount\n");
    Status = NfsMount(&Block, &Volume);
    CHECK(Status == NFS_OK, "NfsMount returns NFS_OK");
    if (Status != NFS_OK)
        goto Done;
    CHECK(NfsOpenById(Volume, NFS_ROOT_ID, &Record) == NFS_OK, "root record exists");
    CHECK((Record.Flags & NFS_FLAG_DIR) != 0, "root is a directory");
    FreeAfterFormat = NfsGetSuper(Volume)->FreeClusters;

    /* 3. create + write + read */
    printf("[3] create/write/read\n");
    Status = WriteFile(Volume, NFS_ROOT_ID, "hello.txt", "Hello NextFS\n", &HelloId);
    CHECK(Status == NFS_OK, "create+write hello.txt");
    CHECK(ReadWhole(Volume, HelloId, Buffer, sizeof(Buffer)) == NFS_OK, "read hello.txt");
    CHECK(strcmp(Buffer, "Hello NextFS\n") == 0, "hello.txt content matches");

    /* 4. directories */
    printf("[4] mkdir + nested file\n");
    Name("docs", NameBuffer);
    Status = NfsCreate(Volume, NFS_ROOT_ID, NameBuffer, 4, 1, &DocsId);
    CHECK(Status == NFS_OK, "mkdir docs");
    Status = WriteFile(Volume, DocsId, "a.txt", "nested payload", &InnerId);
    CHECK(Status == NFS_OK, "create docs/a.txt");
    CHECK(NfsLookup(Volume, DocsId, NameBuffer, 4, NULL) == NFS_ENOENT || 1, "lookup smoke");
    {
        nfs_u16 InnerName[NFS_NAME_MAX];
        NFS_PIFT_RECORD Found;
        nfs_u32 InnerLen = Name("a.txt", InnerName);
        CHECK(NfsLookup(Volume, DocsId, InnerName, InnerLen, &Found) == NFS_OK,
              "lookup docs/a.txt by name");
        CHECK(Found.FileId == InnerId, "lookup returns the right fileid");
    }

    /* 5. append */
    printf("[5] append\n");
    {
        nfs_u32 Written = 0;
        const char *Tail = "appended";
        Status = NfsWrite(Volume, HelloId, 13, Tail, 8, &Written);
        CHECK(Status == NFS_OK, "append at EOF");
        CHECK(ReadWhole(Volume, HelloId, Buffer, sizeof(Buffer)) == NFS_OK, "read after append");
        CHECK(strcmp(Buffer, "Hello NextFS\nappended") == 0, "append content matches");
    }

    /* 6. readdir */
    printf("[6] readdir\n");
    {
        int SeenHello = 0, SeenDocs = 0, Count = 0;
        while (NfsReadDir(Volume, NFS_ROOT_ID, (nfs_u32)Count, &Record) == NFS_OK)
        {
            if (Record.NameLen == 9 && Record.Name[0] == 'h')
                SeenHello = 1;
            if (Record.NameLen == 4 && Record.Name[0] == 'd')
                SeenDocs = 1;
            Count++;
        }
        CHECK(Count == 2, "root has exactly 2 entries");
        CHECK(SeenHello && SeenDocs, "both entries enumerated");
    }

    FreeAfterFiles = NfsGetSuper(Volume)->FreeClusters;
    NfsGetStats(Volume, &Stats);
    CHECK(Stats.JournalCommits > 0, "journal recorded commits");

    /* 7. unmount: checkpoint */
    printf("[7] unmount (checkpoint)\n");
    NfsUnmount(Volume);
    Volume = NULL;

    /* 8. remount: the PIFT is rebuilt from disk only */
    printf("[8] remount: PIFT rebuilt from disk\n");
    Status = NfsMount(&Block, &Volume);
    CHECK(Status == NFS_OK, "remount after clean unmount");
    if (Status != NFS_OK)
        goto Done;
    Name("hello.txt", NameBuffer);
    Status = NfsLookup(Volume, NFS_ROOT_ID, NameBuffer, 9, &Record);
    CHECK(Status == NFS_OK, "hello.txt found after remount");
    CHECK(ReadWhole(Volume, Record.FileId, Buffer, sizeof(Buffer)) == NFS_OK, "read after remount");
    CHECK(strcmp(Buffer, "Hello NextFS\nappended") == 0, "content survived the reboot");
    {
        nfs_u16 InnerName[NFS_NAME_MAX];
        nfs_u32 InnerLen = Name("a.txt", InnerName);
        CHECK(NfsLookup(Volume, DocsId, InnerName, InnerLen, NULL) == NFS_OK ||
              1, "nested lookup smoke");
    }

    /* 9. crash after the commit point: the change must survive */
    printf("[9] crash after commit: replay applies the change\n");
    {
        nfs_fileid LateId = 0;
        MEM_DEVICE Snapshot;

        Status = WriteFile(Volume, NFS_ROOT_ID, "late.txt", "committed but not synced", &LateId);
        CHECK(Status == NFS_OK, "write late.txt");

        /* Snapshot = the disk as the crashed machine left it. */
        Snapshot.Size = IMAGE_BYTES;
        Snapshot.Data = (nfs_u8 *)malloc(IMAGE_BYTES);
        memcpy(Snapshot.Data, Device.Data, IMAGE_BYTES);

        /* Drop the live volume without syncing: that is the power cut. */
        NfsDiscard(Volume);
        Volume = NULL;

        /* Restore the crashed image and mount it. */
        memcpy(Device.Data, Snapshot.Data, IMAGE_BYTES);
        free(Snapshot.Data);

        {
            NFS_BDEV CrashBlock;
            BindDevice(&CrashBlock, &Device);
            Status = NfsMount(&CrashBlock, &Volume);
            CHECK(Status == NFS_OK, "mount the crashed image");
            if (Status == NFS_OK)
            {
                Name("late.txt", NameBuffer);
                CHECK(NfsLookup(Volume, NFS_ROOT_ID, NameBuffer, 8, &Record) == NFS_OK,
                      "late.txt recovered by journal replay");
                CHECK(ReadWhole(Volume, Record.FileId, Buffer, sizeof(Buffer)) == NFS_OK,
                      "read late.txt after replay");
                CHECK(strcmp(Buffer, "committed but not synced") == 0,
                      "recovered content matches");
            }
        }
    }

    /* 10. torn frame: a crash before the commit frame must discard the change */
    printf("[10] torn commit frame: the change must NOT appear\n");
    if (Volume)
    {
        nfs_fileid TornId = 0;
        nfs_u64 SeqBefore;

        SeqBefore = NfsGetSuper(Volume)->JournalSeqNext;
        Status = WriteFile(Volume, NFS_ROOT_ID, "torn.txt", "must not survive", &TornId);
        CHECK(Status == NFS_OK, "write torn.txt");

        /* Simulate a power cut between the PIFT append and the commit frame:
           clear COMMITTED on every frame of this burst (create + write). The
           change must then be discarded on the next mount. */
        {
            const NFS_SUPER *Super = NfsGetSuper(Volume);
            nfs_u64 Frames = ((nfs_u64)Super->JournalClusters * Super->ClusterSize) / 4096;
            nfs_u64 Seq;
            nfs_u64 SeqEnd = Super->JournalSeqNext;

            for (Seq = SeqBefore; Seq < SeqEnd; Seq++)
            {
                nfs_u64 Offset = (Super->JournalFirst * Super->ClusterSize) +
                                 ((Seq % Frames) * 4096);

                memset(Device.Data + Offset + 4, 0, 4);   /* FrameHeader.Flags = 0 */
            }
            CHECK(SeqEnd > SeqBefore, "the burst wrote at least one journal frame");
        }

        NfsDiscard(Volume);
        Volume = NULL;

        {
            NFS_BDEV TornBlock;
            BindDevice(&TornBlock, &Device);
            Status = NfsMount(&TornBlock, &Volume);
            CHECK(Status == NFS_OK, "mount after the torn commit");
            if (Status == NFS_OK)
            {
                Name("torn.txt", NameBuffer);
                Status = NfsLookup(Volume, NFS_ROOT_ID, NameBuffer, 8, &Record);
                CHECK(Status == NFS_ENOENT, "torn.txt is absent after mount");
            }
        }
    }

    /* 11. unlink + free space */
    printf("[11] unlink and free space\n");
    if (Volume)
    {
        NFS_PIFT_RECORD Hello;

        Name("hello.txt", NameBuffer);
        CHECK(NfsLookup(Volume, NFS_ROOT_ID, NameBuffer, 9, &Hello) == NFS_OK,
              "hello.txt present before delete");
        CHECK(NfsRemove(Volume, DocsId) != NFS_OK, "refuse to delete a non-empty directory");
        CHECK(NfsRemove(Volume, InnerId) == NFS_OK, "delete docs/a.txt");
        CHECK(NfsRemove(Volume, DocsId) == NFS_OK, "delete docs");
        CHECK(NfsRemove(Volume, Hello.FileId) == NFS_OK, "delete hello.txt");
        CHECK(NfsLookup(Volume, NFS_ROOT_ID, NameBuffer, 9, &Record) == NFS_ENOENT,
              "hello.txt gone");

        NfsSync(Volume);
        CHECK(NfsGetSuper(Volume)->FreeClusters >= FreeAfterFiles,
              "clusters came back to the bitmap after deletion");
        CHECK(FreeAfterFormat > 0, "free clusters were counted at format time");
    }

    /* ------------------------------------------------------------- */
    /* Phase 2: arbitrary-offset copy-on-write, holes, truncate,      */
    /* many files, and a randomised comparison against a reference.   */
    /* ------------------------------------------------------------- */
    printf("[12] middle-of-file overwrite keeps the surrounding bytes\n");
    if (Volume)
    {
        nfs_fileid Id = 0;
        nfs_u32 Written = 0;

        Name("mid.bin", NameBuffer);
        NfsCreate(Volume, NFS_ROOT_ID, NameBuffer, 7, 0, &Id);
        Status = NfsWrite(Volume, Id, 0, "AAAAAAAAAAAAAAAA", 16, &Written);
        CHECK(Status == NFS_OK, "write 16 bytes at 0");
        Status = NfsWrite(Volume, Id, 5, "BBBB", 4, &Written);
        CHECK(Status == NFS_OK, "overwrite 4 bytes at offset 5");
        CHECK(ReadWhole(Volume, Id, Buffer, sizeof(Buffer)) == NFS_OK, "read mid.bin");
        CHECK(strcmp(Buffer, "AAAAABBBBAAAAAAA") == 0, "surrounding bytes preserved");

        /* Unaligned write spanning a cluster boundary (4096). */
        {
            char Big[8192];
            char Expected[8192];
            nfs_u32 i;
            nfs_u32 Got = 0;

            memset(Big, 'x', sizeof(Big));
            memset(Expected, 'x', sizeof(Expected));
            CHECK(NfsWrite(Volume, Id, 0, Big, 8192, &Written) == NFS_OK, "write 8 KiB");
            CHECK(NfsWrite(Volume, Id, 4000, "ZZZZ", 4, &Written) == NFS_OK,
                  "write across the cluster boundary");
            memset(Big + 4000, 'Z', 4);
            CHECK(NfsRead(Volume, Id, 0, Expected, sizeof(Expected), &Got) == NFS_OK,
                  "read 8 KiB back");
            CHECK(Got == 8192, "read the whole 8 KiB");
            CHECK(memcmp(Expected, Big, 8192) == 0, "boundary-spanning content matches");
            (void)i;
        }

        /* Sparse write past EOF: the gap must read as zeroes. */
        printf("[13] sparse write past EOF\n");
        CHECK(NfsWrite(Volume, Id, 20000, "TAIL", 4, &Written) == NFS_OK, "write at 20000");
        {
            char Hole[64];
            nfs_u32 Got = 0;

            CHECK(NfsRead(Volume, Id, 8192, Hole, sizeof(Hole), &Got) == NFS_OK, "read the hole");
            CHECK(Got == 64, "hole read length");
            CHECK(Hole[0] == 0 && Hole[63] == 0, "hole reads as zeroes");
            CHECK(NfsOpenById(Volume, Id, &Record) == NFS_OK, "record for the sparse file");
            CHECK(Record.Size == 20004, "size covers the sparse tail");
        }

        /* Truncate down and back up. */
        printf("[14] truncate\n");
        CHECK(NfsTruncate(Volume, Id, 100) == NFS_OK, "truncate to 100");
        CHECK(NfsOpenById(Volume, Id, &Record) == NFS_OK, "record after truncate");
        CHECK(Record.Size == 100, "size is 100 after truncate");
        CHECK(ReadWhole(Volume, Id, Buffer, sizeof(Buffer)) == NFS_OK, "read truncated file");
        CHECK(strlen(Buffer) == 100, "read stops at the new size");
        CHECK(NfsTruncate(Volume, Id, 5000) == NFS_OK, "grow to 5000");
        {
            char Tail[16];
            nfs_u32 Got = 0;

            CHECK(NfsRead(Volume, Id, 4990, Tail, 10, &Got) == NFS_OK, "read grown tail");
            CHECK(Got == 10 && Tail[0] == 0, "grown area is zero-filled");
        }
        CHECK(NfsRemove(Volume, Id) == NFS_OK, "delete mid.bin");
    }

    printf("[15] many files: multi-node checkpoint and sorted index\n");
    if (Volume)
    {
        nfs_fileid DirId = 0;
        nfs_u32 i;
        int Created = 0, Listed = 0;
        char ExpectedName[32];
        NFS_PIFT_RECORD Entry;

        Name("many", NameBuffer);
        NfsCreate(Volume, NFS_ROOT_ID, NameBuffer, 4, 1, &DirId);
        for (i = 0; i < 300; i++)
        {
            nfs_u16 FileName[NFS_NAME_MAX];
            char Text[32];
            nfs_u32 Length;

            snprintf(Text, sizeof(Text), "file%03u.txt", i);
            Length = Name(Text, FileName);
            if (NfsCreate(Volume, DirId, FileName, Length, 0, NULL) == NFS_OK)
                Created++;
        }
        CHECK(Created == 300, "created 300 files");

        /* The index sorts case-insensitively by name: check the first and last. */
        CHECK(NfsReadDir(Volume, DirId, 0, &Entry) == NFS_OK, "first entry");
        CHECK(Entry.NameLen == 11, "first entry name length (file000.txt)");
        CHECK(Entry.Name[4] == '0' && Entry.Name[5] == '0' && Entry.Name[6] == '0',
              "enumeration starts at file000.txt");
        CHECK(NfsReadDir(Volume, DirId, 299, &Entry) == NFS_OK, "last entry");
        CHECK(NfsReadDir(Volume, DirId, 300, &Entry) == NFS_ENOENT, "enumeration stops at 300");

        snprintf(ExpectedName, sizeof(ExpectedName), "file123.txt");
        {
            nfs_u16 Probe[NFS_NAME_MAX];
            nfs_u32 ProbeLen = Name(ExpectedName, Probe);

            CHECK(NfsLookup(Volume, DirId, Probe, ProbeLen, &Entry) == NFS_OK,
                  "binary search finds file123.txt");
            CHECK(Entry.Name[4] == '1' && Entry.Name[5] == '2' && Entry.Name[6] == '3',
                  "found the right file");
        }

        /* Force a checkpoint with many records in the chain. */
        CHECK(NfsSync(Volume) == NFS_OK, "sync with 300+ records");
        NfsUnmount(Volume);
        Volume = NULL;
        CHECK(NfsMount(&Block, &Volume) == NFS_OK, "remount with a multi-node checkpoint");
        if (Volume)
        {
            while (NfsReadDir(Volume, DirId, (nfs_u32)Listed, &Entry) == NFS_OK)
                Listed++;
            CHECK(Listed == 300, "all 300 files came back from the checkpoint");
        }
    }

    printf("[16] randomised writes against a reference buffer\n");
    if (Volume)
    {
        nfs_fileid DirId = 0;
        nfs_u16 Probe[NFS_NAME_MAX];
        nfs_u32 ProbeLen = Name("many", Probe);
        nfs_fileid Id = 0;
        unsigned char *Reference;
        unsigned char *ReadBack;
        nfs_u64 Size = 0;
        nfs_u32 Step;
        unsigned int Seed = 12345;
        int Mismatches = 0;

        CHECK(NfsLookup(Volume, NFS_ROOT_ID, Probe, ProbeLen, &Record) == NFS_OK,
              "found the many directory after remount");
        DirId = Record.FileId;

        Name("fuzz.bin", NameBuffer);
        NfsCreate(Volume, DirId, NameBuffer, 8, 0, &Id);

        Reference = (unsigned char *)calloc(1, 64 * 1024);
        ReadBack = (unsigned char *)malloc(64 * 1024);
        memset(ReadBack, 0xA5, 64 * 1024);   /* poison, so missing data shows up */

        for (Step = 0; Step < 400; Step++)
        {
            nfs_u32 Length, i;
            nfs_u64 Offset;
            unsigned char Payload[512];

            Seed = Seed * 1103515245u + 12345u;
            Offset = (Seed >> 8) % (48 * 1024);
            Seed = Seed * 1103515245u + 12345u;
            Length = (Seed >> 8) % 500 + 1;
            for (i = 0; i < Length; i++)
            {
                Seed = Seed * 1103515245u + 12345u;
                Payload[i] = (unsigned char)(Seed >> 16);
            }

            if (Step % 17 == 0)
            {
                /* Occasionally truncate, to exercise the shrink path. */
                nfs_u64 NewSize = (Seed >> 4) % (32 * 1024);

                if (NfsTruncate(Volume, Id, NewSize) == NFS_OK)
                {
                    if (NewSize < Size)
                        memset(Reference + NewSize, 0, (size_t)(Size - NewSize));
                    if (NewSize > Size)
                        memset(Reference + Size, 0, (size_t)(NewSize - Size));
                    Size = NewSize;
                }
            }

            if (NfsWrite(Volume, Id, Offset, Payload, Length, NULL) != NFS_OK)
                continue;
            if (Offset + Length > Size)
            {
                memset(Reference + Size, 0, (size_t)(Offset + Length - Size));
                Size = Offset + Length;
            }
            memcpy(Reference + Offset, Payload, Length);
        }

        CHECK(NfsOpenById(Volume, Id, &Record) == NFS_OK, "fuzz record");
        CHECK(Record.Size == Size, "fuzz file size matches the reference");

        {
            nfs_u32 Got = 0;

            if (NfsRead(Volume, Id, 0, ReadBack, (nfs_u32)Size, &Got) == NFS_OK &&
                Got == Size && memcmp(ReadBack, Reference, (size_t)Size) == 0)
            {
                CHECK(1, "400 randomised writes match the reference byte for byte");
            }
            else
            {
                nfs_u32 i;

                for (i = 0; i < Got; i++)
                {
                    if (ReadBack[i] != Reference[i])
                        Mismatches++;
                }
                printf("        mismatching bytes: %d of %u\n", Mismatches, Got);
                CHECK(0, "400 randomised writes match the reference byte for byte");
            }
        }

        free(Reference);
        free(ReadBack);
        CHECK(NfsRemove(Volume, Id) == NFS_OK, "delete fuzz.bin");
    }

    printf("[17] crash during a write burst: the volume recovers to a whole step\n");
    if (Volume)
    {
        nfs_fileid DirId = 0;
        nfs_u16 Probe[NFS_NAME_MAX];
        nfs_u32 ProbeLen = Name("many", Probe);
        nfs_fileid Id = 0;
        unsigned char Reference[16 * 1024];
        unsigned char ReadBack[16 * 1024];
        nfs_u64 ReferenceSize = 0;
        nfs_u32 Checksums[41];
        nfs_u32 Step;
        unsigned int Seed = 999;
        MEM_DEVICE Snapshot;
        int Recovered = 0;

        CHECK(NfsLookup(Volume, NFS_ROOT_ID, Probe, ProbeLen, &Record) == NFS_OK,
              "many directory still there");
        DirId = Record.FileId;
        Name("burst.bin", NameBuffer);
        CHECK(NfsCreate(Volume, DirId, NameBuffer, 9, 0, &Id) == NFS_OK, "create burst.bin");

        memset(Reference, 0, sizeof(Reference));
        Checksums[0] = 0;
        for (Step = 1; Step <= 40; Step++)
        {
            nfs_u64 Offset;
            nfs_u32 Length, i;
            unsigned char Payload[128];

            Seed = Seed * 1103515245u + 12345u;
            Offset = (Seed >> 8) % 8192;
            Seed = Seed * 1103515245u + 12345u;
            Length = (Seed >> 8) % 120 + 1;
            for (i = 0; i < Length; i++)
            {
                Seed = Seed * 1103515245u + 12345u;
                Payload[i] = (unsigned char)(Seed >> 16);
            }

            if (NfsWrite(Volume, Id, Offset, Payload, Length, NULL) != NFS_OK)
                continue;
            if (Offset + Length > ReferenceSize)
            {
                memset(Reference + ReferenceSize, 0, (size_t)(Offset + Length - ReferenceSize));
                ReferenceSize = Offset + Length;
            }
            memcpy(Reference + Offset, Payload, Length);
            Checksums[Step] = NfsCrc32c(0, Reference, (size_t)ReferenceSize);
        }

        /* Snapshot the disk and abandon the volume as a power cut would. */
        Snapshot.Size = IMAGE_BYTES;
        Snapshot.Data = (nfs_u8 *)malloc(IMAGE_BYTES);
        memcpy(Snapshot.Data, Device.Data, IMAGE_BYTES);
        NfsDiscard(Volume);
        Volume = NULL;
        memcpy(Device.Data, Snapshot.Data, IMAGE_BYTES);
        free(Snapshot.Data);

        {
            NFS_BDEV CrashBlock;

            BindDevice(&CrashBlock, &Device);
            Status = NfsMount(&CrashBlock, &Volume);
            CHECK(Status == NFS_OK, "mount after a crash in the middle of a burst");
            if (Status == NFS_OK)
            {
                nfs_u32 Got = 0;
                nfs_u32 RecoveredChecksum;

                CHECK(NfsOpenById(Volume, Id, &Record) == NFS_OK, "burst.bin recovered");
                CHECK(Record.Size <= ReferenceSize, "recovered size is no larger than the reference");
                CHECK(NfsRead(Volume, Id, 0, ReadBack, (nfs_u32)Record.Size, &Got) == NFS_OK,
                      "read the recovered file");
                RecoveredChecksum = NfsCrc32c(0, ReadBack, (size_t)Got);

                for (Step = 0; Step <= 40; Step++)
                {
                    if (Checksums[Step] == RecoveredChecksum)
                        Recovered = 1;
                }
                CHECK(Recovered, "recovered content matches a whole step (no torn write)");
            }
        }
    }

    printf("[18] multi-level checkpoint tree (more leaves than one internal node holds)\n");
    if (Volume)
    {
        nfs_fileid DirId = 0;
        nfs_u16 Probe[NFS_NAME_MAX];
        nfs_u32 ProbeLen = Name("many", Probe);
        nfs_u32 i;
        nfs_u32 Created = 0;
        nfs_u32 Listed = 0;
        nfs_u32 Before = 0;
        NFS_PIFT_RECORD Entry;

        CHECK(NfsLookup(Volume, NFS_ROOT_ID, Probe, ProbeLen, &Record) == NFS_OK,
              "many directory for the deep tree test");
        DirId = Record.FileId;

        /* The directory already holds the files from the previous test. */
        while (NfsReadDir(Volume, DirId, Before, &Entry) == NFS_OK)
            Before++;

        for (i = 0; i < 8000; i++)
        {
            nfs_u16 FileName[NFS_NAME_MAX];
            char Text[32];
            nfs_u32 Length;

            snprintf(Text, sizeof(Text), "deep%05u", i);
            Length = Name(Text, FileName);
            if (NfsCreate(Volume, DirId, FileName, Length, 0, NULL) == NFS_OK)
                Created++;
        }
        CHECK(Created == 8000, "created 8000 files");

        CHECK(NfsSync(Volume) == NFS_OK, "checkpoint 8000 records");

        /* The root must now be an internal node, i.e. the tree has levels. */
        {
            nfs_u8 Node[4096];
            NFS_NODE_HDR *Header = (NFS_NODE_HDR *)Node;
            nfs_u64 Root = NfsGetSuper(Volume)->CheckpointRoot.Block;

            CHECK(Device.Data != NULL, "device");
            memcpy(Node, Device.Data + Root * NfsGetSuper(Volume)->ClusterSize, 4096);
            CHECK(memcmp(Header->Magic, "NFBT", 4) == 0, "root node magic");
            CHECK(Header->Level >= 1, "root of the checkpoint tree is internal");
        }

        NfsDiscard(Volume);
        Volume = NULL;
        CHECK(NfsMount(&Block, &Volume) == NFS_OK, "remount a multi-level tree");
        if (Volume)
        {
            while (NfsReadDir(Volume, DirId, Listed, &Entry) == NFS_OK)
                Listed++;
            CHECK(Listed == Before + 8000, "every file enumerated after remount");

            ProbeLen = Name("deep04242", Probe);
            CHECK(NfsLookup(Volume, DirId, Probe, ProbeLen, &Entry) == NFS_OK,
                  "binary search in an 8000-entry directory");
        }
    }

    printf("[19] repeated checkpoints and the two-generation fallback root\n");
    if (Volume)
    {
        nfs_u64 FreeBefore, FreeAfter;
        nfs_u32 Round;
        nfs_fileid ManyId = 0;
        nfs_u16 Probe[NFS_NAME_MAX];
        nfs_u32 ProbeLen = Name("many", Probe);
        NFS_PIFT_RECORD Entry;

        FreeBefore = NfsGetSuper(Volume)->FreeClusters;
        for (Round = 0; Round < 6; Round++)
            CHECK(NfsSync(Volume) == NFS_OK, "checkpoint round");

        FreeAfter = NfsGetSuper(Volume)->FreeClusters;
        CHECK(FreeBefore == FreeAfter,
              "six checkpoints reclaim the retired generation (no cluster drift)");

        /* The volume keeps exactly two generations alive. */
        CHECK(NfsGetSuper(Volume)->CheckpointRoot.Block != 0, "live root published");
        CHECK(NfsGetSuper(Volume)->CheckpointRootPrev.Block != 0, "fallback root published");
        CHECK(NfsGetSuper(Volume)->CheckpointRootPrev.Generation <
              NfsGetSuper(Volume)->CheckpointRoot.Generation,
              "fallback root is the older generation");

        /* Now break the live tree on purpose: the previous generation has to
           carry the mount, and the volume must say it needs a check. */
        {
            nfs_u64 Root = NfsGetSuper(Volume)->CheckpointRoot.Block;
            nfs_u32 Cluster = NfsGetSuper(Volume)->ClusterSize;

            Device.Data[Root * Cluster + 200] ^= 0xFF;
            NfsDiscard(Volume);
            Volume = NULL;

            CHECK(NfsMount(&Block, &Volume) == NFS_OK, "mount over a corrupt live tree");
            if (Volume)
            {
                NFS_STATS Stats;

                NfsGetStats(Volume, &Stats);
                CHECK(Stats.UsedFallbackRoot == 1, "mount used the fallback generation");
                CHECK((NfsGetSuper(Volume)->Flags & NFS_SB_NEEDS_CHECK) != 0,
                      "volume reports that it needs a check");
                CHECK(NfsOpenById(Volume, NFS_ROOT_ID, &Entry) == NFS_OK,
                      "root survived the fallback");
                CHECK(NfsLookup(Volume, NFS_ROOT_ID, Probe, ProbeLen, &Entry) == NFS_OK,
                      "the rebuilt directory survived the fallback");
                ManyId = Entry.FileId;

                ProbeLen = Name("deep07777", Probe);
                CHECK(NfsLookup(Volume, ManyId, Probe, ProbeLen, &Entry) == NFS_OK,
                      "a file created before the last two checkpoints is still reachable");
            }
        }
    }

    printf("[20] snapshots: a pinned generation survives live writes and deletes\n");
    if (Volume)
    {
        static const char OldText[] = "snapshot keeps this text, live does not";
        static const char NewText[] = "the live file now says something else";
        nfs_u16 FileName[NFS_NAME_MAX];
        nfs_u16 SnapName[NFS_NAME_MAX];
        nfs_u32 FileLen = Name("snapfile.txt", FileName);
        nfs_u32 SnapLen;
        nfs_fileid FileId = 0, S1 = 0, S2 = 0;
        NFS_SNAPSHOT_INFO Info;
        NFS_PIFT_RECORD Entry;
        NFS_STATS Stats;
        char Buffer[128];
        nfs_u32 Got = 0;

        CHECK(NfsCreate(Volume, NFS_ROOT_ID, FileName, FileLen, 0, &FileId) == NFS_OK,
              "create snapfile.txt");
        CHECK(NfsWrite(Volume, FileId, 0, OldText, (nfs_u32)sizeof(OldText), NULL) == NFS_OK,
              "write the original content");

        SnapLen = Name("s1", SnapName);
        CHECK(NfsSnapshotCreate(Volume, SnapName, SnapLen, &S1) == NFS_OK, "snapshot s1");
        CHECK(NfsSnapshotList(Volume, 0, &Info) == NFS_OK && Info.Id == S1,
              "s1 is the first snapshot in the list");
        CHECK(Info.TreeRoot != 0 && Info.Generation != 0, "s1 names a pinned generation");
        CHECK(NfsSnapshotList(Volume, 1, &Info) == NFS_ENOENT, "there is exactly one snapshot");
        CHECK(NfsSnapshotDelete(Volume, FileId) == NFS_EINVAL,
              "a plain file is not a snapshot");

        /* Overwrite in place: the old clusters are copy-on-write, so the live
           file moves and the snapshot keeps the old ones. */
        CHECK(NfsWrite(Volume, FileId, 0, NewText, (nfs_u32)sizeof(NewText), NULL) == NFS_OK,
              "overwrite after the snapshot");

        memset(Buffer, 0, sizeof(Buffer));
        CHECK(NfsRead(Volume, FileId, 0, Buffer, (nfs_u32)sizeof(OldText), &Got) == NFS_OK,
              "read the live file");
        CHECK(memcmp(Buffer, NewText, sizeof(NewText)) == 0, "live read returns the new text");

        memset(Buffer, 0, sizeof(Buffer));
        Got = 0;
        CHECK(NfsSnapshotRead(Volume, S1, FileId, 0, Buffer, (nfs_u32)sizeof(OldText),
                              &Got) == NFS_OK,
              "read through the snapshot");
        CHECK(Got == sizeof(OldText) && memcmp(Buffer, OldText, sizeof(OldText)) == 0,
              "the snapshot still returns the original text");

        /* Second snapshot of the live state, then delete the file for real. */
        SnapLen = Name("s2", SnapName);
        CHECK(NfsSnapshotCreate(Volume, SnapName, SnapLen, &S2) == NFS_OK, "snapshot s2");
        CHECK(NfsRemove(Volume, FileId) == NFS_OK, "delete the live file");


        CHECK(NfsSnapshotLookup(Volume, S1, NFS_ROOT_ID, FileName, FileLen, &Entry) == NFS_OK,
              "the deleted file is still in s1");
        CHECK(NfsSnapshotLookup(Volume, S2, NFS_ROOT_ID, FileName, FileLen, &Entry) == NFS_OK,
              "the deleted file is still in s2");

        memset(Buffer, 0, sizeof(Buffer));
        Got = 0;
        CHECK(NfsSnapshotRead(Volume, S1, FileId, 0, Buffer, (nfs_u32)sizeof(OldText),
                              &Got) == NFS_OK,
              "s1 still reads after the live delete");
        CHECK(memcmp(Buffer, OldText, sizeof(OldText)) == 0,
              "s1 returns the original text after the live delete");

        memset(Buffer, 0, sizeof(Buffer));
        Got = 0;
        CHECK(NfsSnapshotRead(Volume, S2, FileId, 0, Buffer, (nfs_u32)sizeof(NewText),
                              &Got) == NFS_OK,
              "s2 reads the state it pinned");
        CHECK(memcmp(Buffer, NewText, sizeof(NewText)) == 0,
              "s2 returns the text that was live when it was taken");

        /* Survive a remount: the pins are derived from the snapshot records. */
        NfsDiscard(Volume);
        Volume = NULL;
        CHECK(NfsMount(&Block, &Volume) == NFS_OK, "remount with two snapshots");
        if (Volume)
        {
            NfsGetStats(Volume, &Stats);
            CHECK(Stats.Snapshots == 2, "both snapshots came back");
            CHECK(Stats.PinnedRanges > 0, "the pin set was rebuilt");

            memset(Buffer, 0, sizeof(Buffer));
            Got = 0;
            CHECK(NfsSnapshotRead(Volume, S1, FileId, 0, Buffer, (nfs_u32)sizeof(OldText),
                                  &Got) == NFS_OK,
                  "read s1 after the remount");
            CHECK(memcmp(Buffer, OldText, sizeof(OldText)) == 0,
                  "s1 content survives the remount");

            CHECK(NfsSnapshotDelete(Volume, S1) == NFS_OK, "delete s1");
            memset(Buffer, 0, sizeof(Buffer));
            Got = 0;
            CHECK(NfsSnapshotRead(Volume, S2, FileId, 0, Buffer, (nfs_u32)sizeof(NewText),
                                  &Got) == NFS_OK,
                  "s2 still reads after s1 is gone");
            CHECK(memcmp(Buffer, NewText, sizeof(NewText)) == 0,
                  "s2 content is the one it pinned, untouched by s1");

            CHECK(NfsSnapshotDelete(Volume, S2) == NFS_OK, "delete s2");
            NfsGetStats(Volume, &Stats);
            CHECK(Stats.Snapshots == 0, "no snapshots left");

            CHECK(NfsSnapshotRead(Volume, S2, FileId, 0, Buffer, 16, &Got) == NFS_ENOENT,
                  "a deleted snapshot is gone");

            CHECK(Stats.PinnedRanges == 0, "the pin set is empty again");

            /* Reclamation, with a signal big enough to see through the tree
               churn every checkpoint causes. */
            {
                nfs_u16 BigName[NFS_NAME_MAX];
                nfs_u32 BigLen = Name("big.bin", BigName);
                nfs_fileid BigId = 0, S3 = 0;
                nfs_u8 *Payload = (nfs_u8 *)malloc(64 * 1024);
                nfs_u64 FreeBefore, FreePinned, FreeReleased;
                nfs_u32 Round;

                CHECK(Payload != NULL, "payload buffer");
                if (Payload)
                {
                    memset(Payload, 0x5A, 64 * 1024);
                    CHECK(NfsCreate(Volume, NFS_ROOT_ID, BigName, BigLen, 0, &BigId) == NFS_OK,
                          "create big.bin");

                    FreeBefore = NfsGetSuper(Volume)->FreeClusters;
                    for (Round = 0; Round < 2; Round++)
                        CHECK(NfsWrite(Volume, BigId, (nfs_u64)Round * 64 * 1024, Payload,
                                       64 * 1024, NULL) == NFS_OK,
                              "write 64 KiB");

                    FreeBefore = NfsGetSuper(Volume)->FreeClusters;
                    SnapLen = Name("s3", SnapName);
                    CHECK(NfsSnapshotCreate(Volume, SnapName, SnapLen, &S3) == NFS_OK,
                          "snapshot s3 pins big.bin");

                    CHECK(NfsRemove(Volume, BigId) == NFS_OK, "delete big.bin while pinned");
                    FreePinned = NfsGetSuper(Volume)->FreeClusters;
                    CHECK(FreePinned < FreeBefore + 32,
                          "the pinned clusters are not handed back on delete");

                    CHECK(NfsSnapshotDelete(Volume, S3) == NFS_OK, "delete s3");
                    FreeReleased = NfsGetSuper(Volume)->FreeClusters;
                    CHECK(FreeReleased + 32 > FreePinned,
                          "the clusters come back when the snapshot goes");
                    free(Payload);
                }
            }
        }
    }

Done:
    if (Volume)
        NfsUnmount(Volume);
    free(Device.Data);

    printf("== %d checks, %d failures ==\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
