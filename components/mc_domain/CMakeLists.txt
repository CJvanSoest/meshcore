# mc_domain — stateful app core (identity, contacts, channels, chat, nodes,
# history, settings, sounds). Public REQUIRES tanmatsu-lora because nodes.h /
# settings_nvs.h expose lora_* types. See docs/wiki/Components.md.
idf_component_register(
    SRCS
        "identity.c"
        "contacts.c"
        "channels.c"
        "chat.c"
        "nodes.c"
        "history.c"
        "settings_nvs.c"
        "sounds.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        mc_proto
        mc_common
        tanmatsu-lora
        freertos
    PRIV_REQUIRES
        mc_io
        vendor
        nvs_flash
        mbedtls
        esp_hw_support
        fatfs
        esp_driver_sdmmc
        sdmmc
        badge-bsp
        tanmatsu-wifi
        wifi-manager
        lwip
        esp_driver_i2s
)
