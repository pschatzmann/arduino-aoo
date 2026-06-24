cmake_minimum_required(VERSION 3.20)

# set the project name
project(aoo-source)
set (CMAKE_CXX_STANDARD 11)
set (DCMAKE_CXX_FLAGS "-Werror")

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(FetchContent)

# use arduino_libopus
FetchContent_Declare(arduino_libopus GIT_REPOSITORY "https://github.com/pschatzmann/arduino-libopus.git" GIT_TAG main )
FetchContent_GetProperties(arduino_libopus)
if(NOT arduino_libopus_POPULATED)
    FetchContent_Populate(arduino_libopus)
    add_subdirectory(${arduino_libopus_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/arduino_libopus)
endif()


# build sketch as executable
set_source_files_properties(aoo-source.ino PROPERTIES LANGUAGE CXX)
add_executable (aoo-source aoo-source.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(aoo-source PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(aoo-source arduino_libopus arduino_emulator arduino-audio-tools arduino_aoo)