# Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
# SPDX-License-Identifier: GPL-3.0-or-later

spec2def(portabledeviceapi.dll portabledeviceapi.spec)

list(APPEND SOURCE
    content.c
    device.c
    guids.c
    main.c
    manager.c
    properties.c
    resources.c
    private.h)

add_library(portabledeviceapi MODULE
    ${SOURCE}
    ${CMAKE_CURRENT_BINARY_DIR}/portabledeviceapi.def)

add_idl_reg_scripts(portabledeviceapi registry classes.idl)
set_module_type(portabledeviceapi win32dll UNICODE)
target_compile_definitions(portabledeviceapi PRIVATE __WINESRC__)
target_link_libraries(portabledeviceapi uuid wine wine_dll_register wine_dll_canunload)
add_importlibs(portabledeviceapi setupapi cfgmgr32 ole32 oleaut32 advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET portabledeviceapi DESTINATION reactos/system32 FOR all)
