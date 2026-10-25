cmake_minimum_required(VERSION 3.20)

# Command line tool to send and receive audio with AOO on the desktop
project(aoo-tool)
set(CMAKE_CXX_STANDARD 11)

include(FetchContent)

# Opus codec
if(NOT TARGET arduino_libopus)
    FetchContent_Declare(arduino_libopus GIT_REPOSITORY "https://github.com/pschatzmann/arduino-libopus.git" GIT_TAG main )
    FetchContent_GetProperties(arduino_libopus)
    if(NOT arduino_libopus_POPULATED)
        FetchContent_Populate(arduino_libopus)
        add_subdirectory(${arduino_libopus_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arduino_libopus)
    endif()
endif()

# libhelix: MP3 and AAC decoder for the file input
if(NOT TARGET arduino_helix)
    FetchContent_Declare(arduino_helix GIT_REPOSITORY "https://github.com/pschatzmann/codec-helix.git" GIT_TAG main )
    FetchContent_GetProperties(arduino_helix)
    if(NOT arduino_helix_POPULATED)
        FetchContent_Populate(arduino_helix)
        add_subdirectory(${arduino_helix_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arduino_helix)
    endif()
    target_compile_definitions(arduino_helix PUBLIC -DARDUINO -DHELIX_LOGGING_ACTIVE=0)
endif()

# miniaudio for the microphone and the speaker
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/miniaudio/miniaudio.h)
    file(DOWNLOAD https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h
        ${CMAKE_CURRENT_BINARY_DIR}/miniaudio/miniaudio.h)
endif()

# the target name aoo is used by the official aoo library (interop test)
add_executable(aoo-tool aoo/main.cpp aoo/aoo.cpp)
set_target_properties(aoo-tool PROPERTIES OUTPUT_NAME aoo)
target_compile_definitions(aoo-tool PUBLIC -DARDUINO -DIS_DESKTOP)
target_include_directories(aoo-tool PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/miniaudio)
target_link_libraries(aoo-tool arduino_libopus arduino_helix arduino_emulator arduino-audio-tools arduino_aoo)
