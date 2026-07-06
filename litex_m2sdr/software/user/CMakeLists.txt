cmake_minimum_required(VERSION 3.16)

project(litex_m2sdr_user VERSION 1.0.0 LANGUAGES C CXX)

find_package(Python3 COMPONENTS Interpreter QUIET)
find_package(Threads REQUIRED)

set(M2SDR_ENABLE_LITEPCIE AUTO CACHE STRING "Enable LitePCIe backend: AUTO, ON, or OFF")
set_property(CACHE M2SDR_ENABLE_LITEPCIE PROPERTY STRINGS AUTO ON OFF)
option(M2SDR_ENABLE_LITEETH "Enable LiteEth/Etherbone backend" ON)
option(M2SDR_BUILD_TOOLS "Build command line utilities" ON)
option(M2SDR_BUILD_TESTS "Build CTest tests" ON)
set(M2SDR_BUILD_SOAPY AUTO CACHE STRING "Build SoapySDR module: AUTO, ON, or OFF")
set_property(CACHE M2SDR_BUILD_SOAPY PROPERTY STRINGS AUTO ON OFF)

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
    message(FATAL_ERROR "LitePCIe userspace support is Linux-only; configure with -DM2SDR_ENABLE_LITEPCIE=OFF on this platform")
endif()

if(NOT M2SDR_ENABLE_LITEETH)
    message(FATAL_ERROR "The current portable libm2sdr CMake build requires LiteEth; use -DM2SDR_ENABLE_LITEETH=ON")
endif()

set(M2SDR_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/libm2sdr
    ${CMAKE_CURRENT_SOURCE_DIR}/libliteeth
    ${CMAKE_CURRENT_SOURCE_DIR}/ad9361
)

set(M2SDR_LIB_SOURCES
    libm2sdr/m2sdr_platform.c
    libm2sdr/m2sdr_si5351_i2c.c
    libm2sdr/m2sdr_ad9361_spi.c
    libm2sdr/m2sdr_flash.c
    libm2sdr/m2sdr_device.c
    libm2sdr/m2sdr_stream.c
    libm2sdr/m2sdr_rf.c
    libm2sdr/m2sdr_sata.c
    libm2sdr/m2sdr_utils.c
    libm2sdr/m2sdr_json.c
    libm2sdr/m2sdr_sigmf.c
    ad9361/ad9361.c
    ad9361/ad9361_api.c
    ad9361/ad9361_conv.c
    ad9361/util.c
)

if(M2SDR_ENABLE_LITEETH)
    list(APPEND M2SDR_LIB_SOURCES
        libm2sdr/m2sdr_hal_liteeth.c
        libliteeth/etherbone.c
        libliteeth/liteeth_udp.c
    )
endif()

if(M2SDR_LITEPCIE_ENABLED)
    list(APPEND M2SDR_COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/liblitepcie)
    list(APPEND M2SDR_LIB_SOURCES
        libm2sdr/m2sdr_hal_litepcie.c
        liblitepcie/litepcie_dma.c
        liblitepcie/litepcie_helpers.c
    )
endif()

