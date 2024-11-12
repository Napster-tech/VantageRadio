cmake_minimum_required(VERSION 3.1.0)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(h265nal/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/mpegts/mpegts/)

project (vrts CXX)
find_package(Threads)

add_subdirectory(h265nal)
add_subdirectory(mpegts)

add_executable(vrts-test test.cpp FakeRadioLink.cpp VRTS.cpp)

add_library(vrts STATIC VRTS.cpp)
target_link_libraries(vrts PUBLIC Threads::Threads h265nal mpegts)
target_link_libraries(vrts-test PRIVATE Threads::Threads h265nal vrts)
