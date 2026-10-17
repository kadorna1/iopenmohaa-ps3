# CMake platform description for the PlayStation 3 PPU toolchain.
# PSL1GHT uses GCC 7.x. GCC 7.2 supports GNU C11; it does not accept
# -std=gnu17 for C sources. C++17 is accepted by the C++ compiler.

set(CMAKE_SYSTEM_NAME PS3)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

set(CMAKE_C11_STANDARD_COMPILE_OPTION "-std=gnu11")
set(CMAKE_C11_EXTENSION_COMPILE_OPTION "-std=gnu11")
set(CMAKE_CXX17_STANDARD_COMPILE_OPTION "-std=gnu++17")
set(CMAKE_CXX17_EXTENSION_COMPILE_OPTION "-std=gnu++17")
