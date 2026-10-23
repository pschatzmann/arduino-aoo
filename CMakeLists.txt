cmake_minimum_required(VERSION 3.22)

# set the project name
project(arduino_aoo)

include(FetchContent)

# Use the arduino-audio-tools library next to this one (e.g. in the Arduino
# libraries folder), so that desktop and Arduino builds use the same version.
# Otherwise it is fetched from GitHub: run cmake with
# -DFETCHCONTENT_UPDATES_DISCONNECTED=OFF to update a fetched copy.
option(AOO_USE_LOCAL_AUDIO_TOOLS "Use ../arduino-audio-tools if available" ON)
set(AOO_LOCAL_AUDIO_TOOLS "${CMAKE_CURRENT_SOURCE_DIR}/../arduino-audio-tools")
if(AOO_USE_LOCAL_AUDIO_TOOLS AND EXISTS "${AOO_LOCAL_AUDIO_TOOLS}/CMakeLists.txt")
    message(STATUS "Using local arduino-audio-tools: ${AOO_LOCAL_AUDIO_TOOLS}")
    set(FETCHCONTENT_SOURCE_DIR_ARDUINO-AUDIO-TOOLS "${AOO_LOCAL_AUDIO_TOOLS}" CACHE PATH "" FORCE)
else()
    unset(FETCHCONTENT_SOURCE_DIR_ARDUINO-AUDIO-TOOLS CACHE)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
endif()

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

# automated tests: run with ctest from the build directory
enable_testing()
add_test(NAME protocols COMMAND protocol)
add_test(NAME loopback COMMAND loopback)
add_test(NAME unit-tests COMMAND unit-tests)
add_test(NAME regression-tests COMMAND regression-tests)
set_tests_properties(protocols loopback unit-tests PROPERTIES LABELS unit TIMEOUT 60)
set_tests_properties(regression-tests PROPERTIES LABELS regression TIMEOUT 60)

# Optional interoperability test with the official aoo library (fetched from
# git.iem.at and built without Opus): ctest -L interop
option(AOO_BUILD_INTEROP_TESTS "Build the interop test with the official aoo library" OFF)
if(AOO_BUILD_INTEROP_TESTS)
    FetchContent_Declare(official_aoo GIT_REPOSITORY "https://git.iem.at/aoo/aoo.git" GIT_TAG master)
    FetchContent_GetProperties(official_aoo)
    if(NOT official_aoo_POPULATED)
        FetchContent_Populate(official_aoo)
        set(AOO_USE_OPUS OFF CACHE BOOL "" FORCE)
        set(AOO_INSTALL_LIBRARY OFF CACHE BOOL "" FORCE)
        add_subdirectory(${official_aoo_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/official_aoo EXCLUDE_FROM_ALL)
    endif()
    foreach(program official_sender official_sink)
        add_executable(${program} examples/tests/interop/official/${program}.cpp)
        set_target_properties(${program} PROPERTIES CXX_STANDARD 17)
        target_include_directories(${program} PRIVATE ${official_aoo_SOURCE_DIR}/include ${official_aoo_SOURCE_DIR})
        target_link_libraries(${program} aoo)
    endforeach()
    add_test(NAME interop
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/examples/tests/interop/run-interop.sh
            $<TARGET_FILE:interop> $<TARGET_FILE:official_sender> $<TARGET_FILE:official_sink>)
    set_tests_properties(interop PROPERTIES LABELS interop TIMEOUT 60 RUN_SERIAL TRUE)
endif()

