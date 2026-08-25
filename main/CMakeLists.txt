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
# SPIKE (task #4, IDF6): the newer GCC in esp-idf v6 promotes these to errors on
# the vendored meshcore/ protocol mirror (uint8_t range checks, int/size_t length
# math). The host-test Makefile already suppresses the same two for packet.c; keep
# the mirror unedited and relax them at the build target instead (CLAUDE.md rule 2).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=type-limits" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=sign-compare" APPEND)
# SPIKE ENUMERATION ONLY (task #4): blanket-relax -Werror so the IDF6 build runs
# past every newer-GCC warning in one pass. This reveals whether the migration is
# purely a warning cleanup (build completes) or hides a real error. NOT for merge —
# the real change replaces this with targeted -Wno-error=<class> lines.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)
