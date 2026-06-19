# mc_radio — LoRa RX/TX engine (radio.c) + P4<->C6 system-protocol client.
# See docs/Components.md.
idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
    SRCS
        "radio.c"
        "radio_system_protocol_client.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        freertos
        tanmatsu-lora
        mc_proto
    PRIV_REQUIRES
        mc_domain
        mc_crypto
        mc_common
        vendor
        mbedtls
        ${extra_requires}
)
