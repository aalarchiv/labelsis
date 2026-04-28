cmake_minimum_required(VERSION 3.16)

# Pull in ESP-IDF's project machinery. Source ~/esp/esp-idf/export.sh first
# (or run scripts/idf-env.sh) so IDF_PATH is set.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pt700)
