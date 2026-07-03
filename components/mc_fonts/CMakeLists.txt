# mc_fonts — extended Montserrat display faces (ASCII + Latin-1 + euro/dashes).
# Generated asset data (like vendor/emoji_bitmaps.c): the .c files come out of
# lv_font_conv via scripts/gen_ext_fonts.sh — do not hand-edit them. Kept in a
# leaf component so mc_ui can pull the glyphs downward. See
# docs/features/Special-Characters.md.
idf_component_register(
    SRCS
        "lv_font_montserrat_14_ext.c"
        "lv_font_montserrat_16_ext.c"
        "lv_font_montserrat_20_ext.c"
        "lv_font_montserrat_22_ext.c"
        "lv_font_montserrat_24_ext.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl
)
