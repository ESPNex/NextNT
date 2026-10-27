
get_property(WINMD_SOURCES GLOBAL PROPERTY WINMD_SOURCES)
add_winmd_files(winmetadata reactos/system32/WinMetadata ${WINMD_SOURCES})
