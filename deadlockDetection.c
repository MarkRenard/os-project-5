// resolveDeadlock.c was created by Mark Renard on 4/14/2020.
//
// This file contains functions for detecting deadlock, including Dr. Sanjiv 
// Bhatia's implementation of the deadlock detection algorithm.

#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "constants.h"
#include "message.h"
#include "perrorExit.h"
#include "pidArray.h"
#include "resourceDescriptor.h"

static bool req_lt_avail ( const int*req, const int*avail, const int pnum, \
                    const int num_res )
{
    int i = 0 ;
    for ( ; i < num_res; i++ )
        if ( req[pnum*num_res+i] > avail[i] ) 
            break;
    
    return ( i == num_res );
}



bool deadlock ( const int*available, const int m, const int n, \
                const int*request, const int*allocated , int * deadlocked) 
{
    int  work[m];       // m resources
    bool finish[n];     // n processes
   
    int i; 
    for ( i = 0 ; i < m; i++ )
	work[i] = available[i];

    for ( i = 0 ; i < n; finish[i++] = false );
    
    fprintf(stderr, "\n\nFinished sequence: <");
    int p = 0;
    for ( ; p < n; p++ )   // For each process
    {
        if ( finish[p] ) continue;
        if ( req_lt_avail ( request, work, p, m ) )
        {
            finish[p] = true;
            fprintf(stderr, "p%d, ", p);
            
            for ( i = 0 ; i < m; i++ )
                work[i] += allocated[p*m+i];
            p = -1;
        }
        
    }
    fprintf(stderr, ">\n");
    
    fprintf(stderr, "Deadlock with processes <");
    bool deadlock = false;
    for ( p = 0; p < n; p++ )
        if ( ! finish[p] )
        {
            fprintf(stderr, "p%d, ", p);
            deadlocked[p] = true;
            deadlock = true;
        }
    fprintf(stderr, ">\n");
    
    return ( deadlock );
}

// Sets all the values in a vector to n
static void initVector(int * vector, int size, int n){
	int i = 0;
	for ( ; i < size; i++){
		vector[i] = n;
	}
}

// Sets the allocation matrix
static void setAllocated(ResourceDescriptor * resources, 
			  int * allocated){
	int m = NUM_RESOURCES;

	int r, p;
	for (p = 0; p < MAX_RUNNING; p++) {
		for (r = 0; r < NUM_RESOURCES; r++){
			allocated[p*m + r] = resources[r].allocations[p];
		}
	}
}

// Sets the request matrix
static void setRequest(ResourceDescriptor * resources,
			int * request){
	int m = NUM_RESOURCES;
	int n = MAX_RUNNING;

	Message * msg;

	int i, r;

	// Initializes values to 0
	for (i = 0; i < m * n; i++){
		request[i] = 0;
	}

	// Records quantity requested of each resource
	for (r = 0; r < NUM_RESOURCES; r++){
		msg = resources[r].waiting.front;	

		// Traverses the request queue for each resource
		while(msg != NULL){
			int p = msg->simPid;
			request[p*m + r] = msg->quantity;
			
			msg = msg->previous;
		}
	}
}

// Sets the available vector
static void setAvailable(ResourceDescriptor * resources,
			 int * available) {
	int r;
	for (r = 0; r < NUM_RESOURCES; r++){
		available[r] = resources[r].numAvailable;	
	}
}

// Kills the process with the greatest request and removes it from the pidArray
static void killAProcess(pid_t * pidArray, int * deadlocked, int * request,
			 ResourceDescriptor * resources){
	int p, r;		// Process and resource index variables
	pid_t retval;		// The return value of waitpid

	int maxRequest = 0;	// Greatest quantity of a resource requested
	int greediest = -1;	// Index of the process with greatest request

	// Loops through all logical pids
	for (p = 0; p < MAX_RUNNING; p++){

		// Selects deadlocked processes
		if (deadlocked[p]){

			// Finds whether each deadlocked process is greediest
			for (r = 0; r < NUM_RESOURCES; r++){
			       if (resources[r].allocations[p] > maxRequest)
				       maxRequest = resources[r].allocations[p];
				       greediest = p;
			}
		}
	}

	// Kills the process
	pid_t greedyPid = pidArray[greediest];
	kill(greedyPid, SIGQUIT);
	pidArray[greediest] = EMPTY;
	
	// Waits for the process
	while((retval = waitpid(greedyPid, NULL, 0)) == -1 && errno == EINTR);
	if (errno == ECHILD) 
		perrorExit("killAProcess - waited for non-existent child");

	
}

// Detects and resolves deadlock - returns num killed and removes pids
int resolveDeadlock(pid_t * pidArray, ResourceDescriptor * resources,
		    volatile Message * messages){

	int allocated[NUM_RESOURCES * MAX_RUNNING];	// Resource allocation
	int request[NUM_RESOURCES * MAX_RUNNING];	// Current requests
	int available[NUM_RESOURCES];			// Available resources

	int deadlocked[MAX_RUNNING];	// Whether each pid is deadlocked

	int killed = 0;			// The number of terminated processes

	// Initializes vectors
	setAllocated(resources, allocated);
	setRequest(resources, request);
	setAvailable(resources, available);
	initVector(deadlocked, MAX_RUNNING, 0);

	// If deadlock exists, repeatedly kills processes until resolved
	while(deadlock(available, NUM_RESOURCES, MAX_RUNNING, request,
		       allocated, deadlocked)){
		killAProcess(pidArray, deadlocked, request, resources);
		killed++;
	}

	return killed;
}


