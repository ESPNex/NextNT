# Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
# SPDX-License-Identifier: GPL-3.0-or-later

spec2def(wpdshext.dll wpdshext.spec)

list(APPEND SOURCE
    folder.c
    guids.c
    items.c
    main.c
    ui.c
    wpdshext.h)

add_library(wpdshext MODULE
    ${SOURCE}
    wpdshext.rc
    ${CMAKE_CURRENT_BINARY_DIR}/wpdshext.def)

set_module_type(wpdshext win32dll UNICODE)
target_compile_definitions(wpdshext PRIVATE __WINESRC__)
target_link_libraries(wpdshext uuid wine)
add_importlibs(wpdshext shell32 shlwapi ole32 oleaut32 user32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET wpdshext DESTINATION reactos/system32 FOR all)
