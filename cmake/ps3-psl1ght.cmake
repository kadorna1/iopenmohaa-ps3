# OpenMoHAA PS3 toolchain for PSL1GHT.
set(CMAKE_SYSTEM_NAME PS3)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# CMake does not ship a built-in Platform/PS3 module. Explicitly provide the
# GNU dialect switches supported by the PSL1GHT GCC 7.x toolchain. GCC 7.2
# accepts GNU C11 but rejects -std=gnu17 for C sources. C++17 is accepted by
# the C++ compiler used by this project.
set(CMAKE_C11_STANDARD_COMPILE_OPTION "-std=gnu11")
set(CMAKE_C11_EXTENSION_COMPILE_OPTION "-std=gnu11")
set(CMAKE_CXX17_STANDARD_COMPILE_OPTION "-std=gnu++17")
set(CMAKE_CXX17_EXTENSION_COMPILE_OPTION "-std=gnu++17")

# Keep our source-side platform description available to project modules.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED ENV{PS3DEV})
    message(FATAL_ERROR "PS3DEV is not set; source the PSL1GHT environment first")
endif()

set(PS3DEV "$ENV{PS3DEV}")
if(DEFINED ENV{PSL1GHT})
    set(PSL1GHT "$ENV{PSL1GHT}")
else()
    # The reference ioQuake3/ioRTCW PSL1GHT builds use PS3DEV itself as
    # the SDK root (ppu/include + ppu/lib + ppu_rules).
    set(PSL1GHT "${PS3DEV}")
endif()

set(CMAKE_C_COMPILER   "${PS3DEV}/ppu/bin/ppu-gcc" CACHE FILEPATH "PS3 PPU C compiler")
set(CMAKE_CXX_COMPILER "${PS3DEV}/ppu/bin/ppu-g++" CACHE FILEPATH "PS3 PPU C++ compiler")
set(CMAKE_AR           "${PS3DEV}/ppu/bin/ppu-ar" CACHE FILEPATH "PS3 PPU archiver")
set(CMAKE_RANLIB       "${PS3DEV}/ppu/bin/ppu-ranlib" CACHE FILEPATH "PS3 PPU ranlib")
set(CMAKE_STRIP        "${PS3DEV}/ppu/bin/ppu-strip" CACHE FILEPATH "PS3 PPU strip")
set(CMAKE_LINKER       "${PS3DEV}/ppu/bin/ppu-ld" CACHE FILEPATH "PS3 PPU linker")
set(CMAKE_NM           "${PS3DEV}/ppu/bin/ppu-nm" CACHE FILEPATH "PS3 PPU nm")
set(CMAKE_OBJCOPY      "${PS3DEV}/ppu/bin/ppu-objcopy" CACHE FILEPATH "PS3 PPU objcopy")

set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/ppu" "${PSL1GHT}" "${PS3DEV}/portlibs/ppu" CACHE STRING "PS3 SDK roots" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mminimal-toc -Wl,--no-multi-toc -L${PSL1GHT}/ppu/lib -L${PS3DEV}/portlibs/ppu/lib")

include("${CMAKE_CURRENT_LIST_DIR}/compilers/psl1ght.cmake")
