idf_component_register(
    SRCS "live_pipeline.cpp" "udp_receiver.cpp"
    INCLUDE_DIRS "."
    REQUIRES catvision_display catvision_net catvision_ui jpegdec lwip esp_timer
)