add_library(m2sdr_objects OBJECT ${M2SDR_LIB_SOURCES})
target_include_directories(m2sdr_objects PUBLIC ${M2SDR_COMMON_INCLUDES})
target_compile_features(m2sdr_objects PUBLIC c_std_11)
set_target_properties(m2sdr_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(M2SDR_ENABLE_LITEETH)
    target_compile_definitions(m2sdr_objects PUBLIC USE_LITEETH)
endif()
if(M2SDR_LITEPCIE_ENABLED)
    target_compile_definitions(m2sdr_objects PUBLIC USE_LITEPCIE)
endif()
# Match the Makefile's USE_RUNTIME default: prefer PCIe whenever it is
# enabled so both build systems open the same default device.
if(M2SDR_LITEPCIE_ENABLED)
    target_compile_definitions(m2sdr_objects PUBLIC M2SDR_DEFAULT_TRANSPORT_LITEPCIE)
else()
    target_compile_definitions(m2sdr_objects PUBLIC M2SDR_DEFAULT_TRANSPORT_LITEETH)
endif()
if(WIN32)
    target_compile_definitions(m2sdr_objects PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0600
        _USE_MATH_DEFINES
    )
endif()

add_library(m2sdr_static STATIC $<TARGET_OBJECTS:m2sdr_objects>)
add_library(m2sdr SHARED $<TARGET_OBJECTS:m2sdr_objects>)
add_library(m2sdr::static ALIAS m2sdr_static)
add_library(m2sdr::shared ALIAS m2sdr)

foreach(target m2sdr_static m2sdr)
    target_include_directories(${target} PUBLIC ${M2SDR_COMMON_INCLUDES})
    target_compile_definitions(${target} PUBLIC
        $<TARGET_PROPERTY:m2sdr_objects,COMPILE_DEFINITIONS>
    )
    if(WIN32)
        target_link_libraries(${target} PUBLIC ws2_32)
    elseif(NOT MSVC)
        target_link_libraries(${target} PUBLIC m)
    endif()
endforeach()

set_target_properties(m2sdr PROPERTIES
    OUTPUT_NAME m2sdr
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)
set_target_properties(m2sdr_static PROPERTIES OUTPUT_NAME m2sdr)

function(m2sdr_add_tool name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${M2SDR_COMMON_INCLUDES})
    target_link_libraries(${name} PRIVATE m2sdr_static)
    target_compile_features(${name} PRIVATE c_std_11)
    if(WIN32)
        target_compile_definitions(${name} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0600
            _USE_MATH_DEFINES
        )
        target_link_libraries(${name} PRIVATE ws2_32)
    elseif(NOT MSVC)
        target_link_libraries(${name} PRIVATE m)
    endif()
endfunction()

function(m2sdr_keep_asserts name)
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
endfunction()

if(M2SDR_BUILD_TOOLS)
    set(M2SDR_CLI_SOURCES m2sdr_cli.c m2sdr_getopt.c)
    set(M2SDR_TOOL_SOURCES m2sdr_tool.c)
    set(M2SDR_HOST_QUEUE_SOURCES m2sdr_host_queue.c)
    set(M2SDR_SATA_TOOL_SOURCES
        m2sdr_sata.c
        m2sdr_sata_capture_volume.c
        m2sdr_sata_lowlevel.c
        m2sdr_sata_hostio.c
        m2sdr_sata_sigmf.c
    )

    m2sdr_add_tool(m2sdr_util ${M2SDR_CLI_SOURCES} m2sdr_util.c)
    m2sdr_add_tool(m2sdr_rf ${M2SDR_CLI_SOURCES} m2sdr_rf.c)
    m2sdr_add_tool(m2sdr_gpio ${M2SDR_CLI_SOURCES} m2sdr_gpio.c)
    m2sdr_add_tool(m2sdr_sata ${M2SDR_CLI_SOURCES} ${M2SDR_SATA_TOOL_SOURCES})
    if(CMAKE_USE_PTHREADS_INIT)
        m2sdr_add_tool(m2sdr_play ${M2SDR_CLI_SOURCES} ${M2SDR_TOOL_SOURCES} ${M2SDR_HOST_QUEUE_SOURCES} m2sdr_play.c)
        target_link_libraries(m2sdr_play PRIVATE Threads::Threads)
        m2sdr_add_tool(m2sdr_record ${M2SDR_CLI_SOURCES} ${M2SDR_TOOL_SOURCES} ${M2SDR_HOST_QUEUE_SOURCES} m2sdr_record.c)
        target_link_libraries(m2sdr_record PRIVATE Threads::Threads)
    else()
        message(STATUS "Skipping m2sdr_play/m2sdr_record: pthreads not available (MSVC)")
    endif()
    m2sdr_add_tool(m2sdr_gen ${M2SDR_CLI_SOURCES} ${M2SDR_TOOL_SOURCES} m2sdr_gen.c kissfft/kiss_fft.c)
    m2sdr_add_tool(m2sdr_sigmf m2sdr_getopt.c m2sdr_sigmf.c)
endif()

if(M2SDR_BUILD_TESTS)
    enable_testing()

    if(M2SDR_BUILD_TOOLS AND Python3_FOUND)
        add_test(NAME test_fake_liteeth_cli
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fake_liteeth.py
                --m2sdr-util $<TARGET_FILE:m2sdr_util>
        )
    endif()

    if(NOT WIN32)
        m2sdr_add_tool(test_libm2sdr ${M2SDR_CLI_SOURCES} m2sdr_tool.c tests/test_libm2sdr.c)
        m2sdr_add_tool(test_m2sdr_sigmf tests/test_m2sdr_sigmf.c)
        m2sdr_add_tool(test_sigmf_capture_smoke tests/test_sigmf_capture_smoke.c)
        m2sdr_add_tool(test_m2sdr_sata_cli tests/test_m2sdr_sata_cli.c)
        m2sdr_add_tool(test_m2sdr_sata_hostio tests/test_m2sdr_sata_hostio.c m2sdr_sata_hostio.c)
        m2sdr_add_tool(test_m2sdr_host_queue tests/test_m2sdr_host_queue.c m2sdr_host_queue.c)
        target_link_libraries(test_m2sdr_host_queue PRIVATE Threads::Threads)
        add_executable(test_sigmf_cli tests/test_sigmf_cli.c)

        foreach(test_target
            test_libm2sdr
            test_m2sdr_sigmf
            test_sigmf_capture_smoke
            test_m2sdr_sata_cli
            test_m2sdr_sata_hostio
            test_m2sdr_host_queue
            test_sigmf_cli
        )
            m2sdr_keep_asserts(${test_target})
        endforeach()

        add_test(NAME test_libm2sdr COMMAND test_libm2sdr)
        add_test(NAME test_m2sdr_host_queue COMMAND test_m2sdr_host_queue)
        add_test(NAME test_m2sdr_sigmf COMMAND test_m2sdr_sigmf)
        add_test(NAME test_sigmf_capture_smoke COMMAND test_sigmf_capture_smoke)
        add_test(NAME test_m2sdr_sata_hostio COMMAND test_m2sdr_sata_hostio)
        if(M2SDR_BUILD_TOOLS)
            add_test(NAME test_sigmf_cli COMMAND test_sigmf_cli)
            set_tests_properties(test_sigmf_cli PROPERTIES
                ENVIRONMENT "M2SDR_SIGMF_TOOL=$<TARGET_FILE:m2sdr_sigmf>"
            )
            add_test(NAME test_m2sdr_sata_cli COMMAND test_m2sdr_sata_cli)
            set_tests_properties(test_m2sdr_sata_cli PROPERTIES
                WORKING_DIRECTORY $<TARGET_FILE_DIR:m2sdr_sata>
            )
        endif()
    endif()
endif()

function(m2sdr_add_fake_liteeth_soapy_test)
    if(M2SDR_BUILD_TESTS AND Python3_FOUND AND M2SDR_BUILD_TOOLS AND TARGET SoapyLiteXM2SDR)
        find_program(SOAPY_SDR_UTIL_EXECUTABLE SoapySDRUtil)
        if(SOAPY_SDR_UTIL_EXECUTABLE)
            add_test(NAME test_fake_liteeth_soapy
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fake_liteeth.py
                    --m2sdr-util $<TARGET_FILE:m2sdr_util>
                    --soapy-util ${SOAPY_SDR_UTIL_EXECUTABLE}
                    --soapy-plugin-dir $<TARGET_FILE_DIR:SoapyLiteXM2SDR>
            )
        endif()
    endif()
endfunction()

if(M2SDR_BUILD_SOAPY STREQUAL "ON" OR M2SDR_BUILD_SOAPY STREQUAL "AUTO")
    find_package(SoapySDR CONFIG QUIET)
    if(SoapySDR_FOUND)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../soapysdr ${CMAKE_CURRENT_BINARY_DIR}/soapysdr)
        m2sdr_add_fake_liteeth_soapy_test()
    elseif(M2SDR_BUILD_SOAPY STREQUAL "ON")
        message(FATAL_ERROR "SoapySDR was requested but was not found")
    endif()
endif()
