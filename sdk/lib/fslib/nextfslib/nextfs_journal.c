/*
 * PROJECT:     NextNT
 * FILE:        sdk/lib/fslib/nextfslib/nextfs_journal.c
 * PURPOSE:     NextFS journal (WAL) frames and the PIFT-on-disk append area
 * LICENSE:     GPL-3.0-or-later
 *
 * Commit protocol (see DESIGN.md 2.2):
 *   1. write the frame with COMMITTED clear, flush
 *   2. append the same payload to the PIFT-on-disk area, flush
 *   3. set COMMITTED on the frame, flush          <-- commit point
 *   4. apply to RAM, then checkpoint asynchronously
 */

#include "nextfs_internal.h"

#include <string.h>
#include <stdlib.h>

void *NfsMemAlloc(size_t Size);
void NfsMemFree(void *Block);

nfs_u64
NfsJournalNextSeq(PNFS_VOLUME Volume)
{
    return Volume->Super.JournalSeqNext;
}

static nfs_u64
NfsJournalFrameOffset(PNFS_VOLUME Volume, nfs_seq Seq)
{
    nfs_u64 Frames = Volume->JournalFrames;
    nfs_u64 Slot = Frames ? (Seq % Frames) : 0;

    return (Volume->Super.JournalFirst * Volume->Super.ClusterSize) +
           (Slot * NFS_FRAME_SIZE);
}

int
NfsJournalWriteFrame(PNFS_VOLUME Volume, nfs_seq Seq, nfs_u32 Flags,
                     const void *Payload, nfs_u32 PayloadLen)
{
    nfs_u8 Frame[NFS_FRAME_SIZE];
    NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
    int Status;

    if (PayloadLen > NFS_FRAME_PAYLOAD_MAX)
        return NFS_EINVAL;

    memset(Frame, 0, sizeof(Frame));
    memcpy(Header->Magic, NFS_FRAME_MAGIC, 4);
    Header->Flags = Flags;
    Header->Seq = Seq;
    Header->PayloadLen = PayloadLen;
    if (PayloadLen)
        memcpy(Frame + NFS_FRAME_HDR_SIZE, Payload, PayloadLen);
    Header->Crc32c = NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, PayloadLen);

    Status = NfsDevWrite(Volume, NfsJournalFrameOffset(Volume, Seq), Frame, NFS_FRAME_SIZE);
    if (Status != NFS_OK)
        return Status;
    return NfsDevFlush(Volume);
}

int
NfsJournalCommit(PNFS_VOLUME Volume, nfs_seq Seq)
{
    nfs_u8 Frame[NFS_FRAME_SIZE];
    NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
    nfs_u64 Offset = NfsJournalFrameOffset(Volume, Seq);
    int Status;

    Status = NfsDevRead(Volume, Offset, Frame, NFS_FRAME_SIZE);
    if (Status != NFS_OK)
        return Status;
    if (memcmp(Header->Magic, NFS_FRAME_MAGIC, 4) != 0 || Header->Seq != Seq)
        return NFS_ECORRUPT;
    if (Header->Crc32c != NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, Header->PayloadLen))
        return NFS_ECORRUPT;

    Header->Flags |= NFS_FRAME_COMMITTED;
    Status = NfsDevWrite(Volume, Offset, Frame, NFS_FRAME_SIZE);
    if (Status != NFS_OK)
        return Status;
    return NfsDevFlush(Volume);
}

/* ------------------------------------------------------------------ */
/* PIFT-on-disk append area                                            */
/* ------------------------------------------------------------------ */

static nfs_u64
NfsPiftAreaBytes(PNFS_VOLUME Volume)
{
    return (nfs_u64)Volume->Super.PiftClusters * Volume->Super.ClusterSize;
}

int
NfsPiftAppend(PNFS_VOLUME Volume, const void *Frames, nfs_u32 Length)
{
    nfs_u64 Offset = (Volume->Super.PiftFirst * Volume->Super.ClusterSize) +
                     Volume->Super.PiftHead;
    int Status;

    if (Volume->Super.PiftHead + Length > NfsPiftAreaBytes(Volume))
        return NFS_ENOSPC;

    Status = NfsDevWrite(Volume, Offset, Frames, Length);
    if (Status != NFS_OK)
        return Status;
    Status = NfsDevFlush(Volume);
    if (Status != NFS_OK)
        return Status;

    Volume->Super.PiftHead += Length;
    return NFS_OK;
}

/*
 * Scan the whole journal ring and report every sequence number whose frame is
 * CRC-valid and marked COMMITTED. A PIFT-on-disk record is only trusted if its
 * sequence is in this set (or older than the live journal window), which is what
 * makes "crash before the commit frame" discard the change.
 */
