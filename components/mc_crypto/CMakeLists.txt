# mc_crypto — pure channel crypto + ACK CRC, mbedtls only, host-tested.
# Keep it free of ESP-IDF / radio / domain state. See docs/Components.md.
idf_component_register(
    SRCS
        "mc_crypto.c"
        "mc_crypto_dm.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        mc_proto
    PRIV_REQUIRES
        mbedtls
        vendor
)
