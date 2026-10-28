cmake_minimum_required(VERSION 3.16)
project(SDR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTLSDR REQUIRED IMPORTED_TARGET librtlsdr)
find_package(Threads REQUIRED)

# Driver library
add_library(rtlsdr_driver
    src/RtlSdrDriver.cpp
)
target_include_directories(rtlsdr_driver PUBLIC include)
target_link_libraries(rtlsdr_driver PUBLIC PkgConfig::RTLSDR Threads::Threads)
target_compile_options(rtlsdr_driver PRIVATE -Wall -Wextra)

# Demo / test utility
add_executable(sdr_test src/main.cpp)
target_link_libraries(sdr_test PRIVATE rtlsdr_driver)
target_compile_options(sdr_test PRIVATE -Wall -Wextra)
