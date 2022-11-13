cmake_minimum_required(VERSION 3.13)
project(Composer_Verilator)

set(CMAKE_CXX_STANDARD 17)

find_package(verilator REQUIRED)
find_package(composer 1.0.5 EXACT REQUIRED)
message("${verilator_FOUND}")

set(SRC src/data_server.cc src/cmd_server.cc )

include_directories(include)
# customize make based on simulator (verilator) or fpga (aws) target
if ("${TARGET}" STREQUAL "sim")
    add_executable(Composer_Verilator ${SRC} src/verilator.cc)
    if ("${USE_DRAMSIM}" STREQUAL "1")
        add_subdirectory(DRAMsim3)
        target_compile_definitions(Composer_Verilator PRIVATE USE_DRAMSIM=1)
        target_link_libraries(Composer_Verilator PRIVATE dramsim3 args)
    endif ()
    if ("${USE_WAVES}" STREQUAL "")
        set(vargs "${vargs} --trace")
    endif ()
    set(BUILD_SIM 1)
    set(CMAKE_CXX_FLAGS "-DSIM")
elseif ("${TARGET}" STREQUAL "fpga")
    if ("$ENV{COMPOSER_AWS_SDK_DIR}" STREQUAL "")
        message(FATAL_ERROR "aws-fpga not found. Please install aws-fpga repo and define $COMPOSER_AWS_SDK_DIR in environment")
    endif ()
    add_subdirectory(aws-setup)
    include_directories($ENV{COMPOSER_AWS_SDK_DIR}/sdk/userspace/include)
    add_executable(Composer_Verilator ${SRC} src/response_poller.cc src/fpga_main.cc)
    target_link_libraries(Composer_Verilator PRIVATE composer-fpga-utils)
    target_link_options(Composer_Verilator PRIVATE -lfpga_mgmt -lpthread -lrt)
    set(CMAKE_CXX_FLAGS "-DFPGA")
    set(BUILD_FPGA 1)
else()
    message(FATAL_ERROR "Must define build target: 'sim' or 'fpga'. -DTARGET=<opt>")
endif ()

# Tie in composer
set_target_properties(Composer_Verilator PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        )
target_link_libraries(Composer_Verilator PUBLIC APEX::composer)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

if ("$ENV{COMPOSER_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Environment variable $COMPOSER_ROOT is not defined")
endif ()

set(COMPOSER_HWDIR $ENV{COMPOSER_ROOT}/Composer-Hardware)

if (NOT EXISTS $ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src/composer.v)
    message(FATAL_ERROR "$ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src/composer.v does not exist. Try building your project.")
endif ()

include_directories($ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src)

if (${BUILD_SIM})
    verilate(Composer_Verilator
            SOURCES $ENV{COMPOSER_ROOT}/Composer-Hardware/vsim/generated-src/composer.v TOP_MODULE ComposerTop
            VERILATOR_ARGS -LDFLAGS "-undefined dynamic_lookup -lpthread" # --threads 2
            TRACE
            )
endif ()