cmake_minimum_required(VERSION 3.20)

# set the project name
project(stream-test)
set (CMAKE_CXX_STANDARD 11)

# build sketch as executable
set_source_files_properties(stream-test.ino PROPERTIES LANGUAGE CXX)
add_executable (stream-test stream-test.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(stream-test PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(stream-test arduino_libopus arduino_emulator arduino-audio-tools arduino_aoo)
