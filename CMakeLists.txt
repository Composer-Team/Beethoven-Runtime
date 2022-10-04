cmake_minimum_required(VERSION 3.10)
project(Composer_Verilator)

set(CMAKE_CXX_STANDARD 17)

find_package(verilator REQUIRED)
find_package(composer REQUIRED)
message("${verilator_FOUND}")

add_executable(Composer_Verilator main.cc data_server.cc cmd_server.cc)
target_link_libraries(Composer_Verilator APEX::composer)

verilate(Composer_Verilator SOURCES composer.v TOP_MODULE ComposerTop)
