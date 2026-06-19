idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()


idf_component_register(
	SRCS
		"main.c"
	PRIV_REQUIRES
		mc_common
		mc_proto
		mc_io
		mc_domain
		mc_radio
		mc_net
		mc_ui
		vendor
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
