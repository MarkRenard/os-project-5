// pidArray.c was created by Mark Renard on 4/12/2020.
//
// This file defines functions for manipulating a pid_t array.

#include "constants.h"
#include "perrorExit.h"

#define EMPTY -1

// Assigns a value of EMPTY to all elements in a pid_t array
void initPidArray(pid_t * pidArray){
	int i = 0;
	for ( ; i < MAX_RUNNING; i++){
		pidArray[i] = EMPTY;
	}
}

// Returns the next logical pid corresponding to the index of an EMPTY value
int getLogicalPid(const pid_t * pidArray){
	static int lastChosen = -1;

	int i = lastChosen + 1;
	for( ; i != lastChosen; i++){

		// Wraps to 0 if MAX_RUNNING reached
		if (i == MAX_RUNNING) i = 0;

		// Selects current index if pid is free
		if (pidArray[i] == EMPTY){
			lastChosen = i;
			return i;
		}
	}

	perrorExit("getLogicalPid called with no free pids");

	return 0;
}

// Assigns EMPTY to the value in pidArray at the index with value pid
void removePid(pid_t * pidArray, pid_t pid){
	int i = 0;

	// Searches entire pidArray for value pid
	for ( ; i < MAX_RUNNING; i++){

		// If found, overwrites location with value pid
		if (pidArray[i] == pid){
			pidArray[i] = EMPTY;
			return;
		}
	}

	perrorExit("removePid called on array that doesn't contain chosen pid");
}