int
NfsJournalScanCommitted(PNFS_VOLUME Volume, nfs_u64 *Seqs, nfs_u32 Max,
                        nfs_u32 *Count)
{
    nfs_u64 Frames = Volume->JournalFrames;
    nfs_u64 Slot;
    nfs_u8 Frame[NFS_FRAME_SIZE];
    nfs_u32 Found = 0;

    *Count = 0;
    if (Frames == 0)
        return NFS_OK;

    for (Slot = 0; Slot < Frames; Slot++)
    {
        NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
        nfs_u64 Offset = (Volume->Super.JournalFirst * Volume->Super.ClusterSize) +
                         (Slot * NFS_FRAME_SIZE);

        if (NfsDevRead(Volume, Offset, Frame, NFS_FRAME_SIZE) != NFS_OK)
            break;
        if (memcmp(Header->Magic, NFS_FRAME_MAGIC, 4) != 0)
            continue;
        if (!(Header->Flags & NFS_FRAME_COMMITTED))
            continue;
        if (Header->PayloadLen > NFS_FRAME_PAYLOAD_MAX)
            continue;
        if (Header->Crc32c != NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, Header->PayloadLen))
            continue;

        if (Found < Max)
            Seqs[Found] = Header->Seq;
        Found++;
    }

    *Count = Found;
    return NFS_OK;
}

int
NfsSeqIsCommitted(PNFS_VOLUME Volume, nfs_seq Seq)
{
    nfs_u64 Frames = Volume->JournalFrames;
    nfs_u64 Slot;

    /* Older than the live window: it was committed before the last checkpoint. */
    if (Seq < Volume->Super.JournalSeqFirst)
        return 1;

    for (Slot = 0; Slot < Frames; Slot++)
    {
        nfs_u8 Frame[NFS_FRAME_SIZE];
        NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
        nfs_u64 Offset = (Volume->Super.JournalFirst * Volume->Super.ClusterSize) +
                         (Slot * NFS_FRAME_SIZE);

        if (NfsDevRead(Volume, Offset, Frame, NFS_FRAME_SIZE) != NFS_OK)
            return 0;
        if (memcmp(Header->Magic, NFS_FRAME_MAGIC, 4) != 0)
            continue;
        if (Header->Seq != Seq)
            continue;
        if (!(Header->Flags & NFS_FRAME_COMMITTED))
            return 0;
        if (Header->Crc32c != NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, Header->PayloadLen))
            return 0;
        return 1;
    }
    return 0;
}

/*
 * Walk the PIFT area and apply every record that is newer than what we have.
 * Highest Seq per FileId wins, which makes the walk idempotent and replay-safe.
 */
int
NfsPiftScan(PNFS_VOLUME Volume)
{
    nfs_u64 Base = Volume->Super.PiftFirst * Volume->Super.ClusterSize;
    nfs_u64 Offset = Base;
    nfs_u64 End = Offset + Volume->Super.PiftHead;
    nfs_u64 ValidEnd = Offset;
    nfs_u8 Frame[NFS_FRAME_SIZE];
    int Status;

    while (Offset + NFS_FRAME_SIZE <= End)
    {
        NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
        NFS_PIFT_RECORD Record;
        NFS_PIFT_RECORD *Existing;

        Status = NfsDevRead(Volume, Offset, Frame, NFS_FRAME_SIZE);
        if (Status != NFS_OK)
            return Status;

        if (memcmp(Header->Magic, NFS_FRAME_MAGIC, 4) != 0 ||
            Header->PayloadLen > NFS_FRAME_PAYLOAD_MAX)
            break;   /* torn tail: everything after it is garbage */

        if (Header->Crc32c != NfsCrc32c(0, Frame + NFS_FRAME_HDR_SIZE, Header->PayloadLen))
            break;

        if (NfsPiftDeserialize(Frame + NFS_FRAME_HDR_SIZE, Header->PayloadLen, &Record) == NFS_OK &&
            NfsSeqIsCommitted(Volume, Header->Seq))
        {
            Existing = NfsIndexGet(Volume, Record.FileId);
            if (!Existing || Existing->Seq <= Record.Seq)
            {
                if (NfsIndexInsert(Volume, &Record) != NFS_OK)
                    return NFS_ENOSPC;
            }
        }

        Offset += NFS_FRAME_SIZE;
        ValidEnd = Offset;
    }

    /* Where the valid records end is where the next append goes. A fallback
       mount reads the whole half to find its records, and this puts the append
       pointer back to the real tail. */
    Volume->Super.PiftHead = ValidEnd - Base;
    return NFS_OK;
}

