// queue.h was created by Mark Renard on 2/5/2020 and modified on 4/17/2020.
// This file defines function prototypes for a queue structure

#ifndef QUEUE_H
#define QUEUE_H

#include "message.h"
#include <stdio.h>

typedef struct queue {
	Message * back;
	Message * front;
	int count;
} Queue;

void printQueue(FILE *, const Queue *);
void addToFront(Queue * q, Message * pcb);
void initializeQueue(Queue *);
void enqueue(Queue *, Message *);
Message * dequeue(Queue *);
void removeFromCurrentQueue(Message *);

#endif

