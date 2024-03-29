// oss.c was created by Mark Renard on 4/11/2020.
//
// This program simulates deadlock detection and resolution.

#include "clock.h"
#include "deadlockDetection.h"
#include "getSharedMemoryPointers.h"
#include "logging.h"
#include "message.h"
#include "matrixRepresentation.h"
#include "perrorExit.h"
#include "pidArray.h"
#include "protectedClock.h"
#include "qMsg.h"
#include "queue.h"
#include "resourceDescriptor.h"
#include "stats.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Prototypes
static void simulateResourceManagement();
static pid_t launchUserProcess(int simPid);
static int parseMessage();
void killProcess(int simPid, pid_t realPid);
static void processTermination(int simPid, pid_t realPid);
static void finalizeTermination(int * released, int simPid, pid_t realPid);
static void releaseResources(int * released, int simPid);
static void waitForProcess(pid_t realPid);
static void processRequest(int simPid);
static void processRelease(int);
static void processQueuedRequests(int rNum);
static void processAllQueuedRequests();
static void processReleasedResourceQueues(int * released);
static void grantRequest(Message * msg);
static void validateState(char * functionName);
static void assignSignalHandlers();
static void cleanUpAndExit(int param);
static void cleanUp();

// Constants
static const Clock DETECTION_INTERVAL = {DETECTION_INTERVAL_SEC, 
					 DETECTION_INTERVAL_NS};
static const Clock MIN_FORK_TIME = {MIN_FORK_TIME_SEC, MIN_FORK_TIME_NS};
static const Clock MAX_FORK_TIME = {MAX_FORK_TIME_SEC, MAX_FORK_TIME_NS};
static const Clock MAIN_LOOP_INCREMENT = {LOOP_INCREMENT_SEC,
					  LOOP_INCREMENT_NS};
static const struct timespec SLEEP = {0, 500000};

// Static global variables
static char * shm;				// Pointer to the shared memory region
static ProtectedClock * systemClock;		// Shared memory system clock
static ResourceDescriptor * resources;		// Shared memory resource table
static Message * messages;			// Shared memory message vector

static int requestMqId;	// Id of message queue for resource requests & release
int replyMqId;		// Id of message queue for replies from oss

int main(int argc, char * argv[]){

	exeName = argv[0];	// Assigns exeName for perrorExit
	assignSignalHandlers(); // Sets response to ctrl + C & alarm
	openLogFile();		// Opens file written to in logging.c

	srand(BASE_SEED - 1);   // Seeds pseudorandom number generator

	// Creates shared memory region and gets pointers
	getSharedMemoryPointers(&shm, &systemClock, &resources, &messages, 
				IPC_CREAT);

        // Creates message queues
        requestMqId = getMessageQueue(REQUEST_MQ_KEY, MQ_PERMS | IPC_CREAT);
        replyMqId = getMessageQueue(REPLY_MQ_KEY, MQ_PERMS | IPC_CREAT);

	initStats();

	// Initializes system clock and shared arrays
	initPClock(systemClock);
	initResources(resources);
	initMessageArray(messages);
	
	// Generates processes, grants requests, and resolves deadlock in a loop
	simulateResourceManagement();

	logStats();

	cleanUp();

	return 0;
}

