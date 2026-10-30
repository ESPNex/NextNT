
add_definitions(${I18N_DEFS})
if(_WINKD_)
    add_definitions(-D_WINKD_)
endif()
add_definitions(-D_SETUPLIB_)

include_directories(spapisup utils)

spec2def(setuplib.dll setuplib.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    spapisup/fileqsup.c
    spapisup/infsupp.c
    utils/arcname.c
    utils/bldrsup.c
    utils/devutils.c
    utils/filesup.c
    utils/fsrec.c
    utils/genlist.c
    utils/inicache.c
    utils/partinfo.c
    utils/partlist.c
    utils/regutil.c
    utils/volutil.c
    bootcode.c
    bootsup.c
    fsutil.c
    install.c
    mui.c
    registry.c
    settings.c
    setuplib.c
    precomp.h)

# GCC 13 sees six locals in arcname.c as possibly uninitialised that the GCC of
# RosBE does not, and this tree compiles with -Werror. They are false positives
# (each one is set on every path that uses it), so the flag keeps a compiler
# version difference from stopping the build of an unrelated file.
set_source_files_properties(utils/arcname.c PROPERTIES
    COMPILE_FLAGS "-Wno-error=maybe-uninitialized")

# Static library used for the dll and for setuplib_unittest.
add_library(setuplib_static ${SOURCE})
add_pch(setuplib_static precomp.h SOURCE)
add_dependencies(setuplib_static xdk) # psdk
target_link_libraries(setuplib_static gptsup)

add_library(setuplib SHARED
    main.c
    setuplib.rc
    ${CMAKE_CURRENT_BINARY_DIR}/setuplib.def)
target_link_libraries(setuplib setuplib_static)

add_dependencies(setuplib xdk) # psdk
set_module_type(setuplib nativedll)
target_link_libraries(setuplib ext2lib vfatlib ntfslib btrfslib nextfslib_user ${PSEH_LIB})
add_importlibs(setuplib ntdll)
add_cd_file(TARGET setuplib DESTINATION reactos/system32 NO_CAB FOR bootcd livecd)
add_cd_file(TARGET setuplib DESTINATION ${ARCH}/system32 FOR preinstall)
