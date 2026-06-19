# mc_net — connectivity + peripherals (L4): the HTTPS config server, BLE
# companion link, serial companion transport, WiFi keepalive, the GPS task, and
# the map renderer. These sit above mc_domain because they read settings and
# feed the domain. Public headers expose settings_nvs (mc_domain), config_types
# (mc_common), and pax_gfx (badge-bsp), so those are public REQUIRES.
idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
    SRCS
        "http_server.c"
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
        robotman2412__pax-gfx
    PRIV_REQUIRES
        mc_io
        mc_proto
        vendor
        tanmatsu-lora
        esp_app_format
        json
        esp_event
        esp_http_server
        esp_https_server
        esp_netif
        esp-tls
        esp_wifi
        espressif__mdns
        wifi-manager
        bt
        lwip
        heap
        ${extra_requires}
)
