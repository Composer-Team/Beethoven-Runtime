cmake_minimum_required(VERSION 3.10)
project(Composer_Verilator)

set(CMAKE_CXX_STANDARD 17)

find_package(verilator REQUIRED)
find_package(composer REQUIRED)
message("${verilator_FOUND}")


message("$ENV{YAML_DIR}")
find_package(yaml-cpp REQUIRED HINTS $ENV{YAML_DIR})
add_executable(Composer_Verilator main.cc data_server.cc cmd_server.cc)
target_link_libraries(Composer_Verilator PUBLIC APEX::composer yaml-cpp)


message("$ENV{COMPOSER_HARDWARE_DIR}")
if($ENV{COMPOSER_HARDWARE_DIR} EQUAL "")
    message(FATAL_ERROR "COMPOSER_HARDWARE_DIR is not defined")
endif()

if (NOT EXISTS $ENV{COMPOSER_HARDWARE_DIR}/vsim/generated-src/composer.v)
    message(FATAL_ERROR "$COMPOSER_HARDWARE_DIR/vsim/generated-src/composer.v does not exist. Try building your project.")
endif()

verilate(Composer_Verilator
        SOURCES $ENV{COMPOSER_HARDWARE_DIR}/vsim/generated-src/composer.v TOP_MODULE ComposerTop
        VERILATOR_ARGS -LDFLAGS "-undefined dynamic_lookup"
        )