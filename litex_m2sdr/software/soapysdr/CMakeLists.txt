cmake_minimum_required(VERSION 3.16)
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
            litex_m2sdr/kernel
            software/kernel
)
find_path(LITEPCIE_USER_INCLUDE_DIR liblitepcie.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            litex_m2sdr/liblitepcie
            software/user/liblitepcie
)
set(LITEPCIE_INCLUDE_DIR ${LITEPCIE_KERNEL_INCLUDE_DIR} ${LITEPCIE_USER_INCLUDE_DIR})

include_directories(${LITEPCIE_INCLUDE_DIR})

########################################################################
## LiteEth discovery (added)
########################################################################

find_path(LITEETH_INCLUDE_DIR etherbone.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            litex_m2sdr/libliteeth
            software/user/libliteeth
)

include_directories(${LITEETH_INCLUDE_DIR})

########################################################################
## USER discovery
########################################################################

find_path(USER_INCLUDE_DIR m2sdr_config.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            litex_m2sdr/libm2sdr
            software/user/libm2sdr
)

include_directories(${USER_INCLUDE_DIR})

########################################################################
## AD9361 discovery
########################################################################

find_path(AD9361_INCLUDE_DIR ad9361/ad9361.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            litex_m2sdr
            software/user
)

include_directories(${AD9361_INCLUDE_DIR})

########################################################################
## LIBM2SDR discovery
########################################################################

find_path(LIBM2SDR_INCLUDE_DIR libm2sdr.h
          REQUIRED
          HINTS
            ${CMAKE_CURRENT_SOURCE_DIR}/../..
          PATH_SUFFIXES
            litex_m2sdr/libm2sdr
            software/user/libm2sdr
)

include_directories(${LIBM2SDR_INCLUDE_DIR})

find_library(LIBM2SDR_LIBRARY m2sdr
             REQUIRED
             HINTS
               ${CMAKE_CURRENT_SOURCE_DIR}/../..
             PATH_SUFFIXES
               litex_m2sdr/libm2sdr
               software/user/libm2sdr
)

find_package(SoapySDR "0.2.1" REQUIRED)

add_definitions(-DUSE_LITEPCIE=1 -DUSE_LITEETH=1)

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
    SOURCES LiteXM2SDRDevice.cpp LiteXM2SDRStreaming.cpp
    LiteXM2SDRRegistration.cpp
    LIBRARIES ${LIBM2SDR_LIBRARY} m
)

# Bind internal symbol references to this module to avoid conflicts
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(SoapyLiteXM2SDR PRIVATE -Wl,-Bsymbolic)
endif()

# Suppress specific warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SoapyLiteXM2SDR PRIVATE
        -Wno-sign-compare
        -Wno-implicit-fallthrough
        -Wno-maybe-uninitialized
        -Wno-unused-parameter
    )
endif()
