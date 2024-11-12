cmake_minimum_required(VERSION 3.8)
project(mpegts)
set(CMAKE_CXX_STANDARD 17)

#Macro for printing all CMake variables
macro(print_all_variables)
    message(STATUS "print_all_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        message(STATUS "${_variableName}=${${_variableName}}")
    endforeach()
    message(STATUS "print_all_variables------------------------------------------}")
endmacro()

find_package (Threads REQUIRED)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mpegts/)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/unit_tests/)

file(GLOB libfiles ${CMAKE_CURRENT_SOURCE_DIR}/mpegts/*.cpp)
add_library(mpegts ${libfiles})
