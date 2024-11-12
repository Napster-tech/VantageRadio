cmake_minimum_required(VERSION 3.1.0)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/radio_control_private)

if (CMAKE_ANDROID_NDK)
    find_package(Threads)
    include_directories(${PROJECT_SOURCE_DIR}/libs/libssh/android_libssh/include/)

    ADD_LIBRARY(crypto SHARED IMPORTED)
    SET_TARGET_PROPERTIES(crypto PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libs/libssh/android_libssh/libcrypto.so)
    ADD_LIBRARY(decrepit SHARED IMPORTED)
    SET_TARGET_PROPERTIES(decrepit PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libs/libssh/android_libssh/libdecrepit.so)
    ADD_LIBRARY(ssh SHARED IMPORTED)
    SET_TARGET_PROPERTIES(ssh PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libs/libssh/android_libssh/libssh.so)
    ADD_LIBRARY(ssl SHARED IMPORTED)
    SET_TARGET_PROPERTIES(ssl PROPERTIES IMPORTED_LOCATION ${PROJECT_SOURCE_DIR}/libs/libssh/android_libssh/libssl.so)

    add_library(radio-control HelixControl.cpp
                RadioControl.cpp
                MicrohardControl.cpp
                DTCControl.cpp
                PoplarControl.cpp

                FakeControl.cpp
                radio_control_private/ByteArray.cpp
                radio_control_private/Crc32.cpp
                radio_control_private/DeviceDiscoveryProtocol.cpp
                radio_control_private/DeviceDiscoveryProtocolMessage.cpp
                radio_control_private/Encryption.cpp
                radio_control_private/HostAddress.cpp
                radio_control_private/MacAddress.cpp
                radio_control_private/Network.cpp
                radio_control_private/OperatingSystem.cpp
                radio_control_private/RandomNumber.cpp
                radio_control_private/StrUtils.cpp
                radio_control_private/Trace.cpp
                radio_control_private/UdpBroadcaster.cpp
                radio_control_private/UdpSocket.cpp)
    target_link_libraries(radio-control
            PRIVATE
            Threads::Threads
            ssl
            ssh
            decrepit
            crypto)
    target_compile_options(radio-control PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
            )
else()
    project (radio-control CXX)
    find_package(Threads)
    #TODO: Make source fortification definition only happen outside of Yocto build.
    #add_definitions(-D_FORTIFY_SOURCE=1)

    #find_package(OpenSSL REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    #set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL REQUIRED)

    #add_library(blowfish SHARED dtc/blowfish.c)
    #target_include_directories(blowfish PRIVATE ./dtc)
    #set_target_properties(blowfish PROPERTIES LINKER_LANGUAGE CXX)

    add_executable(radio-control-bin
                        test.cpp
                        HelixControl.cpp
                        RadioControl.cpp
                        MicrohardControl.cpp
            PoplarControl.cpp
                        FakeControl.cpp
                        DTCControl.cpp
                        radio_control_private/ByteArray.cpp
                        radio_control_private/Crc32.cpp
                        radio_control_private/DeviceDiscoveryProtocol.cpp
                        radio_control_private/DeviceDiscoveryProtocolMessage.cpp
                        radio_control_private/Encryption.cpp
                        radio_control_private/HostAddress.cpp
                        radio_control_private/MacAddress.cpp
                        radio_control_private/Network.cpp
                        radio_control_private/OperatingSystem.cpp
                        radio_control_private/RandomNumber.cpp
                        radio_control_private/StrUtils.cpp
                        radio_control_private/Trace.cpp
                        radio_control_private/UdpBroadcaster.cpp
                        radio_control_private/UdpSocket.cpp)

    target_link_libraries(radio-control-bin PRIVATE Threads::Threads OpenSSL::Crypto ssh)
    target_compile_options(radio-control-bin PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
            )
endif()

