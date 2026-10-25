# Cross toolchain bundled with the ArtInChip Luban-Lite SDK (T-Head Xuantie RISC-V, newlib).
# Used as CMAKE_TOOLCHAIN_FILE; the SDK is expected in <project>/sdk.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

get_filename_component(_AKP05_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(AKP05_TOOLCHAIN_DIR "${_AKP05_ROOT}/sdk/toolchain" CACHE PATH "SDK toolchain directory")

if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()
set(_pfx "${AKP05_TOOLCHAIN_DIR}/bin/riscv64-unknown-elf-")

set(CMAKE_C_COMPILER   "${_pfx}gcc${_exe}")
set(CMAKE_CXX_COMPILER "${_pfx}g++${_exe}")
set(CMAKE_ASM_COMPILER "${_pfx}gcc${_exe}")
set(CMAKE_OBJCOPY      "${_pfx}objcopy${_exe}" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${_pfx}objdump${_exe}" CACHE FILEPATH "")
set(CMAKE_SIZE         "${_pfx}size${_exe}"    CACHE FILEPATH "")
set(CMAKE_READELF      "${_pfx}readelf${_exe}" CACHE FILEPATH "")

# bare metal: do not try to link test executables, only compile
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
