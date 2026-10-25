cmake_minimum_required(VERSION 3.20)

# Toolchain must be selected before project()
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain-riscv-xtheade.cmake" CACHE FILEPATH "")
endif()

project(akp05_firmware LANGUAGES C ASM)

# ------------------------------------------------------------------ settings you may want to change
# Edit the version here: it is a plain variable, so a change takes effect at the next build without touching the cache.
set(AKP05_FW_VERSION "1.0.0")            # shown on the boot screen as "AKP05E OPEN FIRMWARE <version>[-<git hash>]"
# The next three are cache entries, set from the command line or from scripts\build.ps1 (-AppLog, -UsbDebug); the values
# below are only the defaults of the first configure of a build directory.
set(AKP05_APP_LOG 1 CACHE STRING "Application UART log level: 0 errors only, 1 events, 2 verbose")
option(AKP05_USB_DEBUG "Log every USB control request (CherryUSB debug level) to the UART" OFF)
set(AKP05_DEFCONFIG "d13x_akp05-nor_rt-thread_akp05fw_defconfig" CACHE STRING "SDK defconfig (target/configs)")

# ------------------------------------------------------------------ project setup
set(AKP05_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(SDK_DIR    "${AKP05_ROOT}/sdk" CACHE PATH "Luban-Lite SDK (clean v1.0.3 + board akp05-nor)")
set(AKP05_IMAGE_DIR "${CMAKE_BINARY_DIR}/images")
set(AKP05_GEN_DIR   "${CMAKE_BINARY_DIR}/generated")
set(AKP05_IMAGE_CFG "${AKP05_ROOT}/firmware/image/image_cfg.json")   # partition layout, single source of truth

set(_sdk_env "${SDK_DIR}/tools/env")
set(SDK_PYTHON3 "${_sdk_env}/tools/Python39/python3.exe" CACHE FILEPATH "Python bundled with the SDK (runs the SDK scripts)")
set(SDK_SCONS   "${_sdk_env}/tools/Python27/Scripts/scons.bat" CACHE FILEPATH "SCons bundled with the SDK (Kconfig step only)")

# 1) Kconfig step: rtconfig.h / cconfig.h / .config for the chosen defconfig (done once at configure time)
if(NOT EXISTS "${SDK_DIR}/rtconfig.h")
    message(STATUS "Applying SDK defconfig ${AKP05_DEFCONFIG} (scons --apply-def)")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                "ENV_ROOT=${_sdk_env}" "PKGS_ROOT=${_sdk_env}/packages" "PYTHONPATH=${_sdk_env}/tools/Python27"
                "${SDK_SCONS}" "--apply-def=${AKP05_DEFCONFIG}"
        WORKING_DIRECTORY "${SDK_DIR}"
        RESULT_VARIABLE _rc OUTPUT_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "scons --apply-def failed (${_rc})")
    endif()
endif()

# 2) partition table header from OUR firmware/image/image_cfg.json (spl 512k etc.), found before the SDK's copy
file(MAKE_DIRECTORY "${AKP05_GEN_DIR}" "${AKP05_IMAGE_DIR}")
add_custom_command(
    OUTPUT "${AKP05_GEN_DIR}/partition_table.h" "${AKP05_IMAGE_DIR}/partition.json"
    COMMAND "${SDK_PYTHON3}" "${SDK_DIR}/tools/scripts/gen_partition_table.py"
            -c "${AKP05_IMAGE_CFG}" -o "${AKP05_GEN_DIR}/partition_table.h"
            -j "${AKP05_IMAGE_DIR}/partition.json"
    DEPENDS "${AKP05_IMAGE_CFG}"
    COMMENT "Generating partition_table.h")
add_custom_command(
    OUTPUT "${AKP05_IMAGE_DIR}/partition_file_list.h"
    COMMAND "${SDK_PYTHON3}" "${SDK_DIR}/tools/scripts/gen_partition_file_list.py"
            -c "${AKP05_IMAGE_CFG}" -o "${AKP05_IMAGE_DIR}/partition_file_list.h"
    DEPENDS "${AKP05_IMAGE_CFG}"
    COMMENT "Generating partition_file_list.h")
add_custom_target(partition_headers DEPENDS
    "${AKP05_GEN_DIR}/partition_table.h" "${AKP05_IMAGE_DIR}/partition_file_list.h")

# ------------------------------------------------------------------ firmware
include("${AKP05_ROOT}/cmake/sdk_sources.cmake")      # SDK_C_FLAGS SDK_DEFINITIONS SDK_INCLUDE_DIRS SDK_SOURCES
include("${AKP05_ROOT}/firmware/app/sources.cmake")    # AKP05_APP_SOURCES AKP05_APP_INCLUDE_DIRS

