
if(NEXTLDR_HAS_BIOS_BOOT)
    add_subdirectory(bootsect)
    add_subdirectory(fdebug)
endif()
add_subdirectory(nextldr)

# The NextFS first stage: one flat image that reads the volume itself and loads
# nextldr as a PE, so a NextFS volume needs no stub chain and no per-filesystem
# boot sector beyond the 512-byte one.
if(NEXTLDR_HAS_BIOS_BOOT)
    add_subdirectory(nextfsldr)
endif()
if(NEXTLDR_HAS_BIOS_BOOT)
    add_subdirectory(install)
endif()
