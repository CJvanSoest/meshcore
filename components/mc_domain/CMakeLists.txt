# mc_domain — application domain (L2): identity, contacts, channels, chat,
# nodes, history, and the NVS-backed settings store. This is the stateful core
# the UI and radio sit on top of. It REQUIRES tanmatsu-lora publicly because
# nodes.h and settings_nvs.h expose lora_* types in their interfaces.
idf_component_register(
    SRCS
        "identity.c"
        "contacts.c"
        "channels.c"
        "chat.c"
        "nodes.c"
        "history.c"
        "settings_nvs.c"
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
)
