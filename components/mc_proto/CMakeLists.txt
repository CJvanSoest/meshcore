# mc_proto — pure protocol + regulatory + parser core.
#
# No ESP-IDF dependencies on purpose (only C stdlib / POSIX), so every source
# here is unit-tested on the host (see tests/, run by 'make test' in CI) and
# stays the upstream protocol mirror (see ARCHITECTURE.md "Wire-boundary
# discipline"). Do not add esp_*, FreeRTOS, pax or BSP includes to this
# component; that is what keeps it host-buildable.
idf_component_register(
    SRCS
        "meshcore/packet.c"
        "meshcore/payload/advert.c"
        "meshcore/payload/grp_txt.c"
        "region_limits.c"
        "gps_parser.c"
        "companion-radio-protocol/mc_companion_command_parser.c"
        "companion-radio-protocol/mc_companion_serial_interface.c"
        "companion-radio-protocol/mc_companion_server.c"
    INCLUDE_DIRS
        "."
)
