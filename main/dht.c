
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_timer.h>	// esp_timer_get_time(..)

#include <driver/gpio.h> // IO via pins

#include "dht.h"

static const char* TAG = "DHT";

#define PRIO_DRIVER 10

// Some definitions for clarity.
#define PIN_HIGH 1
#define PIN_LOW 0
#define RISING PIN_HIGH
#define FALLING PIN_LOW


#define TIMEOUT_MICROS (20000ll)
#define THRESHOLD_MICROS (40ll)

#define TIMEOUT_CACHE (2000000ull)

#define FLAG_INIT		(1ul << 0)
#define FLAG_REFRESH	(1ul << 1)

#define EVT_READ_REQUEST	(1ul << 0)
#define EVT_READ_COMPLETE	(1ul << 1)
#define EVT_READ_FAILURE	(1ul << 2)

uint32_t				flags;
uint32_t				pin;
dht_data_t				cache;
TaskHandle_t			drv_sensor;
EventGroupHandle_t		events;
esp_timer_handle_t		timer;

/*
* Driver task runs at a high priority and has one job -- if the read request
*  event bit is set, take a measurement of the DHT22 sensor, and cache the
*  results.  If the read fails for some reason, the FAILURE bit is set and the
*  cache is unmodified.  If the read succeeds, the COMPLETE bit is set and the
*  cache is updated with the newest measurement.
*/
void drv_task(void* param) {
	while (1) {
		EventBits_t evt_bits = 0;
		while (!(evt_bits & EVT_READ_REQUEST)) {
			evt_bits = xEventGroupWaitBits(events, EVT_READ_REQUEST, pdTRUE, pdTRUE, UINT32_MAX);
		}

		int16_t temp = 0;
		int16_t humid = 0;
		int16_t check = 0;
		int16_t balance = 0;

		int last = PIN_HIGH;
		int edge = RISING;

		int64_t t0 = 0;
		int64_t t_last = 0;
		int64_t t_now = 0;

		// Signal the request for sensor data.
		gpio_set_level(pin, PIN_LOW);
		vTaskDelay(10 / portTICK_RATE_MS);
		gpio_set_level(pin, PIN_HIGH);

		// Initialize time-keeping, which we will need in order to discern
		//	0-bits from 1-bits.
		t0 = esp_timer_get_time();
		t_last = t0;
		t_now = t0;
		// NOTE: esp_timer_get_time() has microsecond resolution.

		// Ignore the first 2 falling edges, these correspond to the sensor's
		//  response & prepare signal.
		int ignore = 1;

		// Keep track of the data bits we've received; this will be used to fill
		//	the correct components of the read.
		int bits = 0;
		int16_t* target = NULL;

		// Within the TIMEOUT window, track the signal edges on the data wire
		//	which indicate transmission from the DHT22.
		while (t_now - t0 < TIMEOUT_MICROS && ignore > -41) {
			t_now = esp_timer_get_time();
			edge = gpio_get_level(pin);

			if (last != edge) {
				last = edge;
				if (edge == FALLING) {
					if (ignore < 0) {

						if (bits < 16)			target = &humid;
						else if (bits < 32)		target = &temp;
						else					target = &check;

						*target = *target << 1;
						if (t_now - t_last > THRESHOLD_MICROS) {
							*target |= 1;
						}
						++bits;
					}
					--ignore;
				}
				t_last = t_now;
			}
		}

		// Failure condition -- timeout window passed without receiving the
		//	expected number of bits from DHT22.
		if (t_now - t0 >= TIMEOUT_MICROS) {
			ESP_LOGE(TAG, "Sensor read timed out.");
			xEventGroupSetBits(events, EVT_READ_FAILURE);
			continue;
		}

		balance = ((temp & 0x00FF) + (temp >> 8) + (humid & 0x00FF) + (humid >> 8)) & 0x00FF;

		// Failure condition -- checksum doesn't match received data.
		if (check != balance) {
			ESP_LOGE(TAG, "Sensor read corrupted.");
			xEventGroupSetBits(events, EVT_READ_FAILURE);
			continue;
		}

		cache.humidity = humid;
		cache.temperature = temp;
		flags &= ~FLAG_REFRESH;
		xEventGroupSetBits(events, EVT_READ_COMPLETE);
	}
}

/*
* TIMER CALLBACK method -- sets the flag indicating that the cache has expired.
*/
void timer_cb_cache(void* param) {
	flags |= FLAG_REFRESH;
}

void dht_init(uint32_t data_pin) {
	gpio_config_t io_mix = {};

	io_mix.intr_type = GPIO_INTR_DISABLE;
	io_mix.mode = GPIO_MODE_INPUT_OUTPUT_OD;
	io_mix.pull_up_en = 0;
	io_mix.pull_down_en = 0;
	io_mix.pin_bit_mask = (1ul << data_pin);
	gpio_config(&io_mix);

	flags = 0;
	pin = data_pin;
	memset(&cache, 0, sizeof(dht_data_t));

	esp_timer_create_args_t timer_init = {};
	timer_init.callback = timer_cb_cache;
	timer_init.arg = NULL;
	timer_init.dispatch_method = ESP_TIMER_TASK;
	timer_init.name = "DhtCacheTimer";
	if (esp_timer_create(&timer_init, &timer) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create DHT driver timer.");
		return;
	}

	events = xEventGroupCreate();
	if (!events) {
		ESP_LOGE(TAG, "Failed to create DHT driver event group.");
		return;
	}

	xTaskCreatePinnedToCore(
		drv_task,
		"drv_sensor",
		2048,
		NULL,
		PRIO_DRIVER,
		&drv_sensor,
		0
	);

	esp_timer_start_periodic(timer, TIMEOUT_CACHE);

	flags |= FLAG_INIT;
	flags |= FLAG_REFRESH;
}

int dht_read(dht_data_t* out) {
	assert(out != NULL);

	if (!(flags & FLAG_INIT)) {
		ESP_LOGE(TAG, "DHT driver not initialized.");
		return 1;
	}

	// Check if we still have an up-to-date cached measurement.
	if (!(flags & FLAG_REFRESH)) {
		memcpy(out, &cache, sizeof(dht_data_t));
		return 0;
	}

	// Otherwise, request a new measurement.
	EventBits_t bits = 0;
	xEventGroupSetBits(events, EVT_READ_REQUEST);

	// TODO: Set a sensible upper bound on read wait -- if that times out, we
	//	can assert that the driver is in an unrecoverable state.
	bits = xEventGroupWaitBits(events, EVT_READ_FAILURE | EVT_READ_COMPLETE, pdTRUE, pdFALSE, UINT32_MAX);

	// Check if the read failed -- if so do nothing.
	if (bits & EVT_READ_FAILURE) {
		return 2;
	}

	memcpy(out, &cache, sizeof(dht_data_t));
	return 0;
}