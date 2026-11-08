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
		mc_rx
		vendor
		esp_lcd
		esp_app_format
		nvs_flash
		bt
		lwip
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
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
# ESP-IDF 6's newer GCC promotes these to errors on the vendored meshcore/ and
# companion-radio-protocol/ mirrors (uint8_t range checks, int/size_t length math,
# a const-qualifier drop). The host-test Makefile already suppresses the first two
# for packet.c; keep the mirrors unedited and relax them at the build target
# instead (CLAUDE.md rule 2). Harmless on the IDF 5.5.1 toolchain.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=type-limits" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=sign-compare" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=discarded-qualifiers" APPEND)
