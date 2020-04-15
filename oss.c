// oss.c was created by Mark Renard on 4/11/2020.
//
// This program simulates deadlock detection and resolution.

#include "clock.h"
#include "getSharedMemoryPointers.h"
#include "logging.h"
#include "message.h"
#include "perrorExit.h"
#include "pidArray.h"
#include "protectedClock.h"
#include "qMsg.h"
#include "queue.h"
#include "resourceDescriptor.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Prototypes
static void simulateResourceManagement(ProtectedClock *, ResourceDescriptor *,
				       Message *);
static pid_t launchUserProcess(int simPid);
static void processTerm(ProtectedClock*, ResourceDescriptor*, Message*, int);
static void processRequest(ProtectedClock*, ResourceDescriptor*, Message*, int);
static void processRelease(ProtectedClock*, ResourceDescriptor*, Message*, int);
static void parseMessages(Message * messages);
static void assignSignalHandlers();
static void cleanUpAndExit(int param);
static void cleanUp();

// Constants
static const Clock DETECTION_INTERVAL = {1, 0};
static const Clock MIN_FORK_TIME = {0, 1 * MILLION};
static const Clock MAX_FORK_TIME = {0, 250 * MILLION};
static const Clock MAIN_LOOP_INCREMENT = {0, 50 * MILLION};
static const struct timespec SLEEP = {0, 1000};

// Static global variables
static char * shm;	// Pointer to the shared memory region
static int requestMqId;	// Id of message queue for resource requests & release
static int replyMqId;	// Id of message queue for replies from oss

int main(int argc, char * argv[]){
	ProtectedClock * systemClock;		// Shared memory system clock
	ResourceDescriptor * resources;	// Shared memory resource table
	Message * messages;			// Shared memory message vector

	exeName = argv[0];	// Assigns exeName for perrorExit
	assignSignalHandlers(); // Sets response to ctrl + C & alarm
//	alarm(MAX_SECONDS);	// Limits total execution time
	openLogFile();		// Opens file written to in logging.c

	srand(BASE_SEED - 1);   // Seeds pseudorandom number generator

	// Creates shared memory region and gets pointers
	int size = getSharedMemoryPointers(&shm, &systemClock, &resources, 
			  		   &messages, IPC_CREAT);
        // Creates message queues
        requestMqId = getMessageQueue(DISPATCH_MQ_KEY, MQ_PERMS | IPC_CREAT);
        replyMqId = getMessageQueue(REPLY_MQ_KEY, MQ_PERMS | IPC_CREAT);

	// Initializes system clock and shared arrays
	initPClock(systemClock);
	initResources(resources);
	initMessageArray(messages);
/*
	fprintf(stderr, "shm: %p\n", shm);
	fprintf(stderr, "clock: %p\n", systemClock);
	fprintf(stderr, "resources: %p\n", resources);
	fprintf(stderr, "messages: %p\n\n", messages);
*/
	// Generates processes, grants requests, and resolves deadlock in a loop
	simulateResourceManagement(systemClock, resources, messages);

	cleanUp();

	return 0;
}

