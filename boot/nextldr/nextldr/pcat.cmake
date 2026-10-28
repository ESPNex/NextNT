##
## PROJECT:     FreeLoader
## LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
## PURPOSE:     Build definitions for PC-AT and "compatibles" (NEC PC-98, XBOX)
## COPYRIGHT:   Copyright 2003 Brian Palmer <brianp@sginet.com>
##              Copyright 2011-2014 Amine Khaldi <amine.khaldi@reactos.org>
##              Copyright 2011-2014 Timo Kreuzer <timo.kreuzer@reactos.org>
##              Copyright 2014 Hervé Poussineau <hpoussin@reactos.org>
##              Copyright 2023 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
##

if(ARCH STREQUAL "i386")
    CreateBootSectorTarget(frldr16
        ${CMAKE_CURRENT_SOURCE_DIR}/arch/realmode/i386.S
        ${CMAKE_CURRENT_BINARY_DIR}/frldr16.bin
        F800)
elseif(ARCH STREQUAL "amd64")
    CreateBootSectorTarget(frldr16
        ${CMAKE_CURRENT_SOURCE_DIR}/arch/realmode/amd64.S
        ${CMAKE_CURRENT_BINARY_DIR}/frldr16.bin
        F800)
endif()


spec2def(nextldr.sys nextldr.spec ADD_IMPORTLIB)

list(APPEND PCATLDR_ARC_SOURCE
    ${NEXTLDR_ARC_SOURCE}
    arch/drivers/hwide.c)

list(APPEND PCATLDR_BOOTMGR_SOURCE
    ${NEXTLDR_BOOTMGR_SOURCE}
    linuxboot.c)

list(APPEND PCATLDR_BASE_ASM_SOURCE)

if(ARCH STREQUAL "i386")
    list(APPEND PCATLDR_BASE_ASM_SOURCE
        arch/i386/multiboot.S)

    list(APPEND PCATLDR_COMMON_ASM_SOURCE
        arch/i386/drvmap.S
        arch/i386/entry.S
        arch/i386/int386.S
        arch/i386/irqsup.S
        arch/i386/pnpbios.S
        # arch/i386/i386trap.S
        arch/i386/linux.S)

    list(APPEND PCATLDR_ARC_SOURCE
        # disk/scsiport.c
        lib/fs/pxe.c
        arch/i386/drivemap.c
        arch/i386/hwacpi.c
        arch/i386/hwapm.c
        arch/i386/hwdisk.c
        arch/i386/hwpci.c
        # arch/i386/i386bug.c
        arch/i386/i386idt.c)

    if(SARCH STREQUAL "xbox")
        list(APPEND PCATLDR_ARC_SOURCE
            arch/vidfb.c)
    endif()
    if(SARCH STREQUAL "pc98" OR SARCH STREQUAL "xbox")
        # These machine types require built-in bitmap font
        list(APPEND PCATLDR_ARC_SOURCE
            arch/vgafont.c)
    endif()

    if(SARCH STREQUAL "xbox")
        list(APPEND PCATLDR_ARC_SOURCE
            # FIXME: Abstract things better so we don't need to include /pc/* here
            arch/i386/pc/machpc.c       # machxbox.c depends on it
            arch/i386/pc/pcbeep.c       # machxbox.c depends on it
            arch/i386/pc/pchw.c         # Many files depends on it
            arch/i386/pc/pcmem.c        # hwacpi.c/xboxmem.c depends on it
            arch/i386/xbox/machxbox.c
            arch/i386/xbox/xboxcons.c
            arch/i386/xbox/xboxdisk.c
            arch/i386/xbox/xboxi2c.c
            arch/i386/xbox/xboxmem.c
            arch/i386/xbox/xboxrtc.c
            arch/i386/xbox/xboxvideo.c)
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            # Prevent a warning when doing a memcmp with address 0
            set_source_files_properties(arch/i386/xbox/xboxmem.c PROPERTIES COMPILE_OPTIONS "-Wno-nonnull")
        endif()

    elseif(SARCH STREQUAL "pc98")
        list(APPEND PCATLDR_ARC_SOURCE
            arch/i386/pc/pcmem.c        # pc98mem.c depends on it
            arch/i386/pc98/machpc98.c
            arch/i386/pc98/pc98beep.c
            arch/i386/pc98/pc98cons.c
            arch/i386/pc98/pc98disk.c
            arch/i386/pc98/pc98hw.c
            arch/i386/pc98/pc98mem.c
            arch/i386/pc98/pc98rtc.c
            arch/i386/pc98/pc98video.c)
    else()
        list(APPEND PCATLDR_ARC_SOURCE
            arch/i386/pc/machpc.c
            arch/i386/pc/pcbeep.c
            arch/i386/pc/pccons.c
            arch/i386/pc/pcdisk.c
            arch/i386/pc/pchw.c
            arch/i386/pc/pcmem.c
            arch/i386/pc/pcrtc.c
            arch/i386/pc/pcvesa.c
            arch/i386/pc/pcvideo.c)
    endif()