set(AKP05_LINKER_SCRIPT "${SDK_DIR}/target/d13x/akp05-nor/lds/link_akp05.ld" CACHE FILEPATH "")
# The SDK linker script picks the startup code by object file name (startup_gcc.o); CMake names objects *.obj.
file(READ "${AKP05_LINKER_SCRIPT}" _ld_text)
string(REPLACE "startup_gcc.o(" "startup_gcc.S.obj(" _ld_text "${_ld_text}")
file(WRITE "${CMAKE_BINARY_DIR}/akp05.ld" "${_ld_text}")
set(AKP05_LINKER_SCRIPT "${CMAKE_BINARY_DIR}/akp05.ld")
set(AKP05_ARCH_FLAGS -march=rv32imafdcpzpsfoperand_xtheade -mabi=ilp32d)

add_executable(akp05_fw ${SDK_SOURCES} ${AKP05_APP_SOURCES})
add_dependencies(akp05_fw partition_headers)

target_include_directories(akp05_fw PRIVATE
    "${AKP05_GEN_DIR}" "${AKP05_IMAGE_DIR}" ${AKP05_APP_INCLUDE_DIRS} ${SDK_INCLUDE_DIRS})
target_compile_definitions(akp05_fw PRIVATE AKP05_APP_LOG=${AKP05_APP_LOG})

# Boot screen banner (firmware/app/display.c): version, git hash and time of the configure step. Only display.c gets
# these definitions, so a new time stamp recompiles that one file. build.ps1 runs the configure step every time.
execute_process(COMMAND git -C "${AKP05_ROOT}" rev-parse --short HEAD
                OUTPUT_VARIABLE _git_hash ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _git_rc)
if(_git_rc EQUAL 0 AND _git_hash)
    set(_fw_version "${AKP05_FW_VERSION}-${_git_hash}")
else()
    set(_fw_version "${AKP05_FW_VERSION}")
endif()
string(TIMESTAMP _build_date "%Y-%m-%d")
string(TIMESTAMP _build_time "%H:%M")
set_source_files_properties("${AKP05_ROOT}/firmware/app/display.c" PROPERTIES COMPILE_DEFINITIONS
    "AKP05_FW_VERSION=\"${_fw_version}\";AKP05_BUILD_DATE=\"${_build_date}\";AKP05_BUILD_TIME=\"${_build_time}\"")
if(AKP05_USB_DEBUG)
    target_compile_definitions(akp05_fw PRIVATE CONFIG_USB_DBG_LEVEL=USB_DBG_LOG)
endif()
target_compile_options(akp05_fw PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${SDK_C_FLAGS} ${SDK_DEFINITIONS}>
    $<$<COMPILE_LANGUAGE:ASM>:${AKP05_ARCH_FLAGS} -x assembler-with-cpp -D__ASSEMBLY__ ${SDK_DEFINITIONS}>)
target_link_options(akp05_fw PRIVATE
    ${AKP05_ARCH_FLAGS} -nostartfiles -Wl,--no-whole-archive
    -Wl,-gc-sections -Wl,-zmax-page-size=1024
    # usb_hid.c filters a bogus EP0 completion of the ArtInChip CherryUSB port (see __wrap_ there)
    -Wl,--wrap=usbd_event_ep_out_complete_handler
    # ... and retries a refused EP0 IN chunk (__wrap_usbd_ep_start_write)
    -Wl,--wrap=usbd_ep_start_write
    # ... and records every control request for the UART trace (__wrap_usbd_event_ep0_setup_complete_handler)
    -Wl,--wrap=usbd_event_ep0_setup_complete_handler
    "-Wl,-Map=${AKP05_IMAGE_DIR}/akp05_fw.map" -T "${AKP05_LINKER_SCRIPT}")
# SCons links with g++, whose driver appends -lm after the objects; with gcc they must come after them explicitly.
target_link_libraries(akp05_fw PRIVATE -Wl,--start-group -lm -lc -lgcc -Wl,--end-group)
set_target_properties(akp05_fw PROPERTIES LINK_DEPENDS "${AKP05_LINKER_SCRIPT}")

# SCons groups with LOCAL_CPPPATH get their own directory appended to -I (entries: "<file>|<-I..>|<-I..>")
foreach(_entry IN LISTS SDK_EXTRA_INCLUDES_BY_FILE)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(POP_FRONT _parts _file)
    set_source_files_properties("${_file}" PROPERTIES COMPILE_OPTIONS "${_parts}")
endforeach()

add_custom_command(TARGET akp05_fw POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:akp05_fw>" "${AKP05_IMAGE_DIR}/akp05_fw.elf"
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:akp05_fw>" "${AKP05_IMAGE_DIR}/akp05_fw.bin"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:akp05_fw>"
    COMMENT "objcopy -> akp05_fw.bin")

# ------------------------------------------------------------------ image (os.itb + flashable .img)
add_custom_target(image ALL
    COMMAND "${SDK_PYTHON3}" "${AKP05_ROOT}/scripts/pack_image.py"
            --sdk "${SDK_DIR}" --pack "${AKP05_ROOT}/firmware/image" --fs "${AKP05_ROOT}/firmware/rootfs"
            --elf "$<TARGET_FILE:akp05_fw>" --out "${AKP05_IMAGE_DIR}"
    DEPENDS akp05_fw
    COMMENT "Packing d13x_os.itb and the flash image"
    VERBATIM)
