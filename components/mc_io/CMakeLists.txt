# mc_io — platform I/O: NVS helpers, GPS reader, internal FAT store,
# crash-safe file replacement.
# See docs/Components.md.
idf_component_register(
    SRCS
        "nvs_helpers.c"
        "gps.c"
        "locfs.c"
        "atomic_file.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        mc_proto
        nvs_flash
        fatfs
        wear_levelling
        esp_driver_i2c
        esp_timer
        esp_hw_support
)