// Generates processes, grants requests, and resolves deadlock in a loop
void simulateResourceManagement(ProtectedClock * clock,
			        ResourceDescriptor * resources,
			        Message * messages){

	Clock timeToFork = zeroClock();		 // Time to launch user process 
//	Clock timeToDetect = DETECTION_INTERVAL; // Time to resolve deadlock

	pid_t pidArray[MAX_RUNNING];		// Array of user process pids
	initPidArray(pidArray);			// Sets pids to -1
	int simPid;				// Temporary logical pid storage
	int running = 0;			// Currently running child count
	int launched = 0;			// Total children launched

	int i = 0, m;				// Loop control variables

	// Launches processes and resolves deadlock until limits reached
	do {

		fprintf(stderr, "\nIteration %d:\n", i++);
		printPids(pidArray);

		// Waits for semaphore protecting system clock
		pthread_mutex_lock(&clock->sem);		

		// Launches user processes at random times
		if (clockCompare(clock->time, timeToFork) >= 0){
			 
			// Launches process & records real pid if within limits
			if (running < MAX_RUNNING && launched < MAX_LAUNCHED){
				simPid = getLogicalPid(pidArray);
				pidArray[simPid] = launchUserProcess(simPid);

				running++;
				launched++;
			}

			// Selects new random time to launch a new user process
			incrementClock(&timeToFork, randomTime(MIN_FORK_TIME,
							       MAX_FORK_TIME));
		}

		// Gets messages from queue
		parseMessages(messages);

		// Loops through message array and checks for new messages
		for (m = 0; m < MAX_RUNNING; m++){
			fprintf(stderr, "  msg[%d].type: %d address: %p\n",
				m, messages[m].type, &(messages[m].type));

			// Responds to termination, releasing resources
			if (messages[m].type == TERMINATION){
/*				processTerm(clock, resources, messages, m);
				waitForProcess(pidArray[m]);
				pidArray[m] = EMPTY;

				running--;
*/
			// Otherwise responds to request or release
			} else if (messages[m].type == REQUEST){
				fprintf(stderr, "Request from %d for %d of %d",
					m, messages[m].quantity, messages[m].rNum);
				processRequest(clock, resources, messages, m);
			} else if (messages[m].type == RELEASE){
				processRelease(clock, resources, messages, m);
			}
		}

/*

		// Detects and resolves deadlock at regular intervals
		if (clockCompare(clock->time, timeToDetect) >= 0){
			logDeadlockDetection(clock->time);

			running -= resolveDeadlock(pidArray, resources, 
						   messages);
		}
*/


/*
		// Detects and resolves deadlock at regular intervals
		if (clockCompare(clock->time, timeToDetect) >= 0){
			if (deadlockDetected(clock->time, resources)){	
				do {
					running--;
				} while(!resolveDeadlock(pidArray, resources));
			}
		}
*/
		incrementClock(&clock->time, MAIN_LOOP_INCREMENT);
		printTimeln(stderr, clock->time);

		nanosleep(&SLEEP, NULL);

		// Unlocks the system clock
		pthread_mutex_unlock(&clock->sem);

	} while ((running > 0 || launched < MAX_LAUNCHED) && i < 60);

	sleep(1);
}

// Forks & execs a user process with the assigned logical pid, returns child pid
static pid_t launchUserProcess(int simPid){

	pid_t realPid;

	// Forks, exiting on error
	if ((realPid = fork()) == -1) perrorExit("Failed to fork");

	// Child process calls execl on the user program binary
	if (realPid == 0){
		char sPid[BUFF_SZ];
		sprintf(sPid, "%d", simPid);
		
		execl(USER_PROG_PATH, USER_PROG_PATH, sPid, NULL);
		perrorExit("Failed to execl");
	}


	fprintf(stderr, "launchUserProcess(%d) called - real pid: %d\n", 
		simPid, realPid);

	return realPid;

}

static void parseMessages(Message * messages){
	char * msgText[MSG_SZ];	// Raw text of each message
	int msgInt;		// Integer form of each message
	int rNum;		// The index of the resource
	int quantity;		// Quantity requested or released

	long int qMsgType;	// Raw type of msg
	int simPid;		// simPid of sender

	while (getMessage(requestMqId, msgText, &qMsgType)){
		simPid = (int)(qMsgType - 1);	// Subtract 1 to get simPid
		msgInt = atoi(msgText);		// Converts to encoded int

		// Parses release messages
		if (msgInt < 0){
			quantity = -msgInt % (MAX_INST + 1);
			rNum = -msgInt / (MAX_INST + 1);
			messages[simPid].type = RELEASE;

			fprintf(stderr, "Receiving that P%d released %d of R%d\n",
				simPid, quantity, rNum);

		// Parses request messages
		} else if (msgInt > 0){
			quantity = msgInt % (MAX_INST + 1);
			rNum = msgInt / (MAX_INST + 1);
			messages[simPid].type = REQUEST;

			fprintf(stderr, "Receiving that P%d requested %d of R%d\n",
				simPid, quantity, rNum);

		// Parses termination messages
		} else {
			messages[simPid].type = TERMINATION;

			fprintf(stderr, "Receiving that P%d terminated\n");
			continue;
		}

		messages[simPid].quantity = quantity;
		messages[simPid].rNum = rNum;
	}		

}

