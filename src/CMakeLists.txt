# MicroFi main component.
#
# Everything in src/ and src/processors/ compiles into the `main` ESP-IDF
# component. Public headers live in ../include and are exposed to other
# components (and to the IDF build system) via INCLUDE_DIRS.

#
# WHOLE_ARCHIVE is important: each processor's translation unit only exposes
# itself to the rest of the binary via a global-constructor side effect
# (MICROFI_REGISTER_PROCESSOR -> static auto-register object). With normal
# library linking the .o file would be dropped because nothing references
# any of its symbols. WHOLE_ARCHIVE forces every .o in this component to be
# linked in, which keeps the auto-registration pattern working.

# Vendored cJSON (upstream DaveGamble/cJSON; *.cpp filenames so PIO/CMake pass
# the C++ toolchain and avoid cxx-only flags leaking into `-std=gnu11` builds).
set(MICROFI_CJSON_SRCS
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/cjson/cJSON.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../vendor/cjson/cJSON_Utils.cpp"
)

idf_component_register(
  SRCS
    "flowfile.cpp"
    "queue.cpp"
    "registry.cpp"
    "session.cpp"
    "flow_engine.cpp"
    "flow_parser.cpp"
    "flow_yaml_parser.cpp"
    "wifi.cpp"
    "agent_id.cpp"
    "manifest.cpp"
    "c2_client.cpp"
    "storage.cpp"
    "littlefs_repository.cpp"
    "flow_store.cpp"
    "flowfile_store.cpp"
    "main.cpp"
    "processors/generate_flowfile.cpp"
    "processors/log_attribute.cpp"
    "processors/publish_mqtt.cpp"
    "processors/update_attribute.cpp"
    "processors/get_gpio.cpp"
    "processors/listen_http.cpp"
    "processors/set_gpio.cpp"
    "processors/capture_image.cpp"
    ${MICROFI_CJSON_SRCS}
  INCLUDE_DIRS
    "."
    "../include"
    "../vendor/cjson"
  REQUIRES
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    esp_http_client
    mqtt
    mbedtls
    esp-tls
    freertos
    log
    littlefs
    driver
    esp_http_server
    esp32-camera
  WHOLE_ARCHIVE
)

target_compile_definitions(
  ${COMPONENT_LIB}
  PRIVATE
    CJSON_NESTING_LIMIT=1000
    CJSON_CIRCULAR_LIMIT=10000
)
