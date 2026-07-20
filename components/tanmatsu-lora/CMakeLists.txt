# TEMPORARY local override of nicolaielectronics/tanmatsu-lora v0.5.6.
# Upstream's CMakeLists.txt omits esp-hosted-tanmatsu from REQUIRES even
# though lora.c includes esp_hosted.h on ESP32P4, which is fetched via
# idf_component.yml but never gets its include path wired in — breaks the
# build for any project that doesn't already require esp-hosted-tanmatsu
# elsewhere. Fix: Nicolai-Electronics/esp32-component-tanmatsu-lora#7.
# Remove this override (and the components/tanmatsu-lora/ dir) once that
# lands and the registry version has caught up.
if(CONFIG_IDF_TARGET_ESP32P4)
	set(lora_extra_priv_requires "nicolaielectronics__esp-hosted-tanmatsu")
endif()

idf_component_register(
	SRCS
		lora.c
	INCLUDE_DIRS
		"include"
	REQUIRES
		sx126x
	PRIV_REQUIRES
		${lora_extra_priv_requires}
)
