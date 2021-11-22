#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "limits.h"
#include "Stack.h"
#include "serial_out.h"
#include "command_functions.h"
#include "wifi.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_log.h"


#define MSG_BUFFER_LENGTH 256

#define TASKS  5
#define HIGH_PRIORITY  4  
#define LOW_PRIORITY   1  
#define MAIN_PRIORITY  0  
struct Stack* stack;
int counter;
char query[MSG_BUFFER_LENGTH];
const TickType_t read_delay = 50 / portTICK_PERIOD_MS;


void check_what_came_in(void *pvParameter) {
	char * string_with_arguments = strtok(query," ");
	if (strlen(query) > 0) {
		if (strcasecmp(query, "ping") == 0) {
			print_pong();
		} else if (strcasecmp(query, "mac") == 0) {
			print_mac();
		} else if (strcasecmp(query, "id") == 0) {
			print_id();
		} else if (strcasecmp(query, "version") == 0) {
			print_version();
		} else if (strcasecmp(query, "error") == 0) {
			print_error();
		} else if (strcasecmp(string_with_arguments,"store") == 0) {
      char * first_argument = strtok(NULL," ");
      int second_argument = strtok(NULL," ");
      store_variable(first_argument,second_argument);
    } else if (strcasecmp(string_with_arguments,"query") == 0) {
      char * first_argument = strtok(NULL," ");
      print_variable(first_argument);
    } else if (strcasecmp(string_with_arguments,"push") == 0) {
      push(stack, strtok(NULL," "));
    } else if (strcasecmp(query,"pop") == 0) {
      pop(stack);
    } else if (strcasecmp(query,"add") == 0) {
      char * first_argument = strtok(NULL," ");
      char * second_argument = strtok(NULL," ");
      add(first_argument,second_argument);
    } else if (strcasecmp(query,"factor") == 0) {
      char * first_argument = strtok(NULL," ");
      if (first_argument != NULL) {
        factor_with_arguments(counter, first_argument);
      } else {
        factor_no_arguments(counter, stack);
      }
      counter++;
    } else if (strcasecmp(query,"ps") == 0) {
      ps();
    } else if (strcasecmp(string_with_arguments,"result") == 0) {
      char * first_argument = strtok(NULL," ");
      result(first_argument);
    } else if (strcasecmp(string_with_arguments,"data_create") == 0) {
      char * first_argument = strtok(NULL," ");
      char * second_argument = strtok(NULL," ");
      data_create(first_argument, second_argument);
    } else if (strcasecmp(string_with_arguments,"data_destroy") == 0) {
      char * first_argument = strtok(NULL," ");
      data_destroy(first_argument);
    } else if (strcasecmp(string_with_arguments,"data_info") == 0) {
      char * first_argument = strtok(NULL," ");
      data_info(first_argument);
    } else if (strcasecmp(query,"net_locate") == 0) {
      net_locate();
    } else if (strcasecmp(query,"net_status") == 0) {
      net_status();
    } else if (strcasecmp(query,"net_reset") == 0) {
      net_reset();
    } else if (strcasecmp(query,"net_table") == 0) {
      net_table();
    } else {
      sprintf(error, "Command not recognized");
    }
	} else{
		serial_out("command error");
	}

	serial_out("");

	vTaskDelete(NULL);
}

void main_task(void *pvParameter) {
	serial_out("firmware ready");
	while (true)
	{
		int complete = 0;
		int at = 0;
		int whitespace = 0;
		int consecutive_whitespace = 0;
		int leading_whitespace = 0;
		int trailing_whitespace = 0;

		memset(query, 0, MSG_BUFFER_LENGTH);

		while (!complete) {
			if (at >= 256) {
				serial_out("input too long\n");
				break;
			}
			int result = fgetc(stdin);

			if (at == 0 && ((char)result == ' ')) {
				leading_whitespace = 1;
			}

			if (result == EOF) {
				vTaskDelay(read_delay);
				continue;
			} else if ((char)result == '\n') {
				if (whitespace) {
					trailing_whitespace = 1;
				}
				complete = true;
			} else {
				query[at++] = (char)result;
			}

			if ((char)result == ' ') {
				if (whitespace) {
					consecutive_whitespace = 1;
				}
				whitespace = 1;
			} else {
				whitespace = 0;
			}
		}

		if (complete) {
			if (leading_whitespace) {
				serial_out("unrecognized command\n");
			}
			else if (consecutive_whitespace || trailing_whitespace) {
				serial_out("unrecognized argument\n");
			} else {
				xTaskCreatePinnedToCore(
					&check_what_came_in,
					"check_what_came_in",
					8192,
					NULL,
					HIGH_PRIORITY,
					NULL,
					tskNO_AFFINITY 
				);
			}
		}
	}
}


void app_main(void) {
	stack = createStack(32);
  esp_log_level_set("wifi", ESP_LOG_NONE);  // disable the default wifi logging
  ESP_ERROR_CHECK(nvs_flash_init());    
  wifi_init();      
  espnow_init();    
	counter = 0;

	xTaskCreate(
		&main_task,	   
		"main_task",   
		2048,		   
		NULL,		   
		MAIN_PRIORITY, 
		NULL		 
	);
}
