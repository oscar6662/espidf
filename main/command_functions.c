#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "limits.h"
#include "Stack.h"
#include "serial_out.h"
#include "factor.h"
#include "util.h"
#include "background.h"
#include "data.h"
#include "command_functions.h"

#define MAX_STORED_VARIABLES 32
#define RESPONSE_LENGTH 128

const TickType_t CONN_DELAY = 500 / portTICK_PERIOD_MS;


void print_mac() {
  unsigned char mac[6] = {0};
  char readytoprint[18];
  esp_efuse_mac_get_default(mac);
  sprintf(readytoprint,"%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
	serial_out(readytoprint);
}
//ping-pong
void print_pong () {
  serial_out("pong");
}
//id
void print_id () {
  serial_out("s107");
}
//version
void print_version () {
  serial_out("1.0.0");
}
//error
void print_error () {
  serial_out(error);
}

typedef struct variable {
  char name[17];
  int value;
} variable;

variable stored_variable_array[MAX_STORED_VARIABLES];
int stored_count = 0;


bool get_variable_by_name (const char * name, int * returnvalue) {
  if(name == NULL) return false;
  for (int i = 0; i < MAX_STORED_VARIABLES; i++) {
    if(strcasecmp(stored_variable_array[i].name,name) == 0) {
      *returnvalue = stored_variable_array[i].value;
      return true;
    }
  }
  return false;
}

void set_variable_by_name (const char * name, int value) {
  for (int i = 0; i < MAX_STORED_VARIABLES; i++) {
    if(strcasecmp(stored_variable_array[i].name,name) == 0) {
      stored_variable_array[i].value = value;
    }
  }
}

void store_variable(const char* name, char * value) {
  int number;
  if(check_string(value)) {
    number = atoi(value);
  } else {
    sprintf(error, "invalid value for store command");
    serial_out("invalid value for store command");
    return;
  }
  if (stored_count >= MAX_STORED_VARIABLES){
      sprintf(error, "Stored array is full");
      return;
    }
  for (int i = 0; i < strlen(name); i++) {
    if((int)name[i] < 65 || (int)name[i] > 122) {
      sprintf(error, "Variable \"%s\" not possible. Use only Letters or _",name);
      return;
    }
  }
  if (abs(number) >= INT_MAX) {
    sprintf(error, "Variable value to big/small");
    return;
  }
  int foundvalue = 0;
  if (get_variable_by_name(name, &foundvalue)) {
    set_variable_by_name(name,number);
  }
    variable var = {};
    strcpy(var.name, name);
    var.value = number;
    
    stored_variable_array[stored_count] = var;
    stored_count++;
}

void print_variable(const char * variable) {
  int foundvalue = 0;
  if (get_variable_by_name(variable, &foundvalue)){
    char out[31];
    itoa(foundvalue, out, 10);
    serial_out(out);
  } else{
    sprintf(error, "Variable \"%s\" not found",variable);
  }
}

void add (const char * first, const char * second) {
  int value_one = 0;
  if (!get_variable_by_name(first, &value_one)) {
    if(first == NULL){
      serial_out("insert a variable name");
      sprintf(error, "No variable inserted!");
      return;
    }
    serial_out("variable does not exist");
    sprintf(error, "Variable \"%s\" not found",first);
    return;
  }
  int value_two = 0;
  if (!get_variable_by_name(second, &value_two)) {
    //value_two = peek(stack);
  }
  char out[31];
  itoa(value_one+value_two, out, 10);
  serial_out(out);
}

void factor_no_arguments (int counter, struct Stack* stack) {
  if (!isEmpty(stack)) {
    if (start_factoring(peek(stack), counter) == 0) {
      char out[31] = "id";
      itoa(counter, out+2, 10);
      serial_out(out);
    } else {
      serial_out("Could not start the task");
    }
  } else {
    serial_out("Nothing to factor");
  }
}

void factor_with_arguments (int counter, const char * first) {
  int value = 0;
  if (get_variable_by_name(first, &value)) {
    if (start_factoring(value, counter) == 0) {
      char out[31] = "id";
      itoa(counter, out+2, 10);
      serial_out(out);
    } else {
      serial_out("Could not start the task");
    }
  } else if (check_string(first)) {
    if (start_factoring(atoi(first), counter) == 0) {
      char out[31] = "id";
      itoa(counter, out+2, 10);
      serial_out(out);
    } else {
      serial_out("Could not start the task");
    }
  } else {
    sprintf(error, "Invalid argument. Variable name does not exist");
    serial_out("undefined");
    return;
  }
}

void ps() {
  show_background_tasks();
}

void result(const char * ident) {
  char n[10] = "";
  sprintf(n, ident+2);
  if (check_string(n)) {
    int id = atoi(n);
    char res[RESPONSE_LENGTH] = "";
    sprintf(res, get_result(id));
    serial_out(res);
    return;
  } else {
    sprintf(error, "undefined");
    serial_out("undefined");
    return;
  }
}


void data_create(const char * first, const char * second) {
    if (!(strcasecmp(second, "NOISE") == 0 || strcasecmp(second, "BT_DEMO") == 0)) {
      sprintf(error, "invalid source");
      serial_out("invalid source");
      return;
    }

    if (create_dataset(first, second) != 0) {
      sprintf(error, "invalid name");
      serial_out("invalid name");
      return;
    }

    serial_out("data set created");
}

void data_destroy(const char * first) {
  if (destroy_dataset(first) != 0) {
    serial_out("invalid name");
    sprintf(error, "could not destroy dataset");
    return;
  }
    serial_out("data set destroyed");
}

void data_info (const char * first) {
  if (info_dataset(first) != 0) {
    serial_out("invalid name");
    sprintf(error, "invalid name");
    return;
  }

}