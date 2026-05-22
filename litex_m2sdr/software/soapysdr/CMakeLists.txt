cmake_minimum_required(VERSION 3.16)
project(SoapySDRLiteXM2SDR CXX C)

set(CMAKE_CXX_STANDARD 17)
set(M2SDR_TREE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

########################################################################
## Kernel CSR discovery
########################################################################

find_path(KERNEL_INCLUDE_DIR csr.h
          REQUIRED
          HINTS
            ${M2SDR_TREE_ROOT}
          PATH_SUFFIXES
            litex_m2sdr/kernel
            software/kernel
          NO_DEFAULT_PATH
)

include_directories(${KERNEL_INCLUDE_DIR})

########################################################################
## LiteEth discovery
########################################################################

find_path(LITEETH_INCLUDE_DIR liteeth_udp.h
          REQUIRED
          HINTS
            ${M2SDR_TREE_ROOT}
          PATH_SUFFIXES
            litex_m2sdr/libliteeth
            software/user/libliteeth
          NO_DEFAULT_PATH
)

include_directories(${LITEETH_INCLUDE_DIR})

########################################################################
## USER discovery
########################################################################

find_path(USER_INCLUDE_DIR m2sdr_config.h
          REQUIRED
          HINTS
            ${M2SDR_TREE_ROOT}
          PATH_SUFFIXES
            litex_m2sdr/libm2sdr
            software/user/libm2sdr
          NO_DEFAULT_PATH
)

include_directories(${USER_INCLUDE_DIR})

########################################################################
## AD9361 discovery
########################################################################

find_path(AD9361_INCLUDE_DIR ad9361/ad9361.h
          REQUIRED
          HINTS
            ${M2SDR_TREE_ROOT}
          PATH_SUFFIXES
            litex_m2sdr
            software/user
          NO_DEFAULT_PATH
)

include_directories(${AD9361_INCLUDE_DIR})

########################################################################
## LIBM2SDR discovery
########################################################################

find_path(LIBM2SDR_INCLUDE_DIR m2sdr.h
          REQUIRED
          HINTS
            ${M2SDR_TREE_ROOT}
          PATH_SUFFIXES
            litex_m2sdr/libm2sdr
            software/user/libm2sdr
          NO_DEFAULT_PATH
)

include_directories(${LIBM2SDR_INCLUDE_DIR})

find_library(LIBM2SDR_STATIC_LIBRARY libm2sdr.a
             REQUIRED
             HINTS
               ${M2SDR_TREE_ROOT}
             PATH_SUFFIXES
               litex_m2sdr/libm2sdr
               software/user/libm2sdr
             NO_DEFAULT_PATH
)

find_package(SoapySDR "0.2.1" REQUIRED)

add_definitions(-DUSE_LITEPCIE=1 -DUSE_LITEETH=1)

SOAPY_SDR_MODULE_UTIL(
    TARGET SoapyLiteXM2SDR
    SOURCES LiteXM2SDRDevice.cpp LiteXM2SDRStreaming.cpp
    LiteXM2SDRRegistration.cpp
    LIBRARIES ${LIBM2SDR_STATIC_LIBRARY} m
)

# Keep AD9361/libm2sdr symbols private to this Soapy module. Gqrx/gr-osmosdr
# loads multiple SDR drivers in one process and some of them export AD9361
# symbols with the same names.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(SoapyLiteXM2SDR PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
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
