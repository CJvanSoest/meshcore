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
		"nodes.c"
		"chat.c"
		"ed25519.c"
		"qrcodegen.c"
		"meshcore/packet.c"
		"meshcore/payload/advert.c"
		"meshcore/payload/grp_txt.c"
	PRIV_REQUIRES
		esp_lcd
		nvs_flash
		badge-bsp
		tanmatsu-lora
		tanmatsu-wifi
		wifi-manager
		mbedtls
		fatfs
		sdmmc
		esp_driver_sdmmc
		${extra_requires}
	INCLUDE_DIRS
		"."
		"meshcore"
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
