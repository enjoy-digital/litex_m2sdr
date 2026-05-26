cmake_minimum_required(VERSION 3.16)
project(SoapySDRLiteXM2SDR LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(M2SDR_TREE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(M2SDR_ENABLE_LITEPCIE AUTO CACHE STRING "Enable LitePCIe backend for Soapy: AUTO, ON, or OFF")
set_property(CACHE M2SDR_ENABLE_LITEPCIE PROPERTY STRINGS AUTO ON OFF)
option(M2SDR_ENABLE_LITEETH "Enable LiteEth backend for Soapy" ON)

if(M2SDR_ENABLE_LITEPCIE STREQUAL "AUTO")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(M2SDR_LITEPCIE_ENABLED ON)
    else()
        set(M2SDR_LITEPCIE_ENABLED OFF)
    endif()
elseif(M2SDR_ENABLE_LITEPCIE)
    set(M2SDR_LITEPCIE_ENABLED ON)
else()
    set(M2SDR_LITEPCIE_ENABLED OFF)
endif()

if(M2SDR_LITEPCIE_ENABLED AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "LitePCIe Soapy support is Linux-only; configure with -DM2SDR_ENABLE_LITEPCIE=OFF")
endif()

if(NOT M2SDR_ENABLE_LITEETH)
    message(FATAL_ERROR "The current Soapy module build requires LiteEth support; use -DM2SDR_ENABLE_LITEETH=ON")
endif()

find_package(SoapySDR "0.2.1" REQUIRED)

find_path(KERNEL_INCLUDE_DIR csr.h
    REQUIRED
    HINTS ${M2SDR_TREE_ROOT}
    PATH_SUFFIXES litex_m2sdr/kernel software/kernel
    NO_DEFAULT_PATH
)

find_path(USER_INCLUDE_DIR m2sdr_config.h
    REQUIRED
    HINTS ${M2SDR_TREE_ROOT}
    PATH_SUFFIXES litex_m2sdr/libm2sdr software/user/libm2sdr
    NO_DEFAULT_PATH
)

find_path(LITEETH_INCLUDE_DIR liteeth_udp.h
    REQUIRED
    HINTS ${M2SDR_TREE_ROOT}
    PATH_SUFFIXES litex_m2sdr/libliteeth software/user/libliteeth
    NO_DEFAULT_PATH
)

find_path(AD9361_INCLUDE_DIR ad9361/ad9361.h
    REQUIRED
    HINTS ${M2SDR_TREE_ROOT}
    PATH_SUFFIXES litex_m2sdr software/user
    NO_DEFAULT_PATH
)

find_path(LIBM2SDR_INCLUDE_DIR m2sdr.h
    REQUIRED
    HINTS ${M2SDR_TREE_ROOT}
    PATH_SUFFIXES litex_m2sdr/libm2sdr software/user/libm2sdr
    NO_DEFAULT_PATH
)

set(M2SDR_SOAPY_LIBS)
if(TARGET m2sdr_static)
    list(APPEND M2SDR_SOAPY_LIBS m2sdr_static)
elseif(TARGET m2sdr)
    list(APPEND M2SDR_SOAPY_LIBS m2sdr)
else()
    find_library(LIBM2SDR_LIBRARY
        NAMES m2sdr libm2sdr.a
        REQUIRED
        HINTS ${M2SDR_TREE_ROOT}
        PATH_SUFFIXES litex_m2sdr/libm2sdr software/user/libm2sdr
        NO_DEFAULT_PATH
    )
    list(APPEND M2SDR_SOAPY_LIBS ${LIBM2SDR_LIBRARY})
endif()

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
    SOURCES
        LiteXM2SDRDevice.cpp
        LiteXM2SDRStreaming.cpp
        LiteXM2SDRRegistration.cpp
    LIBRARIES ${M2SDR_SOAPY_LIBS}
)

target_include_directories(SoapyLiteXM2SDR PRIVATE
    ${KERNEL_INCLUDE_DIR}
    ${USER_INCLUDE_DIR}
    ${LITEETH_INCLUDE_DIR}
    ${AD9361_INCLUDE_DIR}
    ${LIBM2SDR_INCLUDE_DIR}
)

target_compile_definitions(SoapyLiteXM2SDR PRIVATE
    USE_LITEETH=$<BOOL:${M2SDR_ENABLE_LITEETH}>
    USE_LITEPCIE=$<BOOL:${M2SDR_LITEPCIE_ENABLED}>
)

if(WIN32)
    target_compile_definitions(SoapyLiteXM2SDR PRIVATE WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
    target_link_libraries(SoapyLiteXM2SDR PRIVATE ws2_32)
elseif(NOT MSVC)
    target_link_libraries(SoapyLiteXM2SDR PRIVATE m)
endif()

# Keep AD9361/libm2sdr symbols private to this Soapy module. Gqrx/gr-osmosdr
# loads multiple SDR drivers in one process and some of them export AD9361
# symbols with the same names.
if(CMAKE_SYSTEM_NAME MATCHES "Linux|FreeBSD|OpenBSD|NetBSD" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(SoapyLiteXM2SDR PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SoapyLiteXM2SDR PRIVATE
        -Wno-sign-compare
        -Wno-implicit-fallthrough
        -Wno-maybe-uninitialized
        -Wno-unused-parameter
    )
endif()
