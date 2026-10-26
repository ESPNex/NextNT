
list(APPEND SOURCE
    Bulk.c
    common.c
    Device.c
    guids.c
    ioctltrace.c
    PnP.c
    Probe.c
    Serializer.c
    Shell.c
    Transfer.c)

add_executable(portabledeviceapi_apitest ${SOURCE} testlist.c)
target_link_libraries(portabledeviceapi_apitest uuid)
set_module_type(portabledeviceapi_apitest win32cui)
add_importlibs(portabledeviceapi_apitest shell32 shlwapi ole32 oleaut32 setupapi advapi32 msvcrt kernel32 ntdll)
add_rostests_file(TARGET portabledeviceapi_apitest)
