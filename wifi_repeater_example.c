#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/opt.h"

#define STA_SSID "SCF-XIAOMI"
#define STA_PASS "scf888888"
#define AP_SSID  "EPS32S3-cdh"
#define AP_PASS  "12345678"

static const char *TAG = "wifi_repeater";

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Enable NAT when STA gets IP
        ip_napt_enable(event->ip_info.ip.addr, 1);
        ESP_LOGI(TAG, "NAT enabled");
    }
}

void print_connected_stations()
{
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        ESP_LOGI(TAG, "Connected stations: %d", sta_list.num);
        for (int i = 0; i < sta_list.num; i++) {
            ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     sta_list.sta[i].mac[0], sta_list.sta[i].mac[1],
                     sta_list.sta[i].mac[2], sta_list.sta[i].mac[3],
                     sta_list.sta[i].mac[4], sta_list.sta[i].mac[5]);
        }
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize network interfaces
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create STA and AP netifs
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // Configure AP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // Configure WiFi in AP+STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure AP
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    // Configure STA
    wifi_config_t sta_config = {
        .sta = {
            .ssid = STA_SSID,
            .password = STA_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

    // Enable IP forwarding
    esp_netif_set_ip_forward(ap_netif, true);
    esp_netif_set_ip_forward(sta_netif, true);

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi repeater started");
    ESP_LOGI(TAG, "AP SSID: %s", AP_SSID);
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");

    // Main loop - print connected stations every 5 seconds
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        print_connected_stations();
    }
}