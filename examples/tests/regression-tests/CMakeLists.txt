cmake_minimum_required(VERSION 3.20)

# set the project name
project(regression-tests)
set (CMAKE_CXX_STANDARD 11)

# build sketch as executable
set_source_files_properties(regression-tests.ino PROPERTIES LANGUAGE CXX)
add_executable (regression-tests regression-tests.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(regression-tests PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(regression-tests arduino_emulator arduino-audio-tools arduino_aoo)
