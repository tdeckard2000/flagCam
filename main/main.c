#define BOARD_ESP32CAM_AITHINKER
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <string.h>
#include <sys/param.h>

//! important Enable PSRAM on sdkconfig
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1 // software reset will be performed
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

static const char *TAG = "main";
static const char *HUB_Upload_Address = "http://192.168.4.1:8000/upload";

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    // XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 10000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA,
    .jpeg_quality = 12, // 0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static esp_err_t init_camera(void) {
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_brightness(s, 2);                 // Max brightness
    s->set_saturation(s, -1);                // Slightly reduce saturation
    s->set_gain_ctrl(s, 1);                  // Enable auto gain
    s->set_exposure_ctrl(s, 1);              // Enable auto exposure
    s->set_gainceiling(s, (gainceiling_t)6); // Higher gain for low light
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Connected to Wi-Fi!");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from Wi-Fi, retrying...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            char ip_str[16];
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP: %s", esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str)));
            break;
        }
    }
}

static esp_netif_t *init_wifi() {
    esp_netif_init();
    esp_netif_t *wifi_info = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "PiZeroAP",
            .password = "8123605563",
        },
    };
    ESP_LOGI(TAG, "Connecting to %s", wifi_config.sta.ssid);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
    return wifi_info;
}

static esp_err_t init_4mb_ext() {
    nvs_flash_init();
    return ESP_OK;
}

static const char *get_date() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    static char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return timestamp;
}

static char *create_data_string(camera_fb_t *pic, int *out_len) {
    char data[512];
    const char *id = "AA000123";
    const char *date = get_date();
    const float temp = 40.5;
    const float volts = 3.3431;
    snprintf(
        data,
        sizeof(data),
        "id:%s,date:%s,temp:%.1f,volts:%.4f,image:",
        id, date, temp, volts);
    int total_len = strlen(data) + pic->len;
    char *body = malloc(total_len);
    memcpy(body, data, strlen(data));
    memcpy(body + strlen(data), pic->buf, pic->len);
    *out_len = total_len;
    return body;
}

static esp_err_t post_data(char *data_string, int *data_length) {
    esp_http_client_config_t config = {
        .url = HUB_Upload_Address,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_post_field(client, data_string, *data_length);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_err_t err = esp_http_client_perform(client);
    ESP_LOGI(TAG, "POST Status = %d", esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    } else {
        return ESP_OK;
    }
    return ESP_OK;
}

static void init_pins() {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_NUM_13,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_config_t io_conf_led = {
        .pin_bit_mask = 1ULL << GPIO_NUM_4,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf_led);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 0);
}

static void await_wifi_connected() {
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    while (1) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            vTaskDelay(500 / portTICK_RATE_MS);
            break;
        }
    }
}

static void flash_led(int duration) {
    gpio_set_level(GPIO_NUM_4, 1); // Flash on
    vTaskDelay(duration / portTICK_PERIOD_MS);
    gpio_set_level(GPIO_NUM_4, 0);
}

static void deep_sleep() {
    ESP_LOGI(TAG, "G'Night");
    esp_deep_sleep_start();
}

static camera_fb_t *take_photo() {
    camera_fb_t *pic = esp_camera_fb_get();
    return pic;
}

void app_main(void) {
    init_pins();
    flash_led(500);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    } else {
        esp_deep_sleep_start();
    }
    esp_event_loop_create_default();
    if (ESP_OK != init_4mb_ext()) {
        ESP_LOGI(TAG, "Error 4mb");
        return;
    }
    init_camera();
    init_wifi();
    for(int i = 0; i < 30; i++) {
        await_wifi_connected();
        camera_fb_t *pic = take_photo();
        flash_led(100);
        int data_length;
        char *data_string = create_data_string(pic, &data_length);
        post_data(data_string, &data_length);
        esp_camera_fb_return(pic);
        ESP_LOGI(TAG, "Wait 15 seconds");
        vTaskDelay(15000 / portTICK_PERIOD_MS);
    }
    flash_led(500);
    deep_sleep();
}