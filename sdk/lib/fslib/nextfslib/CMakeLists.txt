
# nextfslib: the NextFS on-disk format, the PIFT, the journal and the
# copy-on-write data path. It is deliberately free of kernel dependencies so it
# can be built and self-tested as a host program (tests/nextfs_selftest.c and the
# tools/ in this directory), and the FSD is then just a thin IRP translator over
# the same code. In a kernel build the caller installs the pool allocator and the
# wall clock with NfsSetAllocators()/NfsSetClock().

list(APPEND SOURCE
    nextfs_crc32c.c
    nextfs_super.c
    nextfs_pift.c
    nextfs_journal.c
    nextfs_tree.c
    nextfs_snapshot.c
    nextfs_security.c
    nextfs_namespace.c
    nextfs_handle.c
    nextfs_volume.c
    nextfs_format.h
    nextfs_internal.h)

add_library(nextfslib ${SOURCE})
add_dependencies(nextfslib psdk)

# The in-tree build is the kernel one: no libc, allocator and clock installed by
# nextfs.sys. Host builds (the self-test, the tools) compile these same sources
# without this define.
target_compile_definitions(nextfslib PRIVATE NFS_KERNEL_MODE)

target_include_directories(nextfslib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The user-mode build: the same portable core plus nextfsformat.c, which is the
# PULIB_FORMAT entry point the setup library links against. No NFS_KERNEL_MODE,
# so no pool allocator and no kernel headers; nextfsformat.c turns an NT volume
# handle into the four block-device callbacks and installs the process heap as
# the allocator, which is why the two malloc/free symbols are renamed below: a
# native DLL has no CRT to provide them.
list(APPEND NEXTFS_USER_SOURCE
    nextfs_crc32c.c
    nextfs_super.c
    nextfs_pift.c
    nextfs_journal.c
    nextfs_tree.c
    nextfs_snapshot.c
    nextfs_security.c
    nextfs_namespace.c
    nextfs_handle.c
    nextfs_volume.c
    nextfsformat.c
    nextfs_format.h
    nextfs_internal.h)

add_library(nextfslib_user ${NEXTFS_USER_SOURCE})
add_dependencies(nextfslib_user psdk)
target_compile_definitions(nextfslib_user PRIVATE
    _USRDLL
    malloc=NextfsSetupAlloc
    free=NextfsSetupFree)
target_include_directories(nextfslib_user
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
