#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "background.h"
#include "serial_out.h"

#define MAIN_PRIORITY 0
#define LOW_PRIORITY 1
#define HIGH_PRIORITY 4
#define WAIT_QUEUE ((TickType_t)(10 / portTICK_PERIOD_MS))
#define DELAY ((TickType_t)(50 / portTICK_PERIOD_MS))
#define RESPONSE_LENGTH 128


void factor(void *pvParameter) {
  int id = (int *)pvParameter;
  int n = get_value(id);
  char response[RESPONSE_LENGTH] = "";

  snprintf(response, 10, "%d", n);
  strcat(response, ": ");

  if (n < 0) {
    n = abs(n);
  }

  while (n%2 == 0) {
        strcat(response, "2 "); 
        n = n/2;
    }

    for (int i = 3; i <= sqrt(n); i = i+2) {
        while (n%i == 0) {
            char dest[10] = "";
            itoa(i, dest, 10);
            strcat(response, dest);
            strcat(response, " ");
            n = n/i;
        }
    }

    if (n > 2) {
      char dest[10] = "";
      itoa(n, dest, 10);
      strcat(response, dest);
      strcat(response, " ");
    }

    set_result(id, response);
    vTaskDelete(NULL);

}

int start_factoring (const int number, const int counter) {
   BaseType_t success;
    while (new_task(counter, number, "factor") == -1) {
        vTaskDelay(DELAY);
    }
    char out[6];
    itoa(counter, out, 10);
    success = xTaskCreatePinnedToCore(
        &factor,
        out,
        4096,
        (void *)counter,
        LOW_PRIORITY,
        NULL,
        tskNO_AFFINITY);

    if (success == pdPASS) {
        return 0;
    }

    return 1;
}