#pragma once
#include <nextldr.h>

typedef ARC_STATUS (*ARC_CLOSE)(ULONG FileId);
typedef ARC_STATUS (*ARC_GET_FILE_INFORMATION)(ULONG FileId, FILEINFORMATION *Information);
typedef ARC_STATUS (*ARC_OPEN)(CHAR *Path, OPENMODE OpenMode, ULONG *FileId);
typedef ARC_STATUS (*ARC_READ)(ULONG FileId, VOID *Buffer, ULONG N, ULONG *Count);
typedef ARC_STATUS (*ARC_SEEK)(ULONG FileId, LARGE_INTEGER *Position, SEEKMODE SeekMode);

typedef struct tagDEVVTBL
{
    ARC_CLOSE Close;
    ARC_GET_FILE_INFORMATION GetFileInformation;
    ARC_OPEN Open;
    ARC_READ Read;
    ARC_SEEK Seek;
    PCWSTR ServiceName;
} DEVVTBL;
