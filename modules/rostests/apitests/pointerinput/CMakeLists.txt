
list(APPEND SOURCE
    PressAndHold.c
    testlist.c)

add_executable(pointerinput_apitest ${SOURCE})
target_link_libraries(pointerinput_apitest wine)
set_module_type(pointerinput_apitest win32cui)
add_importlibs(pointerinput_apitest user32 msvcrt kernel32)
add_rostests_file(TARGET pointerinput_apitest)
