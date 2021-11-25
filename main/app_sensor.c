
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_timer.h>

#include "app_sensor.h"
#include "dht.h"
#include "network.h"


#define PRIO_SENSOR_APP 3

#define EVT_UPDATE (1ul << 0)

#define DEFAULT_PERIOD	100ll
#define FACTOR_PERIOD	100000ll

static const char* TAG = "app_sensor";

void push_cache(const sensor_packet_t* entry);
void update_period();
void process_cache(sensor_packet_t* out, const dht_data_t* local);
void combine(sensor_sample_t* acc, const sensor_sample_t* sample);

void init_packet(sensor_packet_t* pkt);
int check_magic(const sensor_packet_t* pkt);

// Debugging method, prints the output packet contents.
void debug_print(const sensor_packet_t* pkt);

struct {
	struct {
		sensor_packet_t		entry[8];
		uint8_t				usage;
	} cache;

	uint8_t					id;
	uint16_t				period;
	esp_timer_handle_t		timer;
	EventGroupHandle_t		events;
	TaskHandle_t			srv_sensor;
} state;

void app_sensor_timer_cb(void* param) {
	xEventGroupSetBits(state.events, EVT_UPDATE);
}

void app_sensor_task(void* param) {
	while (1) {
		EventBits_t evt_bits = 0;
		while (!(evt_bits & EVT_UPDATE)) {
			evt_bits = xEventGroupWaitBits(state.events, EVT_UPDATE, pdTRUE, pdTRUE, UINT32_MAX);
		}

		int64_t	t_start = esp_timer_get_time();
		int64_t t_delta = 0;

		dht_data_t			local = {};
		app_header_t		head = {};
		uint8_t				data[128];
		sensor_packet_t		remote = {};

		memset(data, 0, 128);
		while (!net_receive(APP_SENSOR_ID, &head, data, 0)) {
			if (head.len != sizeof(sensor_packet_t)) {
				continue;
			}
			memcpy(&remote, data, sizeof(sensor_packet_t));
			memset(data, 0, 128);

			if (!check_magic(&remote)) {
				continue;
			}

			push_cache(&remote);
		}

		update_period();
		init_packet(&remote);
		memset(&head, 0, sizeof(app_header_t));
		head.type = APP_SENSOR_ID;
		head.len = sizeof(sensor_packet_t);

		int valid = dht_read(&local);
		process_cache(&remote, (valid == 0 ? &local : NULL));
		net_send_up(&head, (const uint8_t*)&remote);

		// debug_print(&remote);

		t_delta = esp_timer_get_time() - t_start;

		esp_timer_start_once(state.timer, (state.period * FACTOR_PERIOD) - t_delta);
	}
}

void app_sensor_init(uint8_t node_id) {
	assert(node_id != 0);

	esp_timer_create_args_t timer_init = {};
	timer_init.callback = app_sensor_timer_cb;
	timer_init.arg = NULL;
	timer_init.dispatch_method = ESP_TIMER_TASK;
	timer_init.name = "app_sensor_timer";
	if (esp_timer_create(&timer_init, &state.timer) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create application timer.");
		return;
	}

	state.events = xEventGroupCreate();
	if (!state.events) {
		ESP_LOGE(TAG, "Failed to create application event group.");
		return;
	}

	xTaskCreatePinnedToCore(
		app_sensor_task,
		"srv_sensor",
		4096,
		NULL,
		PRIO_SENSOR_APP,
		&state.srv_sensor,
		1
	);

	state.id = node_id;
	state.period = DEFAULT_PERIOD;

	memset(&(state.cache), 0, sizeof(state.cache));

	net_register_app(APP_SENSOR_ID);

	esp_timer_start_once(state.timer, state.period * FACTOR_PERIOD);
}

