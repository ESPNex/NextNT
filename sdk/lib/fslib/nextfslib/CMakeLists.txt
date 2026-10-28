
list(APPEND SOURCE
    nextfs_crc32c.c
    nextfs_super.c
    nextfs_pift.c
    nextfs_journal.c
    nextfs_volume.c)

add_library(nextfslib ${SOURCE})
add_dependencies(nextfslib psdk)