elseif(ARCH STREQUAL "amd64")
    list(APPEND PCATLDR_BASE_ASM_SOURCE
        arch/i386/multiboot.S)

    list(APPEND PCATLDR_COMMON_ASM_SOURCE
        arch/amd64/entry.S
        arch/amd64/amd64trap.S
        arch/amd64/int386.S
        arch/amd64/pnpbios.S
        arch/amd64/linux.S)

    list(APPEND PCATLDR_ARC_SOURCE
        lib/fs/pxe.c
        arch/amd64/amd64trap.c
        arch/i386/drivemap.c
        arch/i386/hwacpi.c
        arch/i386/hwapm.c
        arch/i386/hwdisk.c
        arch/i386/hwpci.c
        # arch/i386/i386bug.c
        arch/i386/pc/machpc.c
        arch/i386/pc/pcbeep.c
        arch/i386/pc/pccons.c
        arch/i386/pc/pcdisk.c
        arch/i386/pc/pchw.c
        arch/i386/pc/pcmem.c
        arch/i386/pc/pcrtc.c
        arch/i386/pc/pcvesa.c
        arch/i386/pc/pcvideo.c)

elseif(ARCH STREQUAL "arm")
    list(APPEND PCATLDR_COMMON_ASM_SOURCE
        arch/arm/boot.S)

    list(APPEND PCATLDR_ARC_SOURCE
        arch/arm/entry.c
        arch/arm/macharm.c)
else()
    #TBD
endif()

add_asm_files(nextldr_common_asm ${NEXTLDR_COMMON_ASM_SOURCE} ${PCATLDR_COMMON_ASM_SOURCE})

add_library(nextldr_common
    ${nextldr_common_asm}
    ${PCATLDR_ARC_SOURCE}
    ${NEXTLDR_BOOTLIB_SOURCE}
    ${PCATLDR_BOOTMGR_SOURCE}
)
target_compile_definitions(nextldr_common PRIVATE _FRLDRLIB_)
if(NEXTLDR_WIM_RAMDISK)
    target_compile_definitions(nextldr_common PRIVATE NEXTLDR_WIM_RAMDISK=1)
endif()
target_link_libraries(nextldr_common fatfs)

if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # We need to reduce the binary size
    target_compile_options(nextldr_common PRIVATE "/Os")
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Prevent using SSE (no support in nextldr)
    target_compile_options(nextldr_common PUBLIC -mno-sse)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(nextldr_common PRIVATE --param=min-pagesize=0)
endif()

set(PCH_SOURCE
    ${PCATLDR_ARC_SOURCE}
    ${NEXTLDR_BOOTLIB_SOURCE}
    ${PCATLDR_BOOTMGR_SOURCE}
)

add_pch(nextldr_common include/nextldr.h PCH_SOURCE)
add_dependencies(nextldr_common bugcodes asm xdk)

## GCC builds need this extra thing for some reason...
if(ARCH STREQUAL "i386" AND NOT MSVC)
    target_link_libraries(nextldr_common mini_hal)
endif()

add_asm_files(nextldr_base_asm ${PCATLDR_BASE_ASM_SOURCE})

