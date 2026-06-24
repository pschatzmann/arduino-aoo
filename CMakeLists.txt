cmake_minimum_required(VERSION 3.22)

# set the project name
project(arduino_aoo)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(FetchContent)

FetchContent_Declare(arduino-audio-tools GIT_REPOSITORY "https://github.com/pschatzmann/arduino-audio-tools.git" GIT_TAG main )
FetchContent_GetProperties(arduino-audio-tools)
if(NOT arduino-audio-tools_POPULATED)
    FetchContent_Populate(arduino-audio-tools)
    add_subdirectory(${arduino-audio-tools_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arduino-audio-tools)
endif()

add_library(arduino_aoo INTERFACE)
target_include_directories(arduino_aoo INTERFACE src)

# build all examples
add_subdirectory("examples")

