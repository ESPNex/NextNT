
# NextFS: the NextNT native file system driver.
# Not wired into drivers/filesystems/CMakeLists.txt yet: NTFS stays the boot
# file system until the phase-2 I/O paths exist (see DESIGN.md).

list(APPEND SOURCE
    nextfs.c)

list(APPEND NEXTFSLIB_SOURCE
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_crc32c.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_super.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_pift.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_journal.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_tree.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_snapshot.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib/nextfs_volume.c)

add_library(nextfs MODULE ${SOURCE} ${NEXTFSLIB_SOURCE})
set_module_type(nextfs kernelmodedriver)
add_importlibs(nextfs ntoskrnl hal)
add_pch(nextfs nextfs.h SOURCE)
target_include_directories(nextfs PRIVATE ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib)
