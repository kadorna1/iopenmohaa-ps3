# PSL1GHT/Cell PPU compiler options.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "PS3")
    return()
endif()

add_compile_definitions(__PS3__ _PS3 __lv2ppu__ __PSL1GHT__)
add_compile_options(-mcpu=cell -mno-altivec -fno-strict-aliasing -fno-common -fno-gnu-unique -mminimal-toc)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