/* ------------------------------------------------------------------ */
/* Replay of committed journal frames                                  */
/* ------------------------------------------------------------------ */

/*
 * In-place heapsort of the committed sequence list. nextfs_journal.c is compiled
 * into the FSD too, and ntoskrnl exports no qsort, so the sort lives here. The
 * order is total and stable enough for replay: frames are applied oldest first.
 */
static void
NfsSeqSiftDown(nfs_u64 *Seqs, nfs_u32 Start, nfs_u32 Count)
{
    nfs_u32 Root = Start;

    for (;;)
    {
        nfs_u32 Child = Root * 2 + 1;
        nfs_u32 Swap = Root;
        nfs_u64 Temp;

        if (Child < Count && Seqs[Swap] < Seqs[Child])
            Swap = Child;
        if (Child + 1 < Count && Seqs[Swap] < Seqs[Child + 1])
            Swap = Child + 1;
        if (Swap == Root)
            return;

        Temp = Seqs[Root];
        Seqs[Root] = Seqs[Swap];
        Seqs[Swap] = Temp;
        Root = Swap;
    }
}

static void
NfsSeqSort(nfs_u64 *Seqs, nfs_u32 Count)
{
    nfs_u32 Start, End;
    nfs_u64 Temp;

    if (Count < 2)
        return;

    for (Start = Count / 2; Start > 0; Start--)
        NfsSeqSiftDown(Seqs, Start - 1, Count);

    for (End = Count - 1; End > 0; End--)
    {
        Temp = Seqs[0];
        Seqs[0] = Seqs[End];
        Seqs[End] = Temp;
        NfsSeqSiftDown(Seqs, 0, End);
    }
}

/*
 * Replay is driven by the committed frames actually present in the ring, not by
 * the [JournalSeqFirst, JournalSeqNext) window: the superblock only stores those
 * two counters at checkpoint time, so after a crash the window is stale while
 * the ring still holds the newest committed transactions.
 */
int
NfsJournalReplay(PNFS_VOLUME Volume)
{
    nfs_u64 Seqs[4096];
    nfs_u32 Count = 0, i;
    nfs_u64 Highest = 0;
    nfs_u8 Frame[NFS_FRAME_SIZE];
    nfs_u64 Applied = 0;

    if (NfsJournalScanCommitted(Volume, Seqs, 4096, &Count) != NFS_OK)
        return NFS_EIO;
    if (Count == 0)
        return NFS_OK;
    if (Count > 4096)
        Count = 4096;

    NfsSeqSort(Seqs, Count);

    for (i = 0; i < Count; i++)
    {
        NFS_FRAME_HDR *Header = (NFS_FRAME_HDR *)Frame;
        nfs_u64 Offset = NfsJournalFrameOffset(Volume, Seqs[i]);
        nfs_u32 Cursor = 0;

        if (NfsDevRead(Volume, Offset, Frame, NFS_FRAME_SIZE) != NFS_OK)
            continue;

        while (Cursor + sizeof(NFS_PIFT_REC_HDR) <= Header->PayloadLen)
        {
            NFS_PIFT_REC_HDR *RecHeader = (NFS_PIFT_REC_HDR *)(Frame + NFS_FRAME_HDR_SIZE + Cursor);
            NFS_PIFT_RECORD Record;
            NFS_PIFT_RECORD *Existing;
            nfs_u32 RecLength;

            if (memcmp(RecHeader->Magic, NFS_REC_MAGIC, 4) != 0)
                break;
            RecLength = (nfs_u32)(sizeof(NFS_PIFT_REC_HDR) + RecHeader->BodyLen);
            if (RecLength > Header->PayloadLen - Cursor)
                break;

            if (NfsPiftDeserialize(RecHeader, RecLength, &Record) == NFS_OK)
            {
                Existing = NfsIndexGet(Volume, Record.FileId);
                if (!Existing || Existing->Seq <= Record.Seq)
                {
                    if (NfsIndexInsert(Volume, &Record) != NFS_OK)
                        return NFS_ENOSPC;
                    Applied++;
                }
            }
            Cursor += RecLength;
        }

        if (Seqs[i] > Highest)
            Highest = Seqs[i];
    }

    Volume->Stats.ReplayedFrames += Applied;

    /* The counters on disk were only written at the last checkpoint. */
    if (Highest + 1 > Volume->Super.JournalSeqNext)
        Volume->Super.JournalSeqNext = Highest + 1;

    return NFS_OK;
}
