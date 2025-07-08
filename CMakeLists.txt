cmake_minimum_required(VERSION 3.10)
project(MyProject VERSION 1.0)

# setup C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(client)
add_subdirectory(deps/src/libdatachannel-0.23.1)