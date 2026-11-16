idf_component_register(
    SRCS
        "src/JPEGDEC.cpp"
        "src/s3_simd_420.S"
        "src/s3_simd_444.S"
        "src/s3_simd_dequant.S"
    INCLUDE_DIRS "src" "include"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ARDUINO_ARCH_ESP32)
