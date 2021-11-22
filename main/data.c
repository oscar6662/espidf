#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "data.h"
#include "serial_out.h"

typedef struct data_set {
    char dataset[36];
    char source[36];
    int entries;
    int memory;

    struct data_node *next;
} data_set;

data_set * Data_head = NULL;
data_set * Data_current = NULL;

int dataset_exists(const char * name) {

    data_set *t = Data_head;

    while (t != NULL) {
        if (strcmp(t->dataset, name) == 0)
            return 1;

        t = t->next;
    }

    return 0;
}

int create_dataset(const char * first, const char * second) {
    if (dataset_exists(first))
      return 1;

    data_set *link = malloc(sizeof(data_set));
    sprintf(link->dataset, first);
    sprintf(link->source, second);
    link->entries = 0;
    link->memory = ((strlen(first) + 1) + (strlen(second) + 1) + (sizeof(int) * 2) + sizeof(data_set));
    link->next = malloc(sizeof(data_set));
    if (Data_head == NULL) {
      Data_head = link;
      Data_current = link;
    } else{
      Data_current->next = link;
      Data_current = link;
    }
    return 0;
}

int destroy_dataset(const char * name) {
  data_set *t = Data_head;
  data_set *t2 = Data_head;
  if(t == NULL) return 1;
  t = t->next;
  while (t != NULL) {
        if (strcmp(t->dataset, name) == 0) {
          t2->next = t->next;
          free(t);
          return 0;
        }
        t2 = t;
        t = t->next;  
    }
    return 1;
}

int info_dataset(const char * name) {
  data_set *t = Data_head;
  char res[128];
  while (t != NULL) {
        if (strcmp(t->dataset, name) == 0) {
          snprintf(res, 128,  "Source: %s\nEntries: %s\n Memory: %i B", t->dataset, t->source, t->memory);
          serial_out(res);
          return 0;
        }
        t = t->next;  
    }
    return 1;
}