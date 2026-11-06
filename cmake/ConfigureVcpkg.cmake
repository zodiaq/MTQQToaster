# 明示されたtoolchainを優先し、指定がなければVisual Studio付属のvcpkgを使う。
function(mtqqtoaster_configure_vcpkg)
    if(CMAKE_TOOLCHAIN_FILE)
        return()
    endif()

    if(NOT "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{CMAKE_TOOLCHAIN_FILE}" CACHE FILEPATH "CMake toolchain file")
        return()
    endif()

    if(NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" vcpkg_root)
        if(NOT EXISTS "${vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
            message(FATAL_ERROR "VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: ${vcpkg_root}")
        endif()
    else()
        set(vs_roots "${CMAKE_GENERATOR_INSTANCE}" "$ENV{VSINSTALLDIR}")
        set(vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
        if(EXISTS "${vswhere}")
            execute_process(
                COMMAND "${vswhere}" -latest -products *
                    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.Vcpkg
                    -property installationPath
                OUTPUT_VARIABLE vs_root
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE vswhere_result
            )
            if(vswhere_result EQUAL 0 AND NOT vs_root STREQUAL "")
                list(APPEND vs_roots "${vs_root}")
            endif()
        endif()

        foreach(vs_root IN LISTS vs_roots)
            if(NOT vs_root STREQUAL "")
                file(TO_CMAKE_PATH "${vs_root}/VC/vcpkg" candidate)
                if(EXISTS "${candidate}/scripts/buildsystems/vcpkg.cmake")
                    set(vcpkg_root "${candidate}")
                    break()
                endif()
            endif()
        endforeach()
    endif()

    if(NOT vcpkg_root)
        message(FATAL_ERROR
            "vcpkg was not found. In Visual Studio Installer, install 'Desktop development with C++' "
            "including 'C++ CMake tools for Windows' and 'vcpkg package manager' (see .vsconfig). "
            "Alternatively, set VCPKG_ROOT to an existing vcpkg installation.")
    endif()

    message(STATUS "Using vcpkg: ${vcpkg_root}")
    set(CMAKE_TOOLCHAIN_FILE "${vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "CMake toolchain file")
endfunction()

mtqqtoaster_configure_vcpkg()
