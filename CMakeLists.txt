cmake_minimum_required(VERSION 3.16)
project(AM_RADIO_QT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(AM_RADIO_BUILD_TESTS "Build unit tests of the core" ON)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse-simple)
pkg_check_modules(FFTW IMPORTED_TARGET fftw3f)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(WARNINGS -Wall -Wextra)

# ---------------------------------------------------------------------------
# Layer 0: hardware driver (librtlsdr wrapper)
# ---------------------------------------------------------------------------
add_subdirectory(SDR ${CMAKE_BINARY_DIR}/sdr_driver EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Layer 1a: DSP, pure C++ (no Qt, no hardware)
# ---------------------------------------------------------------------------
add_library(radio_dsp STATIC
    core/dsp/AmDemodulator.cpp
    core/dsp/Fft.cpp
    core/dsp/Filters.cpp
    core/dsp/Rebin.cpp
)
target_include_directories(radio_dsp PUBLIC core)
target_compile_options(radio_dsp PRIVATE ${WARNINGS})
if(FFTW_FOUND)
    target_compile_definitions(radio_dsp PRIVATE HAVE_FFTW)
    target_link_libraries(radio_dsp PRIVATE PkgConfig::FFTW)
    message(STATUS "FFT backend: FFTW ${FFTW_VERSION}")
else()
    message(STATUS "FFT backend: built-in (install libfftw3-dev for a faster one)")
endif()

# ---------------------------------------------------------------------------
# Layer 1b: SDR core, no Qt: sources, dispatcher, sinks (receiver, analyzer, recorder), scanner
# ---------------------------------------------------------------------------
add_library(radio_core STATIC
    core/AmReceiver.cpp
    core/AsyncSink.cpp
    core/AudioOutput.cpp
    core/FileSource.cpp
    core/IqDispatcher.cpp
    core/IqRecorder.cpp
    core/RtlSdrSource.cpp
    core/Scanner.cpp
    core/SpectrumAnalyzer.cpp
)
target_include_directories(radio_core PUBLIC core)
target_link_libraries(radio_core PUBLIC radio_dsp rtlsdr_driver PkgConfig::PULSE Threads::Threads)
target_compile_options(radio_core PRIVATE ${WARNINGS})

# ---------------------------------------------------------------------------
# Layer 1c: IQ network transport (rtl_tcp server), separate module, POSIX only.
# Touches the core only through the IqSink interface (NetworkIqSink adapter).
# ---------------------------------------------------------------------------
add_library(iq_net STATIC
    net/RtlTcpServer.cpp
)
target_include_directories(iq_net PUBLIC net core)
target_link_libraries(iq_net PUBLIC Threads::Threads)
target_compile_options(iq_net PRIVATE ${WARNINGS})

# ---------------------------------------------------------------------------
# Layers 2 + 3: application controller (Qt Core) and GUI (Qt Widgets)
# ---------------------------------------------------------------------------
qt_add_executable(am_radio
    app/main.cpp
    app/RadioController.cpp
    ui/FrequencyPlot.cpp
    ui/FrequencyView.cpp
    ui/MainWindow.cpp
    ui/SpectrumWidget.cpp
    ui/WaterfallWidget.cpp
)
target_include_directories(am_radio PRIVATE app ui)
target_link_libraries(am_radio PRIVATE radio_core iq_net Qt6::Widgets)
target_compile_options(am_radio PRIVATE ${WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AM_RADIO_BUILD_TESTS)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE radio_core iq_net)
    target_compile_options(core_tests PRIVATE ${WARNINGS})

    # Needs a connected dongle: ./build/live_test [station kHz]
    qt_add_executable(live_test tests/live_test.cpp app/RadioController.cpp)
    target_include_directories(live_test PRIVATE app)
    target_link_libraries(live_test PRIVATE radio_core iq_net Qt6::Core)
    target_compile_options(live_test PRIVATE ${WARNINGS})
endif()
