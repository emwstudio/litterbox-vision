idf_component_register(
    SRCS "main.c" "camera_board.c" "stream_server.c" "camera_mjpeg.c" "camera_udp_stream.c" "camera_udp_protocol.c" "camera_udp_history.c" "camera_udp_control.c" "camera_audio.c" "camera_audio_protocol.c" "camera_ota.c" "camera_ota_url.c"
    INCLUDE_DIRS "."
    REQUIRES catvision_net mdns json esp_https_ota esp_http_server app_update nvs_flash
)