/*
* This method works through the cache of packets from down-stream.  It
*  combines them with a local sensor reading (if available) and fills the
*  samples portion of an output packet.
* NOTE: This method assumes the 'out' argument is pre-initialized.
*/
void process_cache(sensor_packet_t* out, const dht_data_t* local) {
	for (int i = 0; i < 8; ++i) {
		if (state.cache.usage & (1 << i)) {
			const sensor_packet_t* p = state.cache.entry + i;
			combine(&out->temperature, &p->temperature);
			combine(&out->humidity, &p->humidity);
			combine(&out->pm025, &p->pm025);
		}
	}

	if (local != NULL) {
		sensor_sample_t sample = {};

		sample.min = local->temperature;
		sample.max = local->temperature;
		sample.total = local->temperature;
		sample.count = 1;
		sample.id_max = state.id;
		sample.id_min = state.id;
		combine(&out->temperature, &sample);

		sample.min = local->humidity;
		sample.max = local->humidity;
		sample.total = local->humidity;
		sample.count = 1;
		sample.id_max = state.id;
		sample.id_min = state.id;
		combine(&out->humidity, &sample);
	}

	memset(&(state.cache), 0, sizeof(state.cache));
}

/*
* This method adds a sensor packet to the cache.  If an existing packet
*  is already cached from the same node, it is overwritten.
*/
void push_cache(const sensor_packet_t* entry) {
	assert(entry != NULL);

	// Select the slot.
	int slot = -1;
	for (int i = 0; i < 8; ++i) {
		if (state.cache.usage & (1 << i) && state.cache.entry[i].origin == entry->origin) {
			slot = i;
			break;
		}
		if (!(state.cache.usage & (1 << i))) {
			slot = i;
		}
	}

	state.cache.usage |= (1 << slot);
	memcpy(state.cache.entry + slot, entry, sizeof(sensor_packet_t));
}

/*
* This method accumulates a sensor sample into an accumulator of samples.
*/
void combine(sensor_sample_t* acc, const sensor_sample_t* sample) {
	assert(acc != NULL);
	assert(sample != NULL);

	if (sample->count > 0) {
		acc->total += sample->total;
		acc->count += sample->count;
		if (sample->max > acc->max) {
			acc->max = sample->max;
			acc->id_max = sample->id_max;
		}
		if (sample->min < acc->min) {
			acc->min = sample->min;
			acc->id_min = sample->id_min;
		}
	}
}

/*
* This method processes the cache, and sets the application update
*  period to the highest period therein, plus one second.
*/
void update_period() {
	uint16_t p_max = 0;
	for (int i = 0; i < 8; ++i) {
		if (state.cache.usage & (1 << i) && state.cache.entry[i].period > p_max) {
			p_max = state.cache.entry[i].period;
		}
	}
	state.period = (p_max + 10 > p_max ? p_max + 10 : UINT16_MAX);
	state.period = (state.period > DEFAULT_PERIOD ? state.period : DEFAULT_PERIOD);
}

/*
* Predicate method -- returns non-zero if 'magic' field is correct.
*/
int check_magic(const sensor_packet_t* pkt) {
	assert(pkt != NULL);

	const uint8_t* magic = &(pkt->magic);
	if (magic[0] != 's' ||
		magic[1] != 'd' ||
		magic[2] != '0' ||
		magic[3] != '1') {
		return 0;
	}
	return 1;
}

/*
* sensor_packet_t initializer method.
*/
void init_packet(sensor_packet_t* pkt) {
	memset(pkt, 0, sizeof(sensor_packet_t));
	uint8_t* magic = &(pkt->magic);
	magic[0] = 's';
	magic[1] = 'd';
	magic[2] = '0';
	magic[3] = '1';

	pkt->origin = state.id;
	pkt->period = state.period;

	pkt->temperature.max = INT32_MIN;
	pkt->temperature.min = INT32_MAX;

	pkt->humidity.max = INT32_MIN;
	pkt->humidity.min = INT32_MAX;

	pkt->pm025.max = INT32_MIN;
	pkt->pm025.min = INT32_MAX;
}

void debug_sample(const char* label, const sensor_sample_t* s) {
	printf("%s: %.1f %.1f %.1f %d 0x%02X 0x%02X\n",
		label,
		((float)s->min) * 0.1f,
		((float)s->max) * 0.1f,
		((float)s->total) * 0.1f,
		s->count,
		s->id_min,
		s->id_max);
}

void debug_print(const sensor_packet_t* pkt) {
	printf("Period: %.1f\n", ((float)pkt->period) * 0.1f);
	debug_sample("temp", &pkt->temperature);
	debug_sample("humi", &pkt->humidity);
	fflush(stdout);
}