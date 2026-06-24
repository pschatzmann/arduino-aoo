cmake_minimum_required(VERSION 3.20)

# set the project name
project(loopback)
set (CMAKE_CXX_STANDARD 11)
set (DCMAKE_CXX_FLAGS "-Werror")

include(FetchContent)


# build sketch as executable
set_source_files_properties(loopback.ino PROPERTIES LANGUAGE CXX)
add_executable (loopback loopback.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(loopback PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(loopback arduino_emulator arduino-audio-tools arduino_aoo)
