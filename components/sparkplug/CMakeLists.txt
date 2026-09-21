# Sparkplug B stack as its own ESP-IDF component.
#
# The sources are pure C (mkeras BasicTag + EmbeddedSparkplugNode with its
# bundled nanopb -- the exact field-proven copies from the
# xiao-telemetry-sparkplug sketch, #126) and live under vendor/sparkplug/
# with the other vendored code.
#
# They are a separate component, NOT more SRCS on `main`, deliberately: main
# is an all-C++ component, and adding .c files to it makes PlatformIO's
# espidf builder merge ESP-IDF's C-only warning flags (e.g.
# -Wno-old-style-declaration, which GCC 15 rejects for C++) into the shared
# per-component flags -- breaking every .cpp compile in main under -Werror.
# In their own component the C flags stay with the C sources.

set(SPARKPLUG_VENDOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../../vendor/sparkplug")

idf_component_register(
  SRCS
    "${SPARKPLUG_VENDOR_DIR}/BasicTag.c"
    "${SPARKPLUG_VENDOR_DIR}/SparkplugNode.c"
    "${SPARKPLUG_VENDOR_DIR}/EmbeddedSparkplugPayloads.c"
    "${SPARKPLUG_VENDOR_DIR}/pb_common.c"
    "${SPARKPLUG_VENDOR_DIR}/pb_decode.c"
    "${SPARKPLUG_VENDOR_DIR}/pb_encode.c"
    "${SPARKPLUG_VENDOR_DIR}/sparkplug.pb.c"
  INCLUDE_DIRS
    "${SPARKPLUG_VENDOR_DIR}"
)

# Vendored code stays byte-identical to the field-proven copies; GCC 15 under
# ESP-IDF's -Werror is stricter than the Arduino toolchain it was proven on
# (implicit malloc decl, an incompatible-pointer assignment in the unused
# setEncodeStream path). Relax to warnings for this component only.
target_compile_options(${COMPONENT_LIB} PRIVATE
  -Wno-error
  -fpermissive
)
