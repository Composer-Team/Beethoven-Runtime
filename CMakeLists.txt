cmake_minimum_required(VERSION 3.10)
project(Composer_Verilator)

set(CMAKE_CXX_STANDARD 17)

find_package(verilator REQUIRED)
find_package(composer 1.0.2 EXACT REQUIRED)
message("${verilator_FOUND}")

add_executable(Composer_Verilator main.cc data_server.cc cmd_server.cc)
target_link_libraries(Composer_Verilator PUBLIC APEX::composer)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

if("${USE_WAVES}" STREQUAL "")
    set(vargs "${vargs} --trace")
    endif()

message("$ENV{COMPOSER_HARDWARE_DIR}")
if("$ENV{COMPOSER_HARDWARE_DIR}" STREQUAL "")
    if ("$ENV{COMPOSER_ROOT}" STREQUAL "")
        message(FATAL_ERROR "Neither COMPOSER_HARDWARE_DIR or COMPOSER_ROOT is defined")
    else()
        set(COMPOSER_HWDIR $ENV{COMPOSER_ROOT}/Composer-Hardware)
    endif()
else()
    set(COMPOSER_HWDIR $ENV{COMPOSER_HARDWARE_DIR})
endif()

if (NOT EXISTS ${COMPOSER_HWDIR}/vsim/generated-src/composer.v)
    message(FATAL_ERROR "${COMPOSER_HWDIR}/vsim/generated-src/composer.v does not exist. Try building your project.")
endif()

include_directories(${COMPOSER_HWDIR}/vsim/generated-src)

verilate(Composer_Verilator
        SOURCES ${COMPOSER_HWDIR}/vsim/generated-src/composer.v TOP_MODULE ComposerTop
        VERILATOR_ARGS -LDFLAGS "-undefined dynamic_lookup -lpthread" # --threads 2
        TRACE
        )
