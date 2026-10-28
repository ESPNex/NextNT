
if(NEXTLDR_HAS_BIOS_BOOT)
    add_subdirectory(bootsect)
    add_subdirectory(fdebug)
endif()
add_subdirectory(nextldr)
if(NEXTLDR_HAS_BIOS_BOOT)
    add_subdirectory(install)
endif()
