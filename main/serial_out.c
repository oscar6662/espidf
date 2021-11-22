#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "limits.h"
#include "serial_out.h"

#define MSG_BUFFER_LENGTH 256

/**
 * 
 * serial_out. Everything that gets printed goes through here.
 * The purpose of the function is to make sure the output is not to big.
 * 
 * */

void serial_out(const char* string) {
	int end = strlen(string);
	if (end >= MSG_BUFFER_LENGTH) {
		// Error: Output too long.
		return;
	}
	char msg_buffer[MSG_BUFFER_LENGTH+1];
	memset(msg_buffer, 0, MSG_BUFFER_LENGTH+1);
	strcpy(msg_buffer, string);
	msg_buffer[end] = '\n';
	printf(msg_buffer);
	fflush(stdout);
}