
# Vantage Radio Configuration for Vesper and Trace

This README provides a guide for setting up and configuring the Vantage Radio application, allowing users to control and configure settings like frequency, bandwidth, power, network ID, and encryption ID for Vesper and Trace devices. This application is developed in Android Studio, leveraging native C++ code files and headers for device configuration. The project is set up with CMake to manage the native code integration.

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Setting Up Android Studio](#setting-up-android-studio)
4. [Project Configuration](#project-configuration)
5. [Configuring the CMake Build](#configuring-the-cmake-build)
6. [Building and Running the Application](#building-and-running-the-application)
7. [Configuration Options](#configuration-options)
8. [Troubleshooting](#troubleshooting)

---

## Overview

The Vantage Radio application provides a user interface for configuring Vesper and Trace radio devices. Users can adjust:

- **Frequency**: Set the operating frequency.
- **Bandwidth**: Define the transmission bandwidth.
- **Power**: Configure transmission power.
- **Network ID**: Set the network identifier.
- **Encryption ID**: Specify the encryption level for secure communications.

## Prerequisites

1. **Android Studio**: Ensure Android Studio is installed, ideally the latest stable version.
2. **CMake**: The project uses CMake for building and linking C++ files within the project. Make sure CMake is installed and configured within Android Studio.
3. **Vesper and Trace C++ Code Files**: Ensure you have the required `.cpp` and `.h` files that handle the Vesper and Trace device configurations. Place these files in the appropriate project directory.

## Setting Up Android Studio

1. **Open the Project**:
   - Launch Android Studio.
   - Open the Vantage Radio project by selecting **File > Open** and navigating to the project folder.

2. **Configure SDK and NDK**:
   - Go to **File > Project Structure > SDK Location**.
   - Set the **Android SDK Location** and **Android NDK Location**.

3. **CMake and NDK**:
   - Navigate to **File > Settings > Appearance & Behavior > System Settings > Android SDK > SDK Tools**.
   - Ensure **CMake** and **NDK** are installed and checked.

## Project Configuration

The core configuration files for frequency, bandwidth, power, network ID, and encryption ID are implemented in C++ source files that interact directly with Vesper and Trace devices. The native C++ code is integrated within the Android application using JNI (Java Native Interface).

### Key Files

- **MainActivity.java**: Contains the main logic and UI for user interactions.
- **RadioConfig.cpp** and **RadioConfig.h**: Implement the configuration functions for Vesper and Trace devices.
- **CMakeLists.txt**: The CMake configuration file to build and link the native C++ code.

## Configuring the CMake Build

1. **Locate the CMakeLists.txt File**:
   - Open the **CMakeLists.txt** file in the root of your project directory.

2. **Add Source and Header Files**:
   - Add the Vesper and Trace `.cpp` and `.h` files to the `CMakeLists.txt` to ensure they are compiled and linked.
   - Here’s an example configuration for `CMakeLists.txt`:
     ```cmake
     cmake_minimum_required(VERSION 3.10)

     project(VantageRadio)

     # Add native source and header files
     set(SOURCE_FILES
         src/main/cpp/RadioConfig.cpp
         src/main/cpp/vesper_device.cpp
         src/main/cpp/trace_device.cpp)

     add_library(native-lib SHARED ${SOURCE_FILES})

     # Include directories for headers
     include_directories(src/main/cpp)

     # Link the native library
     target_link_libraries(native-lib ${log-lib})
     ```

3. **Sync Gradle**:
   - After editing **CMakeLists.txt**, sync your project with Gradle by selecting **File > Sync Project with Gradle Files**.

## Building and Running the Application

1. **Build the Application**:
   - Select **Build > Make Project** or click the **Run** button in the toolbar to build the application with the C++ configurations.

2. **Run on Device**:
   - Connect an Android device or use an emulator.
   - Click **Run** to install and launch the app on your device or emulator.

3. **Test Configurations**:
   - Once the app is running, input the values for frequency, bandwidth, power, network ID, and encryption ID.
   - Apply settings to verify communication with the Vesper and Trace devices.

## Configuration Options

Within the application, you can configure the following settings:

- **Frequency**: Set the frequency for radio operation.
- **Bandwidth**: Specify the bandwidth for transmission.
- **Power**: Adjust the transmission power level.
- **Network ID**: Assign a unique identifier for the network.
- **Encryption ID**: Set the encryption level for secure communication.

Each setting can be modified within the application and applied to the Vesper and Trace devices using the C++ methods implemented in `RadioConfig.cpp`.

## Troubleshooting

- **Build Errors**: Double-check that all paths in **CMakeLists.txt** are correct and that the required C++ files are included.
- **CMake Issues**: Verify that CMake syntax is accurate and that dependencies are correctly linked.
- **JNI Errors**: Make sure your Java classes have the correct native methods declared and that they match the signatures in the C++ code.
- **Connection Issues**: Confirm that the Vesper and Trace devices are connected and configured for communication.

---

This README provides a foundational setup for configuring and building the Vantage Radio application. For more advanced configurations, refer to the comments within the source code files.
