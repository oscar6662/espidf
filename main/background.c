#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "factor.h"
#include "serial_out.h"
#include "command_functions.h"

#define MAIN_PRIORITY 0
#define LOW_PRIORITY 1
#define HIGH_PRIORITY 4
#define WAIT_QUEUE ((TickType_t)(10 / portTICK_PERIOD_MS))
#define DELAY ((TickType_t)(50 / portTICK_PERIOD_MS))
#define RESPONSE_LENGTH 128
#define MAX_TASKS 8

int tasks = 0;

enum states {
  A, //Active
  P, //Pending
  C, //Complete
  CX, //Complete* (result retrieved)
};

typedef struct background_tasks
{
    char function[16];
    int counter;
    enum states state;
    int value;
    char result[RESPONSE_LENGTH];

    struct background_tasks *next;
} background_tasks;

background_tasks *head = NULL;
background_tasks *current = NULL;


SemaphoreHandle_t head_access;

void initialize_background_tasks() {
    head_access = xSemaphoreCreateBinary();
    assert(head_access != NULL);
    xSemaphoreGive(head_access);
}

void set_result(int id, char * result) {
  background_tasks *t = head;

    while (t->counter != id) {
        if (t->next == NULL) {
            sprintf(error, "could not set the task result");
            return;
        }
        else {
            t = t->next;
        }
    }
    sprintf(t->result, "%s", result);
    t->state = C;
    tasks--;
    return;
}

int get_value(int id) {
  background_tasks *t = head;
    while (t->counter != id) {
        if (t->next == NULL) {
            return 1;
        }
        else {
            t = t->next;
        }
    }
    t->state = A;
    return t->value;
}

char * get_result(int id) {
  background_tasks *t = head;
  background_tasks *temp = head;
  if (t == NULL) {
    return "undefined";
  }
  
    while (t->counter != id) {
        if (t->next == NULL) {
            return "undefined";
        }
        else {
            t = t->next;
        }
    }
    if (t->state == A || t->state == P) {
      return "pending";
    }
    t->state = CX;
    if (t == head) {
      head = t->next;
      free(t);
    } else if (temp->state == CX) {
      while (temp->state == CX) {
        head = temp->next;
        free(temp);
        temp = head;
      }
    }
    return t->result;
}

int new_task(int counter, int value, char* function) {
    if (tasks >= 8) {
      return -1;
    }
    background_tasks *link = malloc(sizeof(background_tasks));
    link->counter = counter;
    link->state = P;
    link->value = value;
    sprintf(link->function, function);
    if (head == NULL) {
      head = link;
      current = link;
    } else{
      current->next = link;
      current = link;
    }
    link->next = NULL;
    tasks++;
    return 0;
}

void show_background_tasks() {
  background_tasks *t = head;
  char res[RESPONSE_LENGTH];
  if  (t == NULL) {
    serial_out("empty set");
    return;
  }
   while (t != NULL) {
        memset(res, 0, RESPONSE_LENGTH);
        if (t->state == A) {
            snprintf(res, RESPONSE_LENGTH, "%s id%i %s", t->function, t->counter, "active");
        }
        else if (t->state == P) {
            snprintf(res, RESPONSE_LENGTH, "%s id%i %s", t->function, t->counter, "pending");
        }
        else if (t->state == C) {
            snprintf(res, RESPONSE_LENGTH, "%s id%i %s", t->function, t->counter, "complete");
        }
        else {
            snprintf(res, RESPONSE_LENGTH, "%s id%i %s", t->function, t->counter, "complete*");
        }
        serial_out(res);
        t = t->next;
    }
}
