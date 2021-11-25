
#include <stdint.h>

/*
* Sensor application 
*/

#define APP_SENSOR_ID 1

typedef struct {
	int32_t		min;
	int32_t		max;
	int32_t		total;
	uint16_t	count;
	uint8_t		id_min;
	uint8_t		id_max;
} sensor_sample_t;

typedef struct {
	uint32_t			magic;
	uint8_t				origin;
	uint8_t				padding;
	uint16_t			period;
	sensor_sample_t		temperature;
	sensor_sample_t		humidity;
	sensor_sample_t		pm025;
} sensor_packet_t;

void app_sensor_init(uint8_t node_id);
