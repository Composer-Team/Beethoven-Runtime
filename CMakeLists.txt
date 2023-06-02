cmake_minimum_required(VERSION 3.13)
project(ComposerRuntime)

set(CMAKE_CXX_STANDARD 17)

find_package(composer 1.0.6 EXACT REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC src/data_server.cc src/cmd_server.cc src/mmio.cc)
if ("$ENV{COMPOSER_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Environment variable $COMPOSER_ROOT is not defined")
endif ()

set(COMPOSER_DIR "$ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src")

if (NOT EXISTS ${COMPOSER_DIR}/composer_allocator_declaration.h)
    message(FATAL_ERROR "${COMPOSER_DIR}/composer_allocator_declaration.h does not exist. Try building your project or moving this file into the directory.")
endif ()

set(AWS_SDK "$ENV{COMPOSER_ROOT}/aws-fpga")

include_directories(include)
# customize make based on simulator (verilator) or fpga (aws) target
if ("${TARGET}" STREQUAL "sim")
    find_package(verilator REQUIRED)
    add_executable(ComposerRuntime ${SRC} src/verilator.cc)
    #
    #    add_executable(ComposerDMASimTest src/dma_sim_test.cc)
    #    target_link_libraries(ComposerDMASimTest PUBLIC APEX::composer)

    if ("${USE_DRAMSIM}" STREQUAL "1")
        add_subdirectory(DRAMsim3)
        target_compile_definitions(ComposerRuntime PRIVATE USE_DRAMSIM=1)
        target_link_libraries(ComposerRuntime PRIVATE dramsim3 args)
    endif ()
    if ("${USE_WAVES}" STREQUAL "")
        set(vargs "${vargs} --trace")
    endif ()
    target_compile_definitions(ComposerRuntime PUBLIC SIM)
    set(BUILD_SIM 1)
elseif ("${TARGET}" STREQUAL "vsim")
    add_library(ComposerRuntime STATIC ${SRC} src/response_poller.cc src/fpga_utils.c src/fpga_main.cc ${AWS_SDK}/sdk/userspace/utils/sh_dpi_tasks.c)
    include_directories(${AWS_SDK}/hdk/common/software/include ${AWS_SDK}/sdk/userspace/include ${AWS_SDK}/sdk/userspace/include/utils/)
    target_compile_definitions(ComposerRuntime PUBLIC VSIM FPGA)
    target_link_libraries(ComposerRuntime PRIVATE rt pthread fpga_mgmt)
    set(BUILD_FPGA 1)
elseif ("${TARGET}" STREQUAL "fpga")
    if ("${BACKEND}" STREQUAL "")
        message(FATAL_ERROR "Must define backend for FPGA. F1 or Kria")
    endif ()
    include_directories(${AWS_SDK}/sdk/userspace/include)
    add_executable(ComposerRuntime ${SRC} src/response_poller.cc src/fpga_utils.c src/fpga_main.cc src/mmio.cc include/mmio.h)
    if ("${BACKEND}" STREQUAL "F1")
        target_link_libraries(ComposerRuntime PRIVATE pthread fpga_mgmt)
    else ()
        target_link_libraries(ComposerRuntime PRIVATE pthread)
    endif ()
    target_compile_definitions(ComposerRuntime PUBLIC FPGA=1 ${BACKEND})
    set(BUILD_FPGA 1)
elseif ("${TARGET}" STREQUAL "sanity_xsim")
    add_library(ComposerRuntime STATIC ${FILE} ${AWS_SDK}/sdk/userspace/utils/sh_dpi_tasks.c)
    include_directories(${AWS_SDK}/hdk/common/software/include ${AWS_SDK}/sdk/userspace/include ${AWS_SDK}/sdk/userspace/include/utils/)
    target_compile_definitions(ComposerRuntime PUBLIC VSIM)
    target_link_libraries(ComposerRuntime PRIVATE pthread fpga_mgmt)
    set(BUILD_FPGA 1)
elseif ("${TARGET}" STREQUAL "sanity_fpga")
    add_executable(ComposerRuntime ${FILE} ${AWS_SDK}/sdk/userspace/utils/sh_dpi_tasks.c)
    include_directories(${AWS_SDK}/hdk/common/software/include ${AWS_SDK}/sdk/userspace/include ${AWS_SDK}/sdk/userspace/include/utils/)
    target_compile_definitions(ComposerRuntime PUBLIC VSIM FPGA)
    target_link_libraries(ComposerRuntime PRIVATE [pthread fpga_mgmt)
    set(BUILD_FPGA 1)
else ()
    message(FATAL_ERROR "Must define build target: 'sim', 'fpga', or 'vsim'. Got '${TARGET}'. -DTARGET=<opt>")
endif ()

# Tie in composer
set_target_properties(ComposerRuntime PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
target_link_libraries(ComposerRuntime PUBLIC APEX::composer)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")


set(COMPOSER_HWDIR $ENV{COMPOSER_ROOT}/Composer-Hardware)

if (APPLE)
    message("Building for Apple")
    set(verilator_args "-undefined dynamic_lookup ")
elseif (UNIX)
    message("Building for Linux")
    target_link_libraries(ComposerRuntime PRIVATE rt)
else ()
    message(FATAL_ERROR "Unsupported platform")
endif ()

include_directories($ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src)

if ("${VERBOSE}" STREQUAL "1")
    target_compile_definitions(ComposerRuntime PUBLIC VERBOSE)
endif ()


if ("${FPGA_CLOCK}" STREQUAL "")
    set(FPGA_CLOCK DEFAULT_PL_CLOCK)
endif ()


if ("${DDR_CLOCK}" STREQUAL "")
    set(DDR_CLOCK 1333)
endif ()

if ("${DDR_BURST_LENGTH}" STREQUAL "")
    set(DDR_BURST_LENGTH 8)
endif()

if ("${DDR_BUS_WIDTH_BITS}" STREQUAL "")
    set(DDR_BUS_WIDTH_BITS 64)
endif()

add_definitions(-DFPGA_CLOCK=${FPGA_CLOCK})
add_definitions(-DDDR_BURST_LENGTH=${DDR_BURST_LENGTH})
add_definitions(-DDDR_CLOCK=${DDR_CLOCK})
add_definitions(-DDDR_BUS_WIDTH_BITS=${DDR_BUS_WIDTH_BITS})

if (${BUILD_SIM})
    verilate(ComposerRuntime
            SOURCES ${COMPOSER_DIR}/composer.v
            INCLUDE_DIRS ${COMPOSER_DIR} $ENV{COMPOSER_ROOT}/Composer-Runtime/verilog_resources/
            TOP_MODULE ComposerTop
            #                        TRACE # VCD
            TRACE_FST
            VERILATOR_ARGS --timescale 1ps/1ps --x-assign unique
            -LDFLAGS "${verilator_args}-lpthread" -Wno-context -Wno-lint -Wno-style
            -Wno-moddup -Wno-unoptflat #  --threads 4
            )
endif ()
