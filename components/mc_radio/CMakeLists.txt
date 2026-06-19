# mc_radio — the radio layer (L3): the MeshCore packet RX/TX engine (radio.c)
# and the P4<->C6 system-protocol client. It sits above mc_domain because RX
# handling writes straight into chat/contacts/nodes and reads identity/settings.
# radio.h only exposes freertos + lora_* types, so mc_domain stays private here.
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
    PRIV_REQUIRES
        mc_domain
        mc_proto
        mc_crypto
        mc_common
        vendor
        mbedtls
        ${extra_requires}
)
