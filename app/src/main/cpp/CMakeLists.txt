cmake_minimum_required(VERSION 3.22.1)
project(minidaw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(oboe REQUIRED CONFIG)

add_library(minidaw SHARED
        native-lib.cpp
        AudioEngine.cpp)

target_compile_options(minidaw PRIVATE -Wall -Wno-unused-function -O3 -ffast-math)

target_link_libraries(minidaw
        oboe::oboe
        android
        log)
