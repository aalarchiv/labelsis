cmake_minimum_required(VERSION 3.16)
project(pt700_oracle C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

add_executable(ptouch-770-capture ptouch-770-capture.c)
