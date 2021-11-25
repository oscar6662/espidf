
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_timer.h>

#include "network.h"

#include "app_bounce.h"

#define APP_BOUNCE_MAGIC 0x0A001B11

void app_bounce_timer_cb(void* param);

static const char* TAG = "app_bounce";

struct {
    uint8_t            node_id;
    uint16_t           cycle_time_ms;
    esp_timer_handle_t timer;
} AppState = {};


void app_bounce_init(uint8_t node_id, uint16_t cycle) {
    // memset(&AppState, 0, sizeof(AppState));
    if (AppState.node_id != 0) {
        ESP_LOGE(TAG, "Error: app_bounce already initialized.");
        return;
    }

    if (node_id == 0) {
        ESP_LOGE(TAG, "Failed to initialize app_bounce, invalid node-id.");
        return;
    }
    if (cycle < 500) {
        ESP_LOGE(TAG, "Failed to initialize app_bounce, invalid cycle time (must be >= 500).");
    }

    AppState.node_id = node_id;
    AppState.cycle_time_ms = cycle;

    esp_timer_create_args_t timer_init = {};
    timer_init.callback = app_bounce_timer_cb;
    timer_init.dispatch_method = ESP_TIMER_TASK;
    timer_init.name = "app_bounce_timer";
    timer_init.skip_unhandled_events = 1;

    if (esp_timer_create(&timer_init, &AppState.timer) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize app_bounce, could not create timer.");
        return;
    }

    net_register_app(APP_BOUNCE_ID);

    uint64_t c = AppState.cycle_time_ms;
    c *= 1000;
    esp_timer_start_periodic(AppState.timer, c);
    ESP_LOGI(TAG, "Initialized app_bounce.");
}

void app_bounce_add_message_up(const char* message, uint32_t life) {
    assert(message != NULL);

    if (strlen(message) >= APP_BOUNCE_BUFFER) {
        ESP_LOGE(TAG, "Cannot add up_message -- too long.");
        return;
    }
    if (life == 0) {
        ESP_LOGE(TAG, "Cannot add up_message -- no lifespan.");
        return;
    }

    app_header_t head = {};
    bounce_packet_t data = {};

    head.type = APP_BOUNCE_ID;
    head.len = 13 + strlen(message) + 1;

    data.magic = APP_BOUNCE_MAGIC;
    data.counter = 0;
    data.life = life;
    data.node_id = AppState.node_id;
    strcpy(data.buffer, message);

    net_send_up(&head, (const uint8_t*)&data);
}

void app_bounce_add_message_down(const char* message, uint32_t life) {
    assert(message != NULL);

    if (strlen(message) >= APP_BOUNCE_BUFFER) {
        ESP_LOGE(TAG, "Cannot add down_message -- too long.");
        return;
    }
    if (life == 0) {
        ESP_LOGE(TAG, "Cannot add down_message -- no lifespan.");
        return;
    }

    app_header_t head = {};
    bounce_packet_t data = {};

    head.type = APP_BOUNCE_ID;
    head.len = 13 + strlen(message) + 1;

    data.magic = APP_BOUNCE_MAGIC;
    data.counter = 0;
    data.life = life;
    data.node_id = AppState.node_id;
    strcpy(data.buffer, message);

    net_send_down(&head, (const uint8_t*)&data);
}

void app_bounce_timer_cb(void* param) {
    app_header_t head = {};
    bounce_packet_t data = {};
    while (net_receive(APP_BOUNCE_ID, &head, (uint8_t*)&data, 0) == 0) {
        if (data.magic == APP_BOUNCE_MAGIC) {
            ESP_LOGI(TAG, "[node: 0x%02X i: %d] %s", data.node_id, data.counter, data.buffer);

            data.counter++;
            data.node_id = AppState.node_id;

            if (data.counter <= data.life) {
                // Leverage the hidden functionality -- check whether packet came
                //    from up-stream or down.
                // NOTE: This behaviour may need to be re-implemented if network
                //    layer implementation changes!  This is a bit of a no-no.
                if (head.reserved[0] == 0x01) {
                    net_send_up(&head, (const uint8_t*)&data);
                }
                else if (head.reserved[0] == 0x00) {
                    net_send_down(&head, (const uint8_t*)&data);
                }
            }
        }

        memset(&head, 0, sizeof(app_header_t));
        memset(&data, 0, sizeof(bounce_packet_t));
    }
}
