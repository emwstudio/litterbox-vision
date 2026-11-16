idf_component_register(
    SRCS "main.c" "stats_ui.c" "stats_http.c" "stats_signature.c"
    EMBED_FILES "assets/qiuqiu.png" "assets/lvlv.png" "assets/bobo.png" "assets/baobao.png"
    EMBED_FILES "assets/box-1.png" "assets/box-2.png" "assets/box-3.png" "assets/box-4.png"
    INCLUDE_DIRS "."
    REQUIRES json catvision_net catvision_display catvision_ui catvision_live nvs_flash esp_timer ${CATVISION_BSP}
)
