add_executable(applaunch applaunch.c)
set_module_type(applaunch win32gui UNICODE)
add_importlibs(applaunch msvcrt kernel32)
add_cd_file(TARGET applaunch DESTINATION reactos/system32 FOR preinstall livecd)
