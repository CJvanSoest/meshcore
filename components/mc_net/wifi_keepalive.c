// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "wifi_keepalive.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"
#include "wifi_connection.h"

static const char *TAG = "wifi-ka";

static esp_ping_handle_t s_ping_handle = NULL;

static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    (void)hdl; (void)args;
    // Don't log every success -- 0.2 Hz traffic would spam the console.
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    (void)hdl; (void)args;
    // iPhone hotspot occasionally drops a packet; only matters if we go
    // many in a row. ping_sock handles continuous retries already.
    ESP_LOGD(TAG, "ping timeout");
}

bool wifi_keepalive_start(uint32_t interval_ms) {
    wifi_keepalive_stop();

    esp_netif_ip_info_t *ip = wifi_get_ip_info();
    if (!ip || !ip->gw.addr) {
        ESP_LOGW(TAG, "no gateway IP yet -- can't start keepalive");
        return false;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count       = 0;            // 0 = run forever (until session deleted)
    cfg.interval_ms = interval_ms;
    cfg.timeout_ms  = 2000;
    cfg.data_size   = 16;           // tiny payload, just enough to be a real packet
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip->gw.addr;
    cfg.task_stack_size = 2048;
    cfg.task_prio       = 3;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };

    if (esp_ping_new_session(&cfg, &cbs, &s_ping_handle) != ESP_OK) {
        ESP_LOGE(TAG, "ping_new_session failed");
        s_ping_handle = NULL;
        return false;
    }
    if (esp_ping_start(s_ping_handle) != ESP_OK) {
        ESP_LOGE(TAG, "ping_start failed");
        esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
        return false;
    }
    ESP_LOGI(TAG, "keepalive started: gw=" IPSTR " interval=%u ms",
             IP2STR(&ip->gw), (unsigned)interval_ms);
    return true;
}

void wifi_keepalive_stop(void) {
    if (s_ping_handle == NULL) return;
    esp_ping_stop(s_ping_handle);
    esp_ping_delete_session(s_ping_handle);
    s_ping_handle = NULL;
    ESP_LOGI(TAG, "keepalive stopped");
}

// Auto-manage keepalive lifetime: start it as soon as WiFi is up and
// gateway IP is known, stop it when the link drops. Polls every 2 s --
// cheap, and avoids missing transitions that hooking on the WiFi-event
// system would require routing through wifi-manager.
static void supervisor_task(void *arg) {
    (void)arg;
    bool last_up = false;
    while (1) {
        bool up = wifi_connection_is_connected();
        if (up && !last_up) {
            // Wait a moment for DHCP so the gateway IP is real.
            vTaskDelay(pdMS_TO_TICKS(1000));
            wifi_keepalive_start(5000);
        } else if (!up && last_up) {
            wifi_keepalive_stop();
        }
        last_up = up;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void wifi_keepalive_supervisor_start(void) {
    xTaskCreate(supervisor_task, "wifi-ka-sup", 3072, NULL, 3, NULL);
}
