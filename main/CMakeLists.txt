idf_component_register(
    SRCS "app_main.c"
         "../remote/remote_core.c"
         "../remote/remote_wire.c"
         "../remote/remote_button.c"
         "../remote/remote_ble.c"
         "../remote/remote_app.c"
         "../utils/crc/custom_crc16.c"
         "../utils/crc/custom_crc32.c"
    INCLUDE_DIRS "." "../remote" "../utils/crc"
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_timer esp_system led_strip
)
