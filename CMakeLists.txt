cmake_minimum_required(VERSION 3.13)
project(BeethovenRuntime)

set(CMAKE_CXX_STANDARD 17)

find_package(beethoven REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC src/data_server.cc src/cmd_server.cc src/mmio.cc)
if ("$ENV{BEETHOVEN_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Environment variable $BEETHOVEN_ROOT is not defined")
endif ()

set(BEETHOVEN_DIR "$ENV{BEETHOVEN_ROOT}/Beethoven-Hardware/vsim/generated-src")

if (NOT EXISTS ${BEETHOVEN_DIR}/beethoven_allocator_declaration.h)
    message(FATAL_ERROR "${BEETHOVEN_DIR}/beethoven_allocator_declaration.h does not exist. Try building your project or moving this file into the directory.")
endif ()

set(AWS_SDK "$ENV{BEETHOVEN_ROOT}/aws-fpga")
include_directories(include)

# define frontend sources for simulator
if ("${FRONTEND}" STREQUAL "axi" OR "${FRONTEND}" STREQUAL "")
    set(SRC ${SRC} src/sim/axi/verilator_axi_frontend.cc src/sim/axi/front_bus_ctrl_axi.cc src/sim/axi/state_machine.cc)
endif ()
if ("${FRONTEND}" STREQUAL "chipkit")
    set(SRC ${SRC} src/chipkit/chipkit_frontend.cc src/chipkit/chipkit_bus_ctrl.cc)
endif ()

if ("${SIMULATOR}" STREQUAL "")
    set (SIMULATOR "verilator")
endif ()

if ("${TARGET}" STREQUAL "sim")
    find_package(verilator REQUIRED VERSION 5.0.0)
    add_link_options(-latomic)
    set(SRC ${SRC} src/sim/mem_ctrl.cc)
    if (NOT DEFINED FRONTEND)
        set(FRONTEND "axi")
    endif ()
    add_executable(BeethovenRuntime ${SRC})
    if (NOT "${KILL_SIM_AFTER}" STREQUAL "")
        target_compile_definitions(BeethovenRuntime PRIVATE KILL_SIM=${KILL_SIM_AFTER})
    endif ()

    add_subdirectory(DRAMsim3)
    target_link_libraries(BeethovenRuntime PRIVATE dramsim3 args)
    if ("${USE_WAVES}" STREQUAL "")
        set(vargs "${vargs} --trace")
    endif ()
    target_compile_definitions(BeethovenRuntime PUBLIC SIM)
    set(BUILD_SIM 1)
elseif ("${TARGET}" STREQUAL "fpga")
    if ("${BACKEND}" STREQUAL "")
        message(FATAL_ERROR "Must define backend for FPGA. F1 or Kria")
    endif ()
    include_directories(${AWS_SDK}/sdk/userspace/include)
    add_executable(BeethovenRuntime ${SRC} src/response_poller.cc src/fpga_utils.c src/fpga_main.cc src/mmio.cc include/mmio.h)
    if ("${BACKEND}" STREQUAL "F1")
        target_link_libraries(BeethovenRuntime PUBLIC pthread fpga_mgmt)
    endif ()
    target_compile_definitions(BeethovenRuntime PUBLIC FPGA=1 ${BACKEND})
    set(BUILD_FPGA 1)
else ()
    message(FATAL_ERROR "Must define build target: 'sim', or 'fpga'. Got '${TARGET}'. -DTARGET=<opt>")
endif ()

# Tie in beethoven
set_target_properties(BeethovenRuntime PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)
target_link_libraries(BeethovenRuntime PUBLIC APEX::beethoven)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")


set(BEETHOVEN_HWDIR $ENV{BEETHOVEN_ROOT}/Beethoven-Hardware)

if (APPLE)
    message("Building for Apple")
    set(verilator_args "-undefined dynamic_lookup ")
elseif (UNIX)
    message("Building for Linux")
    target_link_libraries(BeethovenRuntime PRIVATE rt)
else ()
    message(FATAL_ERROR "Unsupported platform")
endif ()

include_directories($ENV{BEETHOVEN_ROOT}/Beethoven-Hardware/vsim/generated-src)

if ("${VERBOSE}" STREQUAL "1")
    target_compile_definitions(BeethovenRuntime PUBLIC VERBOSE)
endif ()

if ("${VTOP}" STREQUAL "")
    set(VTOP ${BEETHOVEN_DIR}/beethoven.build/BeethovenTop.v)
endif ()

if ("${TOP}" STREQUAL "")
    set(TOP BeethovenTop)
endif ()

if ("${CONTROL_LITE}" STREQUAL "1")
    target_compile_definitions(BeethovenRuntime PUBLIC CONTROL_LITE)
endif ()

if ("${FRONTEND}" STREQUAL "chipkit")
    target_compile_definitions(BeethovenRuntime PUBLIC BAREMETAL_RUNTIME=1)
    message("Building for chipkit")
endif ()

if (${BUILD_SIM} AND ("${SIMULATOR}" STREQUAL "verilator"))
    # if file exists
    include(${BEETHOVEN_DIR}/cmake_srcs.cmake)
    verilate(BeethovenRuntime
            SOURCES ${SRCS}
            INCLUDE_DIRS ${BEETHOVEN_DIR} $ENV{BEETHOVEN_ROOT}/Beethoven-Runtime/verilog_resources/ ${BEETHOVEN_DIR}/beethoven.build/ ${ADDITIONAL_SEARCH}
            TOP_MODULE ${TOP}
            PREFIX ${TOP}
            TRACE_FST
            VERILATOR_ARGS --timescale 1ps/1ps --x-assign fast
            -Wno-context -Wno-lint -Wno-style -Wno-symrsvdword -Wno-multidriven -Wno-combdly -Wno-zerodly
            -Wno-moddup -Wno-unoptflat
    )
endif ()
