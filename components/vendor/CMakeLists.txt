# Third-party vendored libraries (lodepng, qrcodegen, ed25519, emoji_bitmaps),
# kept verbatim from upstream — do not restyle them or "fix" their TODOs.
# Credits + licences in AUTHORS. The lodepng decoder-only flags are set below.
idf_component_register(
    SRCS
        "lodepng.c"
        "qrcodegen.c"
        "ed25519.c"
        "ed25519_mpi.c"
        "emoji_bitmaps.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        mbedtls
        esp_hw_support
)

# Slim lodepng to decoder-only — we read PNGs from SD via fread and never
# write or generate PNGs, so the encoder + ancillary chunk + disk helpers
# just bloat the binary.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/lodepng.c" PROPERTIES COMPILE_OPTIONS
	"-DLODEPNG_NO_COMPILE_ENCODER;-DLODEPNG_NO_COMPILE_DISK;-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS;-DLODEPNG_NO_COMPILE_ERROR_TEXT;-Wno-error=implicit-fallthrough;-Wno-error=stringop-overflow")
