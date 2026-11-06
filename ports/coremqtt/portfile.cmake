vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO FreeRTOS/coreMQTT
    REF v2.3.1
    SHA512 7e4788729ba857ace8515de15f4e1c9d32bcf34d8a70e130d5ed5e19791ba44d5c67ec03618968d72e4e26aac63dc20a55bf02f02d410a97bd919866af033383
)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/coremqtt)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
