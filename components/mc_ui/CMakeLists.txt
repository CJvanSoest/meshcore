# mc_ui — presentation layer (L4): the pax-based renderers, input handling, and
# the emoji picker. It sits on top of everything it draws — domain state, radio
# status, and the net/peripheral modules — and pulls in pax-gfx for drawing.
idf_component_register(
    SRCS
        "render.c"
        "render_settings.c"
        "render_settings_icons.c"
        "render_nodes.c"
        "render_chat.c"
        "render_channel.c"
        "render_home.c"
        "render_about.c"
        "render_map.c"
        "input.c"
        "emoji.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        mc_common
        mc_domain
        mc_radio
        mc_net
        robotman2412__pax-gfx
        badge-bsp
        freertos
    PRIV_REQUIRES
        mc_io
        mc_proto
        vendor
        wifi-manager
        esp_app_format
)
