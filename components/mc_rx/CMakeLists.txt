# mc_rx -- RX application layer: the receive handlers behind the radio sink.
# Depends on everything it wires together (domain, crypto, radio transport);
# nothing depends on it (the sink is a registered callback). See
# docs/Components.md.
idf_component_register(
    SRCS
        "mc_rx.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        mc_radio
        mc_domain
        mc_common
        mc_crypto
        mc_proto
        vendor
        mbedtls
)
