#include "lora.h"
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "portmacro.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted.h"
#endif

static const char TAG[] = "lora";

QueueHandle_t     lora_packet_queue          = NULL;
SemaphoreHandle_t lora_mutex                 = NULL;
SemaphoreHandle_t lora_transaction_semaphore = NULL;
uint32_t          lora_sequence_number       = 0;
uint8_t           lora_packet_buffer[sizeof(uint32_t) + 512];
size_t            lora_packet_size = 0;

static esp_err_t lora_transaction(const uint8_t* request, size_t request_length, uint8_t* out_response,
                                  size_t* response_length, size_t max_response_length) {
    if (!lora_mutex || !lora_transaction_semaphore || !lora_packet_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_FAIL;
    xSemaphoreTake(lora_mutex, portMAX_DELAY);
    xSemaphoreTake(lora_transaction_semaphore, 0);  // Clear semaphore
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    ESP_LOGI(TAG, "TX type=%u len=%u", ((lora_protocol_header_t*)request)->type, request_length);
    result = esp_hosted_send_custom_data(1, (uint8_t*)request, request_length);
    if (result == ESP_OK) {
        // Loop and skip unsolicited C6 status events (type=GET_STATUS, header-only, 8 bytes).
        // The C6 uses its own sequence counter so we do NOT filter by sequence number.
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        result              = ESP_ERR_TIMEOUT;
        while (1) {
            TickType_t now       = xTaskGetTickCount();
            int32_t    remaining = (int32_t)(deadline - now);
            if (remaining <= 0) break;
            if (xSemaphoreTake(lora_transaction_semaphore, (TickType_t)remaining) != pdTRUE) break;
            lora_protocol_header_t* resp_hdr = (lora_protocol_header_t*)lora_packet_buffer;
            ESP_LOGI(TAG, "RX size=%u seq=%u type=%u", lora_packet_size,
                     lora_packet_size >= sizeof(lora_protocol_header_t) ? resp_hdr->sequence_number : 0,
                     lora_packet_size >= sizeof(lora_protocol_header_t) ? resp_hdr->type : 0);
            // Skip header-only type=GET_STATUS packets — these are unsolicited C6 events
            if (lora_packet_size == sizeof(lora_protocol_header_t) &&
                lora_packet_size >= sizeof(lora_protocol_header_t) &&
                resp_hdr->type == LORA_PROTOCOL_TYPE_GET_STATUS) {
                ESP_LOGW(TAG, "Skipping unsolicited C6 status event");
                continue;
            }
            if (lora_packet_size <= max_response_length) {
                memcpy(out_response, lora_packet_buffer, lora_packet_size);
                *response_length = lora_packet_size;
                result           = ESP_OK;
            } else {
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        ESP_LOGI(TAG, "TX done result=%d len=%u", result, *response_length);
    }
#else
    result = ESP_OK;
#endif
    lora_sequence_number++;
    xSemaphoreGive(lora_mutex);
    return result;
}

static void lora_transaction_receive(uint32_t msg_id, const uint8_t* packet, size_t length) {
    if (msg_id != 1) {
        ESP_LOGW(TAG, "Received lora message with unknown ID: %u", msg_id);
        return;
    }

    static lora_protocol_lora_packet_t lora_packet = {0};

    if (!lora_mutex || !lora_transaction_semaphore || !lora_packet_queue) {
        ESP_LOGW(TAG, "Received lora message but lora not initialized");
        return;
    }
    if (length > sizeof(lora_packet_buffer) || length < sizeof(lora_protocol_header_t)) {
        ESP_LOGW(TAG, "Received lora message but size incorrect");
        return;
    }
    lora_protocol_header_t* header = (lora_protocol_header_t*)packet;
    ESP_LOGI(TAG, "RECV from C6: len=%u seq=%u type=%u", length, header->sequence_number, header->type);
    if (header->type == LORA_PROTOCOL_TYPE_PACKET_RX) {
        // Legacy RX zonder stats (oude C6 firmware)
        memset(&lora_packet, 0, sizeof(lora_packet));
        lora_packet.length = length - sizeof(lora_protocol_header_t);
        if (lora_packet.length > sizeof(lora_packet.data)) lora_packet.length = sizeof(lora_packet.data);
        memcpy(lora_packet.data, packet + sizeof(lora_protocol_header_t), lora_packet.length);
        lora_packet.stats.valid = false;
        xQueueSend(lora_packet_queue, &lora_packet, 0);
    } else if (header->type == LORA_PROTOCOL_TYPE_PACKET_RX_V2) {
        // RX met packet stats: [header][stats 3B][payload]
        size_t hdr_size   = sizeof(lora_protocol_header_t);
        size_t stats_size = sizeof(lora_packet_stats_t) - sizeof(bool);  // valid-flag is local-only
        // Bewuste keuze: serialiseer alleen de 3 raw bytes; valid wordt door ons gezet.
        if (length < hdr_size + 3) {
            ESP_LOGW(TAG, "PACKET_RX_V2 too short: %u", length);
            return;
        }
        memset(&lora_packet, 0, sizeof(lora_packet));
        const uint8_t* stats_bytes  = packet + hdr_size;
        lora_packet.stats.rssi_pkt_raw        = stats_bytes[0];
        lora_packet.stats.snr_pkt_raw         = (int8_t)stats_bytes[1];
        lora_packet.stats.signal_rssi_pkt_raw = stats_bytes[2];
        lora_packet.stats.valid               = true;
        lora_packet.length = length - hdr_size - 3;
        if (lora_packet.length > sizeof(lora_packet.data)) lora_packet.length = sizeof(lora_packet.data);
        memcpy(lora_packet.data, packet + hdr_size + 3, lora_packet.length);
        xQueueSend(lora_packet_queue, &lora_packet, 0);
        (void)stats_size;
    } else {
        // Response to a transaction
        memcpy(lora_packet_buffer, packet, length);
        lora_packet_size = length;
        xSemaphoreGive(lora_transaction_semaphore);
    }
}

esp_err_t lora_init(uint32_t packet_queue_size) {
    lora_mutex                 = xSemaphoreCreateMutex();
    lora_transaction_semaphore = xSemaphoreCreateBinary();
    lora_packet_queue          = xQueueCreate(packet_queue_size, sizeof(lora_protocol_lora_packet_t));
    if (lora_mutex == NULL || lora_transaction_semaphore == NULL || lora_packet_queue == NULL) {
        if (lora_mutex != NULL) {
            vSemaphoreDelete(lora_mutex);
        }
        if (lora_transaction_semaphore != NULL) {
            vSemaphoreDelete(lora_transaction_semaphore);
        }
        if (lora_packet_queue != NULL) {
            vQueueDelete(lora_packet_queue);
        }
        return ESP_ERR_NO_MEM;
    }

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    esp_hosted_register_custom_callback(1, lora_transaction_receive);
#endif

    return ESP_OK;
}

QueueHandle_t lora_get_packet_queue(void) {
    return lora_packet_queue;
}

esp_err_t lora_get_mode(lora_protocol_mode_t* out_mode) {
    if (out_mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lora_protocol_header_t request = {
        .sequence_number = lora_sequence_number,
        .type            = LORA_PROTOCOL_TYPE_GET_MODE,
    };
    uint8_t   response[sizeof(lora_protocol_header_t) + sizeof(lora_protocol_mode_params_t)] = {0};
    size_t    response_length                                                                = 0;
    esp_err_t result =
        lora_transaction((uint8_t*)&request, sizeof(request), response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t) + sizeof(lora_protocol_mode_params_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    lora_protocol_mode_params_t* params = (lora_protocol_mode_params_t*)(response + sizeof(lora_protocol_header_t));
    *out_mode                           = params->mode;
    return ESP_OK;
}

esp_err_t lora_set_mode(const lora_protocol_mode_t mode) {
    size_t                  request_length = sizeof(lora_protocol_header_t) + sizeof(lora_protocol_mode_params_t);
    uint8_t                 request[request_length];
    lora_protocol_header_t* header      = (lora_protocol_header_t*)request;
    header->sequence_number             = lora_sequence_number;
    header->type                        = LORA_PROTOCOL_TYPE_SET_MODE;
    lora_protocol_mode_params_t* params = (lora_protocol_mode_params_t*)(request + sizeof(lora_protocol_header_t));
    params->mode                        = mode;
    uint8_t   response[sizeof(lora_protocol_header_t)] = {0};
    size_t    response_length                          = 0;
    esp_err_t result = lora_transaction(request, request_length, response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lora_get_config(lora_protocol_config_params_t* out_config) {
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lora_protocol_header_t request = {
        .sequence_number = lora_sequence_number,
        .type            = LORA_PROTOCOL_TYPE_GET_CONFIG,
    };
    uint8_t   response[sizeof(lora_protocol_header_t) + sizeof(lora_protocol_config_params_t)] = {0};
    size_t    response_length                                                                  = 0;
    esp_err_t result =
        lora_transaction((uint8_t*)&request, sizeof(request), response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t) + sizeof(lora_protocol_config_params_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    lora_protocol_config_params_t* params = (lora_protocol_config_params_t*)(response + sizeof(lora_protocol_header_t));
    memcpy(out_config, params, sizeof(lora_protocol_config_params_t));
    return ESP_OK;
}

esp_err_t lora_set_config(const lora_protocol_config_params_t* config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t                  request_length = sizeof(lora_protocol_header_t) + sizeof(lora_protocol_config_params_t);
    uint8_t                 request[request_length];
    lora_protocol_header_t* header        = (lora_protocol_header_t*)request;
    header->sequence_number               = lora_sequence_number;
    header->type                          = LORA_PROTOCOL_TYPE_SET_CONFIG;
    lora_protocol_config_params_t* params = (lora_protocol_config_params_t*)(request + sizeof(lora_protocol_header_t));
    memcpy(params, config, sizeof(lora_protocol_config_params_t));
    uint8_t   response[sizeof(lora_protocol_header_t)] = {0};
    size_t    response_length                          = 0;
    esp_err_t result = lora_transaction(request, request_length, response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lora_get_status(lora_protocol_status_params_t* out_status) {
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lora_protocol_header_t request = {
        .sequence_number = lora_sequence_number,
        .type            = LORA_PROTOCOL_TYPE_GET_STATUS,
    };
    uint8_t   response[sizeof(lora_protocol_header_t) + sizeof(lora_protocol_status_params_t)] = {0};
    size_t    response_length                                                                  = 0;
    esp_err_t result =
        lora_transaction((uint8_t*)&request, sizeof(request), response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t) + sizeof(lora_protocol_status_params_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    lora_protocol_status_params_t* params = (lora_protocol_status_params_t*)(response + sizeof(lora_protocol_header_t));
    memcpy(out_status, params, sizeof(lora_protocol_status_params_t));
    return ESP_OK;
}

esp_err_t lora_send_packet(const lora_protocol_lora_packet_t* packet) {
    size_t                  request_length = sizeof(lora_protocol_header_t) + packet->length;
    uint8_t                 request[request_length];
    lora_protocol_header_t* header = (lora_protocol_header_t*)request;
    header->sequence_number        = lora_sequence_number;
    header->type                   = LORA_PROTOCOL_TYPE_PACKET_TX;
    uint8_t* data_ptr              = (uint8_t*)(request + sizeof(lora_protocol_header_t));
    memcpy(data_ptr, packet->data, packet->length);
    uint8_t   response[sizeof(lora_protocol_header_t)] = {0};
    size_t    response_length                          = 0;
    esp_err_t result = lora_transaction(request, request_length, response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    if (response_length < sizeof(lora_protocol_header_t)) {
        ESP_LOGE(TAG, "Invalid response length: %u\r\n", response_length);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lora_receive_packet(lora_protocol_lora_packet_t* out_packet, TickType_t timeout) {
    return xQueueReceive(lora_packet_queue, out_packet, timeout) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t lora_get_rssi_inst(uint8_t* out_rssi_raw) {
    if (out_rssi_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lora_protocol_header_t request = {
        .sequence_number = lora_sequence_number,
        .type            = LORA_PROTOCOL_TYPE_GET_RSSI_INST,
    };
    uint8_t   response[sizeof(lora_protocol_header_t) + 1] = {0};
    size_t    response_length                              = 0;
    esp_err_t result =
        lora_transaction((uint8_t*)&request, sizeof(request), response, &response_length, sizeof(response));
    if (result != ESP_OK) {
        return result;
    }
    lora_protocol_header_t* resp_hdr = (lora_protocol_header_t*)response;
    if (resp_hdr->type == LORA_PROTOCOL_TYPE_NACK) {
        return ESP_ERR_NOT_SUPPORTED;  // Oude C6 firmware kent dit type niet
    }
    if (response_length < sizeof(lora_protocol_header_t) + 1) {
        return ESP_FAIL;
    }
    *out_rssi_raw = response[sizeof(lora_protocol_header_t)];
    return ESP_OK;
}
