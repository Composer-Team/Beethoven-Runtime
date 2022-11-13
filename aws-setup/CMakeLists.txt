project(composer-fpga-utils)
cmake_minimum_required(VERSION 3.13)

include_directories("$ENV{COMPOSER_AWS_SDK_DIR}/sdk/userspace/include")
include_directories(include)
add_library(composer-fpga-utils SHARED
        src/fpga_utils.c)
target_include_directories(composer-fpga-utils INTERFACE include)
target_link_options(composer-fpga-utils PRIVATE -lfpga_mgmt -lpthread -lrt)