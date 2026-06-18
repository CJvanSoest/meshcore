# Third-party vendored libraries, kept verbatim from upstream. This is not
# first-party code: do not refactor it to local style or "fix" its TODOs.
# Credits and licences are in the README.
#   lodepng        - Lode Vandevenne (PNG; decoder-only build, see flags below)
#   qrcodegen      - Project Nayuki
#   ed25519        - NaCl/SUPERCOP ref10 + ESP32 mbedtls adaptation
#   emoji_bitmaps  - generated Twemoji glyph data
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
