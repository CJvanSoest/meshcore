# Third-party vendored libraries (lodepng, qrcodegen, ed25519, emoji_bitmaps),
# kept verbatim from upstream — do not restyle them or "fix" their TODOs.
# Credits + licences in AUTHORS. The lodepng decoder-only flags are set below.
idf_component_register(
    SRCS
        "lodepng.c"
        "qrcodegen.c"
        # ed25519: self-contained public-domain ref10 (orlp/ed25519) under
        # ed25519/. Provides the signer (keypair/sign) + X25519 (key_exchange),
        # with its own SHA-512 and field/group math — no mbedtls low-level MPI/
        # SHA-512/ECP, which mbedtls 4.1 (ESP-IDF 6) made private. The two X25519
        # extras we add (key_exchange_raw, pub_to_x25519) live in the ext file.
        "ed25519/keypair.c"
        "ed25519/sign.c"
        "ed25519/verify.c"
        "ed25519/key_exchange.c"
        "ed25519/ge.c"
        "ed25519/sc.c"
        "ed25519/fe.c"
        "ed25519/sha512.c"
        "ed25519_x25519_ext.c"
        # AES-128: vendored public-domain tiny-AES (aes/) + a thin mc_aes wrapper.
        # Replaces mbedtls/aes.h, which mbedtls 4.1 (ESP-IDF 6) made private.
        "aes/aes.c"
        "mc_aes.c"
        # HMAC-SHA256 built on the still-public mbedtls MD hash API, since
        # mbedtls 4.1 dropped the mbedtls_md_hmac* helpers.
        "mc_hmac.c"
        "emoji_bitmaps.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        esp_hw_support
        mbedtls  # mc_hmac.c uses the public mbedtls/md.h hash API
)

# The ref10 ed25519 sources are third-party and kept verbatim; do not hold them
# to our -Werror bar (the newer GCC flags -Wtype-limits / -Wsign-compare on
# them, the same class already suppressed for the meshcore mirror).
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/keypair.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/sign.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/verify.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/key_exchange.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/ge.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/sc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/fe.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ed25519/sha512.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/aes/aes.c"
    PROPERTIES COMPILE_OPTIONS "-w")

# Slim lodepng to decoder-only — we read PNGs from SD via fread and never
# write or generate PNGs, so the encoder + ancillary chunk + disk helpers
# just bloat the binary.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/lodepng.c" PROPERTIES COMPILE_OPTIONS
	"-DLODEPNG_NO_COMPILE_ENCODER;-DLODEPNG_NO_COMPILE_DISK;-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS;-DLODEPNG_NO_COMPILE_ERROR_TEXT;-Wno-error=implicit-fallthrough;-Wno-error=stringop-overflow")
