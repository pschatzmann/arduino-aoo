cmake_minimum_required(VERSION 3.20)

# set the project name
project(unit-tests)
set (CMAKE_CXX_STANDARD 11)

# build sketch as executable
set_source_files_properties(unit-tests.ino PROPERTIES LANGUAGE CXX)
add_executable (unit-tests unit-tests.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(unit-tests PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(unit-tests arduino_emulator arduino-audio-tools arduino_aoo)
