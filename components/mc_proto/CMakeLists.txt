# mc_proto — pure protocol + regulatory + parser core (upstream mirror).
# Do NOT add esp_*, FreeRTOS, pax or BSP includes — C stdlib / POSIX only; that
# is what keeps it host-testable. See docs/wiki/Components.md.
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
