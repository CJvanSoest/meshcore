# mc_common — foundation leaf: constants, view enum, UI-state externs, config
# enums, pax-free emoji table, and the diagnostics capture ring. The diag ring
# is pure transport-log storage (no domain state); it sits at L0 so the radio
# tap (mc_radio) and the UI both reach it downward, hence the freertos + heap
# requirements below. See docs/Components.md.
idf_component_register(
    SRCS
        "emoji_table.c"
        "diag.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        freertos
        esp_hw_support
)
