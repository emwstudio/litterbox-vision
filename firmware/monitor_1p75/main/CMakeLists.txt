idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES json catvision_net catvision_display catvision_ui catvision_live nvs_flash esp_timer ${CATVISION_BSP}
)
