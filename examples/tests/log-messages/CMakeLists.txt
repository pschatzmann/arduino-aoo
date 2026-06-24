cmake_minimum_required(VERSION 3.20)

# set the project name
project(log-messages)
set (CMAKE_CXX_STANDARD 11)
set (DCMAKE_CXX_FLAGS "-Werror")

include(FetchContent)


# build sketch as executable
set_source_files_properties(log-messages.ino PROPERTIES LANGUAGE CXX)
add_executable (log-messages log-messages.ino)

# set preprocessor defines
target_compile_definitions(arduino_emulator PUBLIC -DDEFINE_MAIN)
target_compile_definitions(log-messages PUBLIC -DARDUINO -DIS_DESKTOP)

# specify libraries
target_link_libraries(log-messages arduino_emulator arduino-audio-tools arduino_aoo)