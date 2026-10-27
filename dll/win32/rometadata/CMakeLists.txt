
spec2def(rometadata.dll rometadata.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    assembly.c
    main.c
    mdtables.c
    rometadatapriv.h)

add_library(rometadata MODULE
    ${SOURCE}
    ${CMAKE_CURRENT_BINARY_DIR}/rometadata.def
    ${CMAKE_CURRENT_BINARY_DIR}/rometadata_stubs.c)

set_module_type(rometadata win32dll)
target_compile_definitions(rometadata PRIVATE __WINESRC__)
target_link_libraries(rometadata uuid wine)
add_importlibs(rometadata combase msvcrt kernel32 ntdll)
add_cd_file(TARGET rometadata DESTINATION reactos/system32 FOR all)
set_wine_module_FIXME(rometadata)
