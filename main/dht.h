
/*
* DHT22 sensor driver header.  This driver is NOT thread-safe, clients
*  must ensure that they only perform dht_read(..) from a single task
*  or the results are undefined.
*/

#include <stdint.h>

typedef struct {
	int16_t temperature;
	int16_t humidity;
} dht_data_t;

void dht_init(uint32_t data_pin);

// Blocking read call; may take up to ~25ms
// Returns 0 on success, non-zero on failure.
int dht_read(dht_data_t* out);
