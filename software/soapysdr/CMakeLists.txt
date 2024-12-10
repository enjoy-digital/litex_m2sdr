cmake_minimum_required(VERSION 3.16)
project(SoapySDRLiteXM2SDR CXX C)

set(CMAKE_CXX_STANDARD 17)

option(WITH_ETH_STREAM "Uses Ethernet interface for the stream" OFF)
option(WITH_ETH_CTRL   "Uses Ethernet interface for the control" OFF)

########################################################################
## LitePCIe discovery
########################################################################

find_path(LITEPCIE_KERNEL_INCLUDE_DIR litepcie.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            software/kernel)
find_path(LITEPCIE_USER_INCLUDE_DIR liblitepcie.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            software/user/liblitepcie)
set(LITEPCIE_INCLUDE_DIR ${LITEPCIE_KERNEL_INCLUDE_DIR} ${LITEPCIE_USER_INCLUDE_DIR})
find_library(LITEPCIE_LIBRARY litepcie
             REQUIRED
             HINTS
               ${CMAKE_CURRENT_SOURCE_DIR}/../..
             PATH_SUFFIXES
               software/user/liblitepcie)

include_directories(${LITEPCIE_INCLUDE_DIR})

########################################################################
## USER discovery
########################################################################

find_path(USER_INCLUDE_DIR libm2sdr/m2sdr_config.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            software/user)

include_directories(${USER_INCLUDE_DIR})

########################################################################
## AD9361 discovery
########################################################################

find_path(AD9361_INCLUDE_DIR ad9361.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            software/user/ad9361)

include_directories(${AD9361_INCLUDE_DIR})

########################################################################
## LIBM2SDR discovery
########################################################################

find_path(LIBM2SDR_INCLUDE_DIR libm2sdr.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            software/user/libm2sdr)

include_directories(${LIBM2SDR_INCLUDE_DIR})

find_library(LIBM2SDR_LIBRARY m2sdr
             REQUIRED
             HINTS
               ${CMAKE_CURRENT_SOURCE_DIR}/../..
             PATH_SUFFIXES
               software/user/libm2sdr)

########################################################################
## SoapySDR library
########################################################################

find_package(SoapySDR "0.2.1" REQUIRED)

set(LITEXM2SDR_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../software/user/ad9361/ad9361_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../software/user/ad9361/ad9361.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../software/user/ad9361/ad9361_conv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../software/user/ad9361/util.c
)

if(WITH_ETH_STREAM)
    add_definitions(-DWITH_ETH_STREAM=1)
endif()

if(WITH_ETH_CTRL)
	add_definitions(-DWITH_ETH_CTRL=1)
endif()

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
    SOURCES LiteXM2SDRDevice.cpp LiteXM2SDRStreaming.cpp
    LiteXM2SDRRegistration.cpp etherbone.c
	LiteXM2SDRUDPRx.cpp
	${LITEXM2SDR_SOURCE}
    LIBRARIES ${LITEPCIE_LIBRARY} ${LIBM2SDR_LIBRARY} m
)

# Suppress specific warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SoapyLiteXM2SDR PRIVATE
        -Wno-sign-compare
        -Wno-implicit-fallthrough
        -Wno-maybe-uninitialized
        -Wno-unused-parameter
    )
endif()
