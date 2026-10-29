
# NextFS: the NextNT native file system driver.
#
# The driver is built as part of the tree from phase 10 on, but it is NOT
# installed into the boot image: NTFS is still the boot file system. Installing it
# is the last step of the switch over (see DESIGN.md), and it will only happen
# once a volume formatted by `nextformat` boots a system end to end.

list(APPEND SOURCE
    nextfs.c
    nextfs_lock.c
    nextfs_rw.c
    nextfs_finfo.c
    nextfs_dirctl.c
    nextfs_volinfo.c
    nextfs.h)

add_library(nextfs MODULE ${SOURCE})
set_module_type(nextfs kernelmodedriver)
add_importlibs(nextfs ntoskrnl hal)
add_pch(nextfs nextfs.h SOURCE)
target_link_libraries(nextfs nextfslib)
target_include_directories(nextfs PRIVATE ${REACTOS_SOURCE_DIR}/sdk/lib/fslib/nextfslib)
