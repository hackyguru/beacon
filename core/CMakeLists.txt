cmake_minimum_required(VERSION 3.14)
project(BeaconCorePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# No media libraries here on purpose: the streamer's own software (OBS, ffmpeg,
# vMix…) does the encoding, and the viewer's WebView does the decoding. Beacon
# only moves already-encoded bytes. libsodium is for station identity — every
# fragment is signed so a station cannot be impersonated on its own topic.
find_package(PkgConfig REQUIRED)
pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)

logos_module(
    NAME beacon_core
    SOURCES
        src/beacon_core_impl.h
        src/beacon_core_impl.cpp
        src/http_server.h
        src/http_server.cpp
        src/ingest.h
        src/ingest.cpp
        src/station.h
        src/station.cpp
        src/stream_buffer.h
        src/stream_buffer.cpp
        src/wire.h
        src/mpegts_js.h
        src/mpegts_js.cpp
    LINK_LIBRARIES
        PkgConfig::sodium
)
