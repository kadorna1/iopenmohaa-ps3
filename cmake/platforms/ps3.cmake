# PlayStation 3 / PSL1GHT platform.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "PS3")
    return()
endif()

set(PS3 TRUE CACHE BOOL "Building for PlayStation 3" FORCE)
set(BUILD_SERVER OFF CACHE BOOL "Build dedicated server" FORCE)

# Keep runtime USRDIR/log/data paths identical to PARAM.SFO TITLE_ID.
# OMOHAA020 is the APPID currently used by the package builder.
set(OPENMOHAA_PS3_APPID "OMOHAA020" CACHE STRING "PS3 TITLE_ID / HDD game directory")
string(LENGTH "${OPENMOHAA_PS3_APPID}" OPENMOHAA_PS3_APPID_LENGTH)
if(NOT OPENMOHAA_PS3_APPID_LENGTH EQUAL 9 OR NOT OPENMOHAA_PS3_APPID MATCHES "^[A-Z0-9]+$")
    message(FATAL_ERROR "OPENMOHAA_PS3_APPID must be a 9-character uppercase alphanumeric TITLE_ID (got '${OPENMOHAA_PS3_APPID}')")
endif()

set(CMAKE_DEFAULT_INSTALL_RUNTIME_DIR bin)
set(BIN_INSTALL_SUBDIR "")
set(LIB_INSTALL_SUBDIR "")

list(APPEND SYSTEM_PLATFORM_SOURCES
    ${SOURCE_DIR}/sys/ps3_sys.c
    ${SOURCE_DIR}/sys/ps3_log.c
    ${SOURCE_DIR}/sys/ps3_ctor_trace.c
    ${SOURCE_DIR}/sys/ps3_rsxtest.c
    ${SOURCE_DIR}/sys/ps3_screenlog.c
    ${SOURCE_DIR}/sys/ps3_prof.c
    ${SOURCE_DIR}/sys/con_ps3.c
    ${SOURCE_DIR}/sys/new/sys_ps3_new.c
)

list(APPEND CLIENT_PLATFORM_SOURCES
    ${SOURCE_DIR}/input/ps3_input.c
    ${SOURCE_DIR}/input/ps3_osk.c
)

list(APPEND COMMON_LIBRARIES
    rsx
    gcm_sys
    io
    audio
    sysmodule
    sysutil
    rt
    lv2
    net
    netctl
    m
)

include_directories(
    ${SOURCE_DIR}
    ${SOURCE_DIR}/sys/include
    ${PSL1GHT}/ppu/include
    ${PS3DEV}/ppu/include
    ${PS3DEV}/portlibs/ppu/include
)
link_directories(
    ${PSL1GHT}/ppu/lib
    ${PS3DEV}/ppu/lib
    ${PS3DEV}/portlibs/ppu/lib
)

add_compile_definitions(__PS3__ _PS3 __lv2ppu__ __PSL1GHT__ PS3_APPID=\"${OPENMOHAA_PS3_APPID}\")
option(OPENMOHAA_PS3_RSX_TEST "Boot into the RSX bring-up test (sys/ps3_rsxtest.c) instead of the engine" OFF)
if(OPENMOHAA_PS3_RSX_TEST)
    add_compile_definitions(PS3_RSX_TEST=1)
endif()
add_compile_options(-mcpu=cell -mno-altivec -fno-strict-aliasing -fno-common -fno-gnu-unique -mminimal-toc)
add_compile_options(-include ${SOURCE_DIR}/sys/ps3_platform.h)

set(CLIENT_EXECUTABLE_OPTIONS)
set(CLIENT_LINK_OPTIONS -mminimal-toc -Wl,--no-multi-toc)
set(CLIENT_LIBRARIES ${CLIENT_LIBRARIES})

# OpenMoHAA does not need desktop dynamic loader libraries on PS3.
set(CMAKE_DL_LIBS "")

# PSL1GHT's standard build flow can produce a fake SELF after the client
# executable has been declared. client.cmake owns that target creation.
find_program(PS3_FSELF fself
    HINTS "${PS3DEV}/bin" "${PSL1GHT}/bin")
find_program(PS3_SPRXLINKER sprxlinker
    HINTS "${PS3DEV}/bin" "${PSL1GHT}/bin")

