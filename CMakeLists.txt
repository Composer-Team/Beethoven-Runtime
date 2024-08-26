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

if ("${SIMULATOR}" STREQUAL "")
    set(SIMULATOR "verilator")
endif ()

if ("${TARGET}" STREQUAL "sim")
    #    find_package(verilator REQUIRED VERSION )
    # change find_package to require at least major version 5.0.0

    # define frontend sources for simulator
    set(SRC ${SRC} src/sim/tick.cc)
    if ("${FRONTEND}" STREQUAL "axi" OR "${FRONTEND}" STREQUAL "")
        set(SRC ${SRC} src/sim/axi/front_bus_ctrl_axi.cc)
        if ("${SIMULATOR}" STREQUAL "verilator")
            set(SRC ${SRC} src/sim/axi/verilator_axi_frontend.cc)
            find_package(verilator REQUIRED VERSION 5.0.0)
#            add_link_options(-latomic)
        elseif ("${SIMULATOR}" STREQUAL "vcs")
            set(SRC ${SRC} src/sim/axi/vcs_axi_frontend.cc)
        endif ()
    endif ()
    if ("${FRONTEND}" STREQUAL "chipkit")
        if ("${SIMULATOR}" STREQUAL "verilator")
            set(SRC ${SRC} src/sim/chipkit/verilator_chipkit_frontend.cc)
        elseif ("${SIMULATOR}" STREQUAL "vcs")
            set(SRC ${SRC} src/sim/chipkit/vcs_chipkit_frontend.cc)
        endif ()
    endif ()

    set(SRC ${SRC} src/sim/mem_ctrl.cc)
    if (NOT DEFINED FRONTEND)
        set(FRONTEND "axi")
    endif ()


    if ("${USE_WAVES}" STREQUAL "")
        set(vargs "${vargs} --trace")
    endif ()

    if ("${SIMULATOR}" STREQUAL "verilator")
        add_subdirectory(DRAMsim3)
        find_package(verilator REQUIRED VERSION 5.0.0)
        add_executable(BeethovenRuntime ${SRC})

        target_link_libraries(BeethovenRuntime PRIVATE dramsim3 args)
        add_compile_definitions(VERILATOR=1)
    elseif ("${SIMULATOR}" STREQUAL "vcs")
        # NOTE: YOU ARE RESPONSIBLE FOR BUILDING AND LINKING DRAMSIM3
        add_subdirectory(DRAMsim3)
        install(TARGETS dramsim3
                DESTINATION $ENV{BEETHOVEN_ROOT}/lib/)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/tab.tab
          DESTINATION $ENV{BEETHOVEN_ROOT}/inc/)
        add_library(BeethovenRuntime STATIC ${SRC})
        add_compile_definitions(VCS=1)
        target_include_directories(BeethovenRuntime PUBLIC $ENV{VCS_HOME}/include)
        target_link_libraries(BeethovenRuntime PRIVATE dramsim3 args)
    else ()
        message(FATAL_ERROR "Unsupported simulator")
    endif ()

    if (NOT "${KILL_SIM_AFTER}" STREQUAL "")
        target_compile_definitions(BeethovenRuntime PRIVATE KILL_SIM=${KILL_SIM_AFTER})
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

add_executable(brt-kill src/kill.cc)
target_link_libraries(brt-kill PUBLIC APEX::beethoven)