// Gets message
//static void parseMessages(Message * messages);

// Responds to a termination message by releasing associated resources
static void processTerm(ProtectedClock * clock, ResourceDescriptor * resources,
			Message * messages, int simPid){

	fprintf(stderr, "Responding to termination of process %d\n", simPid);

	// Frees all resources previously allocated to the process
	int r;
	for (r = 0; r < NUM_RESOURCES; r++){
		resources[r].numAvailable += resources[r].allocations[simPid];
		resources[r].allocations[simPid] = 0;
	}

	// Resets message
	initMessage(&messages[simPid], simPid);
}

// Responds to a request for resources by granting it or enqueueing the request
static void processRequest(ProtectedClock * clock, ResourceDescriptor * resources,
			   Message * messages, int simPid){
	Message * msg = &messages[simPid]; // The message to respond to


	// Grants request if it is less than available
	if (msg->quantity < resources[msg->rNum].numAvailable){
		fprintf(stderr, "Granting P%d request for %d of R%d\n",
			simPid, msg->quantity, msg->rNum);

		resources[msg->rNum].allocations[simPid] += msg->quantity;
		resources[msg->rNum].numAvailable -= msg->quantity;

		msg->quantity = 0;
		msg->type = VOID;

	// Enqueues message otherwise
	} else {
		fprintf(stderr, "Can't meet P%d request for %d of R%d, in q\n",
			simPid, msg->quantity, msg->rNum);

		enqueue(&resources[msg->rNum].waiting, msg);
		msg->type = PENDING_REQUEST;
	}
}

// Releases resources from a process
static void processRelease(ProtectedClock * clock, ResourceDescriptor * resources,
			   Message * messages, int simPid){
	Message * msg = &messages[simPid];

	resources[msg->rNum].allocations[simPid] -= msg->quantity;
	resources[msg->rNum].numAvailable += msg->quantity;

	msg->quantity = 0;
	msg->type = VOID;	
}

// Determines the processes response to ctrl + c or alarm
static void assignSignalHandlers(){
	struct sigaction sigact;

	// Initializes sigaction values
	sigact.sa_handler = cleanUpAndExit;
	sigact.sa_flags = 0;

	// Assigns signals to sigact
	if ((sigemptyset(&sigact.sa_mask) == -1)
	    ||(sigaction(SIGALRM, &sigact, NULL) == -1)
	    ||(sigaction(SIGINT, &sigact, NULL)  == -1)){

		// Prints error message and exits on failure
		char buff[BUFF_SZ];
		sprintf(buff, "%s: Error: Failed to install signal handlers",
			exeName);
		perror(buff);
		exit(1);
	}
}

// Signal handler - closes files, removes shm, terminates children, and exits
static void cleanUpAndExit(int param){

	// Closes files, removes shm, terminates children
	cleanUp();

	// Prints error message
	char buff[BUFF_SZ];
	sprintf(buff,
		 "%s: Error: Terminating after receiving a signal",
		 exeName
	);
	perror(buff);

	// Exits
	exit(1);
}

// Ignores interrupts, kills child processes, closes files, removes shared mem
static void cleanUp(){
	// Handles multiple interrupts by ignoring until exit
	signal(SIGALRM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	// Kills all other processes in the same process group
	kill(0, SIGQUIT);

	// Removes message queues
	removeMessageQueue(requestMqId);
	removeMessageQueue(replyMqId);

	// Detatches from and removes shared memory
	detach(shm);
	removeSegment();
}

