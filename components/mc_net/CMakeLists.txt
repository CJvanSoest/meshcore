# mc_net — connectivity + peripherals: BLE, companion transport,
# WiFi keepalive, GPS task, map. See docs/Components.md.
idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
    SRCS
        "ble_companion.c"
        "companion_transport.c"
        "wifi_keepalive.c"
        "gps_task.c"
        "map.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        mc_domain
        mc_common
        badge-bsp
    PRIV_REQUIRES
        mc_io
        mc_proto
        vendor
        tanmatsu-lora
        esp_app_format
        esp_event
        esp_netif
        esp_wifi
        wifi-manager
        bt
        lwip
        heap
        ${extra_requires}
)
