cmake_minimum_required(VERSION 3.17)
project(SoapySDRLiteXM2SDR CXX C)

set(CMAKE_CXX_STANDARD 17)

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

find_path(USER_INCLUDE_DIR m2sdr_config.h
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

find_library(AD9361_LIBRARY ad9361_m2sdr
             REQUIRED
             HINTS
               ${CMAKE_CURRENT_SOURCE_DIR}/../..
             PATH_SUFFIXES
               software/user/ad9361)

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

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
	SOURCES LiteXM2SDRDevice.cpp LiteXM2SDRStreaming.cpp LiteXM2SDRRegistration.cpp
	LIBRARIES ${LITEPCIE_LIBRARY} ${LIBM2SDR_LIBRARY} ${AD9361_LIBRARY} m
)

