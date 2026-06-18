# mc_common — foundation layer (L0): app-wide constants, the view enum + UI
# state externs, shared config-type enums, and the pax-free emoji table. No
# dependency on anything else in the tree, so every higher component can rely
# on it. Header-only except emoji_table.c (pure codepoint lookup + UTF-8).
idf_component_register(
    SRCS
        "emoji_table.c"
    INCLUDE_DIRS
        "."
)
