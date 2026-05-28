# Top-level CMakeLists.txt
#
# PlatformIO generates the ESP-IDF project glue around this file. We just have
# to point ESP-IDF at the main component (which lives in src/ per the
# PlatformIO + ESP-IDF convention).

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(microfi)
