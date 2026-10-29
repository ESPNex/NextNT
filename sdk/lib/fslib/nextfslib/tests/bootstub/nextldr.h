/* Minimal stand-in for the loader headers, used only by the host-side glue test.
   It reproduces the ARC contract the real headers declare, nothing else. */
#pragma once

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char      UCHAR;
typedef unsigned short     USHORT;
typedef unsigned int       ULONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long ULONG_PTR;
typedef long long          LONGLONG;
typedef char               CHAR;
typedef void               VOID;
typedef void              *PVOID;
typedef const char        *PCSTR;
typedef const unsigned short *PCWSTR;
typedef UCHAR              BOOLEAN;

#define ANSI_NULL '\0'
#define MAX_FDS 60
#define SECTOR_SIZE 512
#define RTL_FIELD_SIZE(type, field) (sizeof(((type *)0)->field))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))

typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;

typedef ULONG ARC_STATUS;
#define ESUCCESS 0
#define ENOENT   2
#define EIO      5
#define EACCES   13
#define EINVAL   22
#define ENOMEM   12

typedef enum { OpenReadOnly = 0, OpenReadWrite = 1 } OPENMODE;
typedef enum { SeekAbsolute = 0, SeekRelative = 1 } SEEKMODE;

typedef struct
{
    LARGE_INTEGER EndingAddress;
    LARGE_INTEGER CurrentAddress;
    UCHAR Attributes;
    CHAR FileName[32];
    ULONG FileNameLength;
} FILEINFORMATION, *PFILEINFORMATION;

PVOID FrLdrTempAlloc(size_t Size, ULONG Tag);
VOID FrLdrTempFree(PVOID Block, ULONG Tag);
ARC_STATUS ArcSeek(ULONG DeviceId, LARGE_INTEGER *Position, SEEKMODE SeekMode);
ARC_STATUS ArcRead(ULONG DeviceId, VOID *Buffer, ULONG N, ULONG *Count);
ULONG FsGetDeviceId(ULONG FileId);
VOID *FsGetDeviceSpecific(ULONG FileId);
VOID FsSetDeviceSpecific(ULONG FileId, PVOID Specific);
ARC_STATUS FsGetVolumeSize(ULONG DeviceId, ULONGLONG *VolumeSize);
VOID FileSystemError(PCSTR Message);
