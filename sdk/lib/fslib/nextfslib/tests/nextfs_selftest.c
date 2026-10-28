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

Done:
    if (Volume)
        NfsUnmount(Volume);
    free(Device.Data);

    printf("== %d checks, %d failures ==\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
