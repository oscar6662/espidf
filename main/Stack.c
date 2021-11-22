#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "limits.h"
#include "serial_out.h"
#include "Stack.h"
#include "util.h"
#include "command_functions.h"


struct Stack* createStack(unsigned capacity)
{
    struct Stack* stack = (struct Stack*)malloc(sizeof(struct Stack));
    stack->capacity = capacity;
    stack->top = -1;
    stack->array = (int*)malloc(stack->capacity * sizeof(int));
    return stack;
}
 
int isFull(struct Stack* stack)
{
    sprintf(error, "Stack is full");
    return stack->top == stack->capacity - 1;
}
 
int isEmpty(struct Stack* stack)
{
    sprintf(error, "Stack is empty");
    return stack->top == -1;
}

void push(struct Stack* stack, char * item)
{
    int number;
    if(check_string(item)) {
      number = atoi(item);
    } else {
      sprintf(error, "invalid argument for push command");
      serial_out("invalid argument for push command");
      return;
    }
    if (isFull(stack)){
        sprintf(error, "stack is full");
        serial_out("overflow");
        return;
    }
    stack->array[++stack->top] = number;
    serial_out("done");
}
 
void pop(struct Stack* stack)
{
    if (isEmpty(stack)){
      serial_out("undefined");
      return;
    }
    char out[31];
    itoa(stack->array[stack->top--], out, 10);
    serial_out(out);
}

int peek(struct Stack* stack)
{
    if (isEmpty(stack))
        return INT_MIN;
    return stack->array[stack->top];
}
