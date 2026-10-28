
if(ARCH STREQUAL "i386" OR ARCH STREQUAL "amd64")
    set(NEXTLDR_HAS_BIOS_BOOT TRUE)
else()
    set(NEXTLDR_HAS_BIOS_BOOT FALSE)
endif()

set(NEXTLDR_HAS_OFW_BOOT FALSE)
if(ARCH STREQUAL "ppc")
    set(NEXTLDR_HAS_OFW_BOOT TRUE)
endif()

option(NEXTLDR_WIM_RAMDISK "Enable NextLDR boot.wim ramdisk decompression support" OFF)

include(boot_images.cmake)

add_subdirectory(bootdata)
add_subdirectory(nextldr)
add_subdirectory(rtl)

add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.cmd DESTINATION loader NO_CAB FOR bootcd)
add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/remaster.sh DESTINATION loader NO_CAB FOR bootcd)