list(APPEND PCATLDR_BASE_SOURCE
    ${nextldr_base_asm}
    ${NEXTLDR_BASE_SOURCE})

add_executable(nextldr_pe ${PCATLDR_BASE_SOURCE})
target_compile_definitions(nextldr_pe PRIVATE _FRLDRLIB_)

set_target_properties(nextldr_pe
    PROPERTIES
    ENABLE_EXPORTS TRUE
    DEFINE_SYMBOL "")

if(MSVC)
    if(ARCH STREQUAL "arm")
        target_link_options(nextldr_pe PRIVATE /ignore:4078 /ignore:4254 /DRIVER)
    else()
        target_link_options(nextldr_pe PRIVATE /ignore:4078 /ignore:4254 /DYNAMICBASE:NO /FIXED /FILEALIGN:512 /ALIGN:512)
        add_linker_script(nextldr_pe nextldr_i386.msvc.lds)
    endif()
    # We don't need hotpatching
    remove_target_compile_option(nextldr_pe "/hotpatch")
    remove_target_compile_option(nextldr_common "/hotpatch")
else()
    target_link_options(nextldr_pe PRIVATE -Wl,--exclude-all-symbols,--file-alignment,0x200,--section-alignment,0x200)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_link_options(nextldr_pe PRIVATE -Wl,/driver)
    endif()
    add_linker_script(nextldr_pe nextldr_gcc.lds)
    # Strip everything, including the compressed symbol section
    add_custom_command(TARGET nextldr_pe
                    POST_BUILD
                    COMMAND ${CMAKE_STRIP} --remove-section=.symz $<TARGET_FILE:nextldr_pe>
                    COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:nextldr_pe>)
endif()

set_image_base(nextldr_pe 0x10000)
set_subsystem(nextldr_pe native)
set_entrypoint(nextldr_pe RealEntryPoint)

target_link_libraries(nextldr_pe nextldr_common cportlib libcntpr blrtl)
if(NEXTLDR_WIM_RAMDISK)
    target_link_libraries(nextldr_pe nextldr_wimcore)
endif()
if(ARCH STREQUAL "i386")
    target_link_libraries(nextldr_pe mini_hal)
endif()

# dynamic analysis switches
if(STACK_PROTECTOR)
    target_link_libraries(nextldr_pe gcc_ssp_nt)
endif()

if(RUNTIME_CHECKS)
    target_link_libraries(nextldr_pe runtmchk)
    target_link_options(nextldr_pe PRIVATE "/MERGE:.rtc=.text")
endif()

add_dependencies(nextldr_pe asm)

if(SARCH STREQUAL "pc98")
    file(MAKE_DIRECTORY ${REACTOS_BINARY_DIR}/PC98)
    add_custom_target(pc98bootfdd
        COMMAND native-fatten ${REACTOS_BINARY_DIR}/PC98/ReactOS-98.IMG -format 2880 ROS98BOOT -boot ${CMAKE_BINARY_DIR}/boot/nextldr/bootsect/pc98/fat12fdd.bin -add ${CMAKE_CURRENT_BINARY_DIR}/nextldr.sys NEXTLDR.SYS -add ${CMAKE_SOURCE_DIR}/boot/bootdata/floppy_pc98.ini NEXTLDR.INI
        DEPENDS native-fatten fat12pc98 nextldr
        VERBATIM)
endif()

if(NOT ARCH STREQUAL "arm")
    concatenate_files(
        ${CMAKE_CURRENT_BINARY_DIR}/nextldr.sys
        ${CMAKE_CURRENT_BINARY_DIR}/frldr16.bin
        ${CMAKE_CURRENT_BINARY_DIR}/$<TARGET_FILE_NAME:nextldr_pe>)
    add_custom_target(nextldr ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/nextldr.sys)
else()
    add_custom_target(nextldr ALL DEPENDS nextldr_pe)
endif()

add_cd_file(TARGET nextldr FILE ${CMAKE_CURRENT_BINARY_DIR}/nextldr.sys DESTINATION loader NO_CAB FOR bootcd livecd)
