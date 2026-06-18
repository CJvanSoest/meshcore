# mc_proto — pure MeshCore protocol codecs (packet + payloads).
#
# No ESP-IDF dependencies on purpose: the same sources are unit-tested on the
# host (see tests/, run by 'make test' in CI). Keep this component free of
# esp_*, FreeRTOS, pax and BSP includes so it stays host-buildable and remains
# the upstream protocol mirror (see ARCHITECTURE.md "Wire-boundary discipline").
idf_component_register(
    SRCS
        "meshcore/packet.c"
        "meshcore/payload/advert.c"
        "meshcore/payload/grp_txt.c"
    INCLUDE_DIRS
        "."
)