// Generates processes, grants requests, and resolves deadlock in a loop
void simulateResourceManagement(){
	Clock timeToFork = zeroClock();		 // Time to launch user process 
	Clock timeToDetect = DETECTION_INTERVAL; // Time to resolve deadlock

	pid_t pidArray[MAX_RUNNING];		// Array of user process pids
	initPidArray(pidArray);			// Sets pids to -1
	pid_t simPid;				// Temporary pid storage

	int running = 0;			// Currently running child count
	int launched = 0;			// Total children launched
	int terminated = 0;			// Killed last deadlock res.
	int m;					// Message index

	// Launches processes and resolves deadlock until limits reached
	do {

		// Launches user processes at random times
		if (clockCompare(getPTime(systemClock), timeToFork) >= 0){
			 
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

		// Responds to new messages from the queue
		while ((m = parseMessage()) != -1){
			if (messages[m].type == REQUEST){
				processRequest(m);
			} else if (messages[m].type == RELEASE) {
				processRelease(m);
			} else if (messages[m].type == TERMINATION){
				processTermination(m, pidArray[m]);
			
				// Removes from running processes
				pidArray[m] = EMPTY;
				running--;
			}
		}

		// Detects and resolves deadlock at regular intervals
		if (clockCompare(getPTime(systemClock), timeToDetect) >= 0){
			logDeadlockDetection(systemClock->time);

			// Resolves deadlock
			terminated = resolveDeadlock(pidArray, resources, 
						   messages);
			if (terminated > 0) processAllQueuedRequests();
			running -= terminated;

			// Selects new time to detect deadlock
			incrementClock(&timeToDetect, DETECTION_INTERVAL);
		}

		// Increments and unlocks the system clock
		incrementPClock(systemClock, MAIN_LOOP_INCREMENT);

		nanosleep(&SLEEP, NULL);

	} while ((running > 0 || launched < MAX_LAUNCHED));

}

// Forks & execs a user process with the assigned logical pid, returns child pid
static pid_t launchUserProcess(int simPid){
	pid_t realPid;

	// Forks, exiting on error
	if ((realPid = fork()) == -1){
		perrorExit("Failed to fork");
	}

	// Child process calls execl on the user program binary
	if (realPid == 0){
		char sPid[BUFF_SZ];
		sprintf(sPid, "%d", simPid);
		
		execl(USER_PROG_PATH, USER_PROG_PATH, sPid, NULL);
		perrorExit("Failed to execl");
	}

	return realPid;
}

// Parses & returns the pid of a newly received message, or -1 if there are none
static int parseMessage(){
	char msgText[MSG_SZ];	// Raw text of each message
	int msgInt;		// Integer form of each message
	int rNum;		// The index of the resource
	int quantity;		// Quantity requested or released

	long int qMsgType;	// Raw type of msg
	int simPid;		// simPid of sender

	if (getMessage(requestMqId, msgText, &qMsgType)){
		simPid = (int)(qMsgType - 1);	// Subtract 1 to get simPid
		msgInt = atoi(msgText);		// Converts to encoded int

		// Parses release messages
		if (msgInt < 0){
			quantity = -msgInt % (MAX_INST + 1);
			rNum = -msgInt / (MAX_INST + 1);
			messages[simPid].type = RELEASE;


		// Parses request messages
		} else if (msgInt > 0){
			quantity = msgInt % (MAX_INST + 1);
			rNum = msgInt / (MAX_INST + 1);
			messages[simPid].type = REQUEST;

			logRequestDetection(simPid, rNum, quantity, 
					    systemClock->time);

		// Parses termination messages
		} else {
			messages[simPid].type = TERMINATION;


			return simPid;
		}

		// Sets values in shared array
		messages[simPid].quantity = quantity;
		messages[simPid].rNum = rNum;

		return simPid;
	}else{
		// Returns -1 if no messages found in message queue
		return -1;
	}
}

// Messages a program to terminate, releases its resources, and writes to log
void killProcess(int simPid, pid_t realPid){
	int released[NUM_RESOURCES]; // Array of prevous resource allocations
	
	// Sends the message killing the process
	sendMessage(replyMqId, KILL_MSG, simPid + 1);

	// Releases and records previously held resources, calls waitpid
	finalizeTermination(released, simPid, realPid);

	// Logging
	logKill(simPid);
	logRelease(released);
}

// Releases resources of a finished process, waits, checks queues, writes to log
static void processTermination(int simPid, pid_t realPid){

	int released[NUM_RESOURCES]; // Array of prevous resource allocations

	sendMessage(replyMqId, "termination confirmed", simPid + 1);

	// Releases and records previously held resources, calls waitpid
	finalizeTermination(released, simPid, realPid);

	// Checks queued requests for released resources, grants if possible
	processReleasedResourceQueues(released);

	// Logging
	logCompletion(simPid);
#ifdef VERBOSE
	logRelease(released);
#endif

}

// Releases and records previously held resources, calls waitpid, resets message
static void finalizeTermination(int * released, int simPid, pid_t realPid){

	releaseResources(released, simPid);
	waitForProcess(realPid);
	resetMessage(&messages[simPid]);

	// Validates the state of the simulated system
	char buff[BUFF_SZ];
	sprintf(buff, "finalizeTermination on proces %d", simPid);
	validateState(buff);
}

// Counts resources previously held by the process as available, writes to array
static void releaseResources(int * released, int simPid){
	int r;
	for (r = 0; r < NUM_RESOURCES; r++){
		released[r] = resources[r].allocations[simPid];

		// Increases numAvailable if the resoruce is not shared
		if (!resources[r].shareable){
			resources[r].numAvailable += \
				resources[r].allocations[simPid];
		}
		resources[r].allocations[simPid] = 0;
	}
}

// Waits for the process with pid equal to the realPid parameter
static void waitForProcess(pid_t realPid){

        pid_t retval;
        while(((retval = waitpid(realPid, NULL, 0)) == -1)
                 && errno == EINTR);

        if (errno == ECHILD)
                perrorExit("waited for non-existent child");

}

// Responds to a request for resources by granting it or enqueueing the request
static void processRequest(int simPid){
	Message * msg = &messages[simPid]; // The message to respond to

	// Grants request if it is less than available
	if (msg->quantity <= resources[msg->rNum].numAvailable){
		grantRequest(msg);

	// Enqueues message otherwise
	} else {

		// Logs request denial
		logEnqueue(simPid, msg->quantity, msg->rNum, 
			resources[msg->rNum].numAvailable);

		enqueue(&resources[msg->rNum].waiting, msg);
		msg->type = PENDING_REQUEST;
	}

	// Validates the state of the simulated system
	char buff[BUFF_SZ];
	sprintf(buff, "processRequest(%d)", simPid);
	validateState(buff);
}

// Examines a single request queue and grants old requests if able
static void processQueuedRequests(int rNum){
	Message * msg;				// Stores each queued message	
	Queue * q = &resources[rNum].waiting;	// The queue to process
	int qCount = q->count;			// Initial number in queue

	int i = 0;
	for ( ; i < qCount; i++){

		msg = q->front;
		if (msg == NULL)
			perrorExit("processQueuedReuqest() - msg NULL");

		if (msg->quantity <= 0)
			perrorExit("processQueuedRequests() - request <= 0");

		// Grants request if possible
		if (msg->quantity <= resources[rNum].numAvailable){

			grantRequest(msg);
			dequeue(q);

		// Re-enqueues if not
		} else {

			dequeue(q);
			enqueue(q, msg);
		}

		// Validates the state of the simulated system
		char buff[BUFF_SZ];
		sprintf(buff, "processQueuedRequests(%d), iteration %d,", 
			rNum, i);
		validateState(buff);
	}
}

// Grants a request for resources
static void grantRequest(Message * msg){

	// Increeases allocation and if not shareable, decreases availability
	resources[msg->rNum].allocations[msg->simPid] += msg->quantity;
	if (!resources[msg->rNum].shareable)
		resources[msg->rNum].numAvailable -= msg->quantity;


	// Prints granted request to log file
	logAllocation(msg->simPid, msg->rNum, msg->quantity, 
		      systemClock->time);

	// Logs resource table every 20 granted requests by default
	logTable(resources);

	// Resets msg
	msg->quantity = 0;
	msg->type = VOID;
	
	// Validates the state of the simulated system
	char buff[BUFF_SZ];
	sprintf(buff, "grantRequest(msg P%d, %d of R%d)", msg->simPid, 
		msg->quantity, msg->rNum);
	validateState(buff);

	// Replies with acknowlegement
	sendMessage(replyMqId, "request confirmed", msg->simPid + 1);
}

// Calls processQueuedRequest on all resource numbers
static void processAllQueuedRequests(){
	int i = 0;
	for ( ; i < NUM_RESOURCES; i++){
		processQueuedRequests(i);
	}
}

// Calls processQueuedRequest on resources in released vector
static void processReleasedResourceQueues(int * released){
	int i = 0;
	for ( ; i < NUM_RESOURCES; i++){
		if (released[i] > 0) processQueuedRequests(i);
	}
}

// Releases resources from a process
static void processRelease(int simPid){
	Message * msg = &messages[simPid];

	logResourceRelease(simPid, msg->rNum, msg->quantity, 
			   systemClock->time);

	resources[msg->rNum].allocations[simPid] -= msg->quantity;

	if (!resources[msg->rNum].shareable)
		resources[msg->rNum].numAvailable += msg->quantity;

	msg->quantity = 0;
	msg->type = VOID;

	processAllQueuedRequests();

	// Validates the state of the simulated system
	char buff[BUFF_SZ];
	sprintf(buff, "processRelease(%d)", simPid);
	validateState(buff);

	// Replies with acknowlegement
	sendMessage(replyMqId, "release confirmed", simPid + 1);
}

// This function calls perrorExit if any allocation < 0 or allocation > existing
static void validateState(char * functionName){
	int i;
	char buff[BUFF_SZ];

	for (i = 0; i < NUM_RESOURCES; i++){
		if (resources[i].numAvailable > resources[i].numInstances){
			sprintf(buff, "After call to %s, %d of R%d are"\
				" available, but only %d instances exist",
				functionName, resources[i].numAvailable, i,
				resources[i].numInstances);
			perrorExit(buff);
		} else if (resources[i].numAvailable < 0) {
			sprintf(buff, "After call to %s, %d of R%d available",
				functionName, resources[i].numAvailable, i);
			perrorExit(buff); 
		}
	}
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

// Kills child processes, closes message queues & files, removes shared mem
static void cleanUp(){
	// Handles multiple interrupts by ignoring until exit
	signal(SIGALRM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);

	// Kills all other processes in the same process group
	kill(0, SIGQUIT);

	// Destroys semaphore protecting system clock
	while (pthread_mutex_destroy(&systemClock->sem) != 0 && errno == EBUSY);
	if (errno == EINVAL){
		char buff[BUFF_SZ];
		sprintf(buff, "%s: Error: ", exeName);
		perror("Attempted to destroy invalid semaphore");
	}

	// Removes message queues
	removeMessageQueue(requestMqId);
	removeMessageQueue(replyMqId);

	closeLogFile();

	// Detatches from and removes shared memory
	detach(shm);
	removeSegment();
}

