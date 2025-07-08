cmake_minimum_required(VERSION 3.10)
project(P2PTransfer)

set(CMAKE_CXX_STANDARD 17)

find_package(OpenSSL REQUIRED)

# Source files
add_executable(p2ptransfer
    Cert.cpp
    P2PTransfer.cpp
)

# find all include dir
file(GLOB_RECURSE INCLUDE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/*/include")
list(REMOVE_DUPLICATES INCLUDE_DIRS)

# Include directories
include_directories(
    ${INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(p2ptransfer PRIVATE
    datachannel
    srtp2
    usrsctp
    juice
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# Compiler options
option(DEBUG "Build with debug symbols" OFF)
if(DEBUG)
    target_compile_options(p2ptransfer PRIVATE -g)
else()
    target_compile_options(p2ptransfer PRIVATE -O3)
endif()
