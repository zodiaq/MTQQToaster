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

find_package(unofficial-mosquitto CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

set(WINAPP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.winapp")
if(NOT EXISTS "${WINAPP_SDK_DIR}/include/winrt/Microsoft.UI.Xaml.h")
    find_program(WINAPP_CLI winapp)
    if(NOT WINAPP_CLI)
        message(FATAL_ERROR "Windows App SDK is missing. Install WinApp CLI and run 'winapp restore'.")
    endif()
    execute_process(
        COMMAND "${WINAPP_CLI}" restore
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE WINAPP_RESTORE_RESULT
    )
    if(NOT WINAPP_RESTORE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to restore Windows App SDK (exit ${WINAPP_RESTORE_RESULT}).")
    endif()
endif()

enable_testing()

# 1. ロジック部（静的ライブラリ）
add_library(MTQQToasterLib STATIC
    src/app_logic.cpp
)

target_include_directories(MTQQToasterLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

target_link_libraries(MTQQToasterLib
    PUBLIC
        unofficial::mosquitto::mosquitto
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        windowsapp
        Shell32
        Comdlg32
        Windowscodecs
        Gdi32
        User32
        Kernel32       # 明示的に Kernel32 を追加
        Advapi32       # mosquitto が使用する標準 Windows サービス API
        Ws2_32         # WinSock (Socket 通信)
)

target_compile_options(MTQQToasterLib PUBLIC /utf-8 PRIVATE /permissive- /W4)
target_compile_definitions(MTQQToasterLib PUBLIC SPDLOG_WCHAR_FILENAMES)

# WinUI実行ファイル用。Windows App SDKが生成したC++/WinRT投影と同じ版で
# app_logic.cppをコンパイルし、SDK同梱投影を使うテストターゲットと分離する。
add_library(MTQQToasterWinUiLib STATIC
    src/app_logic.cpp
)
target_include_directories(MTQQToasterWinUiLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${WINAPP_SDK_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
target_link_libraries(MTQQToasterWinUiLib
    PUBLIC
        unofficial::mosquitto::mosquitto
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        windowsapp
        Shell32
        Comdlg32
        Windowscodecs
        Gdi32
        User32
        Kernel32
        Advapi32
        Ws2_32
)
target_compile_options(MTQQToasterWinUiLib PUBLIC /utf-8 PRIVATE /permissive- /W4)
target_compile_definitions(MTQQToasterWinUiLib PUBLIC SPDLOG_WCHAR_FILENAMES)

# 2. 本番用 EXE
add_executable(MTQQToaster WIN32
    src/main.cpp
    src/modern_ui.cpp
    resources/app.rc
)
set_source_files_properties(resources/app.rc PROPERTIES COMPILE_FLAGS "/c65001")
target_include_directories(MTQQToaster PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
    ${WINAPP_SDK_DIR}/include
)
target_link_directories(MTQQToaster PRIVATE ${WINAPP_SDK_DIR}/lib/x64)
target_link_libraries(MTQQToaster PRIVATE
    MTQQToasterWinUiLib
    Microsoft.WindowsAppRuntime.Bootstrap.lib
    OneCoreUap.lib
)
target_compile_options(MTQQToaster PRIVATE /bigobj)
set_target_properties(MTQQToaster PROPERTIES WIN32_EXECUTABLE TRUE)
add_custom_command(TARGET MTQQToaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/config.sample.json
        $<TARGET_FILE_DIR:MTQQToaster>/config.sample.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${WINAPP_SDK_DIR}/bin/x64/Microsoft.WindowsAppRuntime.Bootstrap.dll
        $<TARGET_FILE_DIR:MTQQToaster>/Microsoft.WindowsAppRuntime.Bootstrap.dll
)

# 3. テスト用 EXE
add_executable(MTQQToasterTests
    tests/test_main.cpp
)
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
    target_compile_options(MTQQToasterWinUiLib PRIVATE
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
