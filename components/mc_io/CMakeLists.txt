# mc_io — platform I/O: NVS helpers, GPS reader, device-cert generation.
# See docs/Components.md.
idf_component_register(
    SRCS
        "nvs_helpers.c"
        "gps.c"
        "cert_gen.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        mc_proto
        nvs_flash
        esp_driver_i2c
        esp_timer
        esp_hw_support
        mbedtls
)
