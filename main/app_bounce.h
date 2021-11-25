
#include <stdint.h>

/*
* Bounce application sends bounce packets it receives from downstream back
*  down, and bounce packets it receives from upstream back up.
* A control value, 'cycle', describes the period (in milliseconds) between
*  performing bounces for this application.
*/

#define APP_BOUNCE_ID 10
#define APP_BOUNCE_BUFFER 115

typedef struct {
	uint32_t magic;
	uint32_t counter;
	uint32_t life;
	uint8_t node_id;
	char buffer[APP_BOUNCE_BUFFER];
} bounce_packet_t;


void app_bounce_init(uint8_t node_id, uint16_t cycle);
void app_bounce_add_message_up(const char* message, uint32_t life);
void app_bounce_add_message_down(const char* message, uint32_t life);
