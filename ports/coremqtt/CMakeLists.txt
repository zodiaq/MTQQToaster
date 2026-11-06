cmake_minimum_required(VERSION 3.20)
project(coremqtt LANGUAGES C)
include(mqttFilePaths.cmake)
add_library(coremqtt STATIC ${MQTT_SOURCES} ${MQTT_SERIALIZER_SOURCES})
target_include_directories(coremqtt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source/interface>
    $<INSTALL_INTERFACE:include>)
target_compile_definitions(coremqtt PRIVATE MQTT_DO_NOT_USE_CUSTOM_CONFIG
    MQTT_RECV_POLLING_TIMEOUT_MS=0 MQTT_SEND_TIMEOUT_MS=5000)
install(TARGETS coremqtt EXPORT coremqttTargets ARCHIVE DESTINATION lib)
install(DIRECTORY source/include/ source/interface/ DESTINATION include FILES_MATCHING PATTERN "*.h")
install(EXPORT coremqttTargets NAMESPACE coremqtt:: DESTINATION lib/cmake/coremqtt)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coremqttConfig.cmake" "include(\"\${CMAKE_CURRENT_LIST_DIR}/coremqttTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/coremqttConfig.cmake" DESTINATION lib/cmake/coremqtt)
