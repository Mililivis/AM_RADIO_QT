cmake_minimum_required(VERSION 3.16)
project(AM_RADIO_QT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)

qt_standard_project_setup()

# RTL-SDR driver (librtlsdr wrapper), bundled in SDR/
set(SDR_DRIVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDR" CACHE PATH "Path to the SDR driver project")
add_subdirectory(${SDR_DRIVER_DIR} ${CMAKE_BINARY_DIR}/sdr_driver EXCLUDE_FROM_ALL)

qt_add_executable(am_radio
    src/main.cpp
    src/AudioOutput.cpp
    src/Dsp.cpp
    src/MainWindow.cpp
    src/RadioEngine.cpp
    src/Scanner.cpp
    src/SpectrumWidget.cpp
)

target_link_libraries(am_radio PRIVATE Qt6::Widgets PkgConfig::PULSE rtlsdr_driver)
target_compile_options(am_radio PRIVATE -Wall -Wextra)
