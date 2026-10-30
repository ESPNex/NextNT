##
## PROJECT:     NextNT
## FILE:        boot/nextldr/nextfsldr/CMakeLists.txt
## PURPOSE:     Build the NextFS first stage
## LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
##
## This target is not an ordinary CMake target. What it produces is a flat binary
## that runs before there is an operating system: no libc, no stack protector, no
## unwinding, no SSE, and a linker script that places the real-mode half at
## 0x0F800 while it is stored somewhere else in the file. That is a job for a
## bare-metal ELF toolchain, not for the PE toolchain that builds the rest of the
## tree, so the host's own gcc, ld and objcopy are used directly when they are
## there, and the target is skipped with a message when they are not.
##
## It compiles nextldr's reader, lib/fs/nextfs_core.c, unchanged: one
## implementation of the format in the boot path, reached two ways.
##

if(NOT ARCH STREQUAL "i386")
    return()
endif()

foreach(tool /usr/bin/gcc /usr/bin/ld /usr/bin/objcopy)
    if(NOT EXISTS ${tool})
        message(STATUS "nextfsldr: ${tool} not found, the NextFS first stage is not built")
        return()
    endif()
endforeach()

set(NEXTFSLDR_CC /usr/bin/gcc)
set(NEXTFSLDR_LD /usr/bin/ld)
set(NEXTFSLDR_OBJCOPY /usr/bin/objcopy)

set(NEXTFSLDR_DIR ${REACTOS_SOURCE_DIR}/boot/nextldr/nextfsldr)

set(NEXTFSLDR_SOURCE
    ${NEXTFSLDR_DIR}/stage1.S
    ${NEXTFSLDR_DIR}/stage1.c
    ${NEXTFSLDR_DIR}/peload.c
    ${NEXTFSLDR_DIR}/div64.c
    ${REACTOS_SOURCE_DIR}/boot/nextldr/nextldr/lib/fs/nextfs_core.c
    ${REACTOS_SOURCE_DIR}/boot/nextldr/nextldr/lib/fs/nextfs_manifest.c)

set(NEXTFSLDR_CFLAGS
    -m32 -march=i386 -Os -std=c99
    -ffreestanding -fno-builtin -fno-pic -fno-pie
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables
    -mno-sse -mno-mmx -mno-80387
    -Wall -Wextra -Werror -Wno-array-bounds
    -DNEXTFS_BOOT_NO_ARC=1
    -I${NEXTFSLDR_DIR}
    -I${REACTOS_SOURCE_DIR}/boot/nextldr/nextldr/include)

set(NEXTFSLDR_OBJECTS)

foreach(source ${NEXTFSLDR_SOURCE})
    get_filename_component(name ${source} NAME)
    # stage1.S and stage1.c are two different objects with the same stem
    string(REPLACE "." "_" object ${name})
    set(object ${CMAKE_CURRENT_BINARY_DIR}/${object}.o)

    add_custom_command(
        OUTPUT ${object}
        COMMAND ${NEXTFSLDR_CC} ${NEXTFSLDR_CFLAGS} -c ${source} -o ${object}
        DEPENDS ${source}
        COMMENT "Building the NextFS first stage: ${name}"
        VERBATIM)

    list(APPEND NEXTFSLDR_OBJECTS ${object})
endforeach()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.sys
    COMMAND ${NEXTFSLDR_LD} -m elf_i386 -T ${NEXTFSLDR_DIR}/nextfsldr.ld
            -nostdlib --build-id=none
            -o ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.elf ${NEXTFSLDR_OBJECTS}
    COMMAND ${NEXTFSLDR_OBJCOPY} -O binary
            ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.elf
            ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.sys
    DEPENDS ${NEXTFSLDR_OBJECTS} ${NEXTFSLDR_DIR}/nextfsldr.ld
    COMMENT "Linking nextfsldr.sys"
    VERBATIM)

add_custom_target(nextfsldr ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.sys)

add_cd_file(TARGET nextfsldr FILE ${CMAKE_CURRENT_BINARY_DIR}/nextfsldr.sys
            DESTINATION loader NO_CAB FOR bootcd livecd)
