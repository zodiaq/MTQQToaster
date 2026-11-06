# CMakeList.txt : 最上位の CMake プロジェクト ファイル。グローバル構成を行います
# また、サブプロジェクトをここに含めます。
#
cmake_minimum_required (VERSION 4.2)

# project()より前にvcpkgを有効にし、通常のcmake -B build -S .でも依存関係を復元する。
include(cmake/ConfigureVcpkg.cmake)
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")

# サポートされている場合は、MSVC コンパイラのホット リロードを有効にします。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("MTQQToaster" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "This project requires MSVC on Windows.")
endif()

find_package(coremqtt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

enable_testing()

# 1. ロジック部（静的ライブラリ）
add_library(MTQQToasterLib STATIC
    src/ui_text.cpp
    src/app_logic.cpp
    src/mqtt_client.cpp
    src/mqtt_transport.cpp
)

target_include_directories(MTQQToasterLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_link_libraries(MTQQToasterLib
    PUBLIC
        coremqtt::coremqtt
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        windowsapp
        Shell32
        Comdlg32
        Windowscodecs
        Gdi32
        User32
        Kernel32       # 明示的に Kernel32 を追加
        Advapi32
        Ws2_32         # WinSock (Socket 通信)
        Secur32        # Schannel / SSPI
        Crypt32        # Windows certificate chain validation
        Normaliz       # IDN host names
)

target_compile_options(MTQQToasterLib PUBLIC /utf-8 PRIVATE /permissive- /W4)
target_compile_definitions(MTQQToasterLib PUBLIC SPDLOG_WCHAR_FILENAMES
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007 CERT_CHAIN_PARA_HAS_EXTRA_FIELDS)

# 2. 本番用 EXE
add_executable(MTQQToaster WIN32
    src/main.cpp
    src/settings_ui.cpp
    resources/app.rc
)
set_source_files_properties(resources/app.rc PROPERTIES COMPILE_FLAGS "/c65001")
target_include_directories(MTQQToaster PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
target_link_libraries(MTQQToaster PRIVATE
    MTQQToasterLib
    Comctl32
    Dwmapi
)
target_compile_options(MTQQToaster PRIVATE /permissive- /W4)
# resources/app.rc embeds the application manifest.
target_link_options(MTQQToaster PRIVATE /MANIFEST:NO)
set_target_properties(MTQQToaster PROPERTIES WIN32_EXECUTABLE TRUE)
add_custom_command(TARGET MTQQToaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config.sample.json
        $<TARGET_FILE_DIR:MTQQToaster>/config.sample.json
)

# 3. テスト用 EXE
add_executable(MTQQToasterTests
    tests/test_main.cpp
    tests/test_mqtt.cpp
)
add_dependencies(MTQQToasterTests MTQQToaster)
target_link_libraries(MTQQToasterTests
    PRIVATE
        MTQQToasterLib
        GTest::gtest
        GTest::gtest_main
)
if(MSVC)
    # コンパイルオプション: サイズ優先最適化 (/O1) と 未使用コードの分離 (/Gy)
    target_compile_options(MTQQToaster PRIVATE
        "$<$<CONFIG:Release>:/O1;/Gy;/Gw;/GL>"
    )
    target_compile_options(MTQQToasterLib PRIVATE
        "$<$<CONFIG:Release>:/O1;/Gy;/Gw;/GL>"
    )

    # リンクオプション: 未使用コード/データの削除 (/OPT:REF, /OPT:ICF) と 全体プログラム最適化 (/LTCG)
    target_link_options(MTQQToaster PRIVATE
        "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF;/LTCG>"
    )
    target_link_options(MTQQToasterLib PRIVATE
        "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF;/LTCG>"
    )
endif()

include(GoogleTest)
gtest_discover_tests(MTQQToasterTests)

option(MTQQTOASTER_XAML_PROBE "Build the isolated OS XAML feasibility probe" OFF)
if(MTQQTOASTER_XAML_PROBE)
    add_executable(MTQQToasterXamlProbe tests/xaml_probe.cpp tests/xaml_probe.manifest)
    target_compile_options(MTQQToasterXamlProbe PRIVATE /utf-8 /permissive- /W4)
    target_link_libraries(MTQQToasterXamlProbe PRIVATE windowsapp)
endif()

# Opt-in local socket/TLS tests; the fixture creates memory-only certificate trust.
option(MTQQTOASTER_TRANSPORT_TESTS "Enable local TCP/Schannel integration tests (PowerShell 7.5+)" OFF)
if(MTQQTOASTER_TRANSPORT_TESTS)
    find_program(PWSH_EXECUTABLE pwsh REQUIRED)
    add_executable(MTQQToasterTransportTests tests/test_transport.cpp)
    target_link_libraries(MTQQToasterTransportTests PRIVATE MTQQToasterLib GTest::gtest_main)
    add_test(NAME WindowsMqttTransport
        COMMAND "${PWSH_EXECUTABLE}" -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_transport_tests.ps1"
            -TestExecutable $<TARGET_FILE:MTQQToasterTransportTests>)
    set_tests_properties(WindowsMqttTransport PROPERTIES TIMEOUT 60)
endif()
