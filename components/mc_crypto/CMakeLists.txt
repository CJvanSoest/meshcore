# mc_crypto — pure MeshCore symmetric crypto (channel decrypt/encrypt + the ACK
# binding CRC), lifted out of radio.c. mbedtls only, so every function is host
# unit-tested in tests/ (see test_mc_crypto.c). Keep it free of ESP-IDF, radio
# and domain state; that is what keeps the security-critical paths testable.
idf_component_register(
    SRCS
        "mc_crypto.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        mc_proto
    PRIV_REQUIRES
        mbedtls
)
