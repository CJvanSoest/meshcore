idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
		"history.c"
		"contacts.c"
		"identity.c"
		"settings_nvs.c"
		"nvs_helpers.c"
		"nodes.c"
		"chat.c"
		"channels.c"
		"radio.c"
		"radio_system_protocol_client.c"
		"input.c"
		"render.c"
		"render_settings.c"
		"render_settings_icons.c"
		"render_nodes.c"
		"render_chat.c"
		"render_channel.c"
		"render_home.c"
		"render_about.c"
		"render_map.c"
		"map.c"
		"lodepng.c"
		"gps_task.c"
		"emoji.c"
		"emoji_bitmaps.c"
		"ed25519.c"
		"ed25519_mpi.c"
		"gps.c"
		"qrcodegen.c"
		"companion_transport.c"
		"ble_companion.c"
		"wifi_keepalive.c"
		"http_server.c"
		"cert_gen.c"
		"sounds.c"
	PRIV_REQUIRES
		mc_proto
		esp_lcd
		esp_app_format
		nvs_flash
		bt
		lwip
		esp_http_server
		esp_https_server
		json
		badge-bsp
		tanmatsu-lora
		tanmatsu-wifi
		wifi-manager
		mbedtls
		fatfs
		sdmmc
		esp_driver_sdmmc
		esp_driver_i2s
		${extra_requires}
		espressif__mdns
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)

# Slim lodepng to decoder-only — we read PNGs from SD via fread and never
# write or generate PNGs, so the encoder + ancillary chunk + disk helpers
# just bloat the binary.
set_source_files_properties("main/lodepng.c" PROPERTIES COMPILE_OPTIONS
	"-DLODEPNG_NO_COMPILE_ENCODER;-DLODEPNG_NO_COMPILE_DISK;-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS;-DLODEPNG_NO_COMPILE_ERROR_TEXT;-Wno-error=implicit-fallthrough;-Wno-error=stringop-overflow")
