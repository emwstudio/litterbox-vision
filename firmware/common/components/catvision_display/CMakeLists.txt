idf_component_register(
    SRCS "catvision_display.c" "catvision_zlib.c" "catvision_direct.c" "catvision_half.c" "catvision_present.c" "catvision_crc.c" "catvision_lvgl.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl espressif__esp_lvgl_adapter esp_timer espressif__zlib
    LDFRAGMENTS "linker.lf"
)

if(CATVISION_BSP)
    idf_component_optional_requires(PRIVATE ${CATVISION_BSP})
endif()
