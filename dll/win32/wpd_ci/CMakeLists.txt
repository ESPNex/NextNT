# Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
# SPDX-License-Identifier: GPL-3.0-or-later

spec2def(wpd_ci.dll wpd_ci.spec)

list(APPEND SOURCE
    wpd_ci.c
    ${CMAKE_CURRENT_BINARY_DIR}/wpd_ci.def)

add_library(wpd_ci MODULE ${SOURCE})
set_module_type(wpd_ci win32dll UNICODE)
target_compile_definitions(wpd_ci PRIVATE __WINESRC__)
target_link_libraries(wpd_ci uuid wine)
add_importlibs(wpd_ci setupapi ole32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET wpd_ci DESTINATION reactos/system32 FOR all)
