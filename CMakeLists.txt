# CMakeList.txt : 最上位の CMake プロジェクト ファイル。グローバル構成を行います
# また、サブプロジェクトをここに含めます。
#
cmake_minimum_required (VERSION 3.20)

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

find_package(mosquitto CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

enable_testing()

# 1. ロジック部（静的ライブラリ）
add_library(MTQQToasterLib STATIC
    app_logic.cpp
)

target_link_libraries(MTQQToasterLib
    PUBLIC
        unofficial::mosquitto::mosquitto
        spdlog::spdlog_header_only
        nlohmann_json::nlohmann_json
        windowsapp
        Shell32
        User32
        Kernel32       # 明示的に Kernel32 を追加
        Advapi32       # mosquitto が使用する標準 Windows サービス API
        Ws2_32         # WinSock (Socket 通信)
)

target_compile_options(MTQQToasterLib PRIVATE /utf-8 /permissive- /W4)

# 2. 本番用 EXE
add_executable(MTQQToaster WIN32
    main.cpp
    app.rc
)
target_link_libraries(MTQQToaster PRIVATE MTQQToasterLib)
set_target_properties(MTQQToaster PROPERTIES WIN32_EXECUTABLE TRUE)

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
        $<$<CONFIG:Release>:/O1 /Gy /Gw /GL>
    )
    target_compile_options(MTQQToasterLib PRIVATE
        $<$<CONFIG:Release>:/O1 /Gy /Gw /GL>
    )

    # リンクオプション: 未使用コード/データの削除 (/OPT:REF, /OPT:ICF) と 全体プログラム最適化 (/LTCG)
    target_link_options(MTQQToaster PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /LTCG>
    )
    target_link_options(MTQQToasterLib PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /LTCG>
    )
endif()

include(GoogleTest)
gtest_discover_tests(MTQQToasterTests)