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
## LMS7002MNG discovery
########################################################################

#message(STATUS "Looking for LMS7002MNG in ${LMS7002MNG_ROOT}...")
#
#find_path(LMS7002MNG_INCLUDE_DIR lms7002mNG/LMS7002M.h
#          HINTS ${LMS7002MNG_ROOT}
#          PATH_SUFFIXES include
#          REQUIRED)
#
#find_library(LMS7002MNG_LIBRARY lms7002mNG
#             HINTS ${LMS7002MNG_ROOT}
#             PATH_SUFFIXES lib
#             REQUIRED)
#
#include_directories(${LMS7002MNG_INCLUDE_DIR})


########################################################################
## SoapySDR library
########################################################################

find_package(SoapySDR "0.2.1" REQUIRED)

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
	SOURCES LiteXM2SDRDevice.cpp LiteXM2SDRStreaming.cpp LiteXM2SDRRegistration.cpp
	LIBRARIES ${LITEPCIE_LIBRARY} m
)
