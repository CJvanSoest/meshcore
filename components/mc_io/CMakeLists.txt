# mc_io — platform I/O helpers (L1): NVS key/value wrappers, the GPS UART/I2C
# reader, and device-certificate generation. Sits just above the pure cores:
# it pulls in ESP-IDF drivers but exposes only small, behaviour-neutral helpers.
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
