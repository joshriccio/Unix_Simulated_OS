#include <stdlib.h>
#include <usloss.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <usyscall.h>
#include <sems.h>
#include <libuser.h>
#include <string.h>

void setUserMode();
void spawn(systemArgs *args);
int spawnReal(char *name, int (* userFunc)(char *), char *arg, int stackSize, 
        int priority);
int spawnLaunch(char *arg);
void checkKernelMode(char * processName);
void wait(systemArgs *args);
void terminate(systemArgs *args);
int waitReal(int *status);
void addChildToList(procPtr3 child);
void removeFromChildList(procPtr3 process);
void semCreate(systemArgs *args);
void addToSemBlockList(procPtr3 process, int semIndex);
void semP(systemArgs *args);
void semV(systemArgs *args);
void semFree(systemArgs *args);
void getPid(systemArgs *args);
void getTimeOfDay(systemArgs *args);
void cpuTime(systemArgs *args);

// remove prototype TODO:
extern int start3(char *arg);

procStruct3 procTable[MAXPROC]; // Process Table

semStruct semTable[MAXSEMS]; // Semaphore Table

int start2(char *arg) {
    int pid;
    int status;

    /* Check kernel mode here. */
    checkKernelMode("start2");

    // initialize all sturcts in the process table to EMPTY
    for (int i = 0; i < MAXPROC; i++) {
        procTable[i].status = EMPTY;
    }

    // initialize semaphore table
    for (int i = 0; i < MAXSEMS; i++) {
        semTable[i].status = EMPTY;
    }

    // initialize systemCallVec to system call functions
    systemCallVec[SYS_SPAWN] = spawn;
    systemCallVec[SYS_WAIT] = wait;
    systemCallVec[SYS_TERMINATE] = terminate;
    systemCallVec[SYS_SEMCREATE] = semCreate;
    systemCallVec[SYS_SEMP] = semP;
	systemCallVec[SYS_SEMV] = semV;
	systemCallVec[SYS_SEMFREE] = semFree;
	systemCallVec[SYS_GETPID] = getPid;
	systemCallVec[SYS_GETTIMEOFDAY] = getTimeOfDay;
	systemCallVec[SYS_CPUTIME] = cpuTime;

    // TODO: finish initialized systemCallVec

    // TODO: place start2 in process table

    /*
     * Create first user-level process and wait for it to finish.
     * These are lower-case because they are not system calls;
     * system calls cannot be invoked from kernel mode.
     * Assumes kernel-mode versions of the system calls
     * with lower-case names.  I.e., Spawn is the user-mode function
     * called by the test cases; spawn is the kernel-mode function that
     * is called by the syscallHandler; spawnReal is the function that
     * contains the implementation and is called by spawn.
     *
     * Spawn() is in libuser.c.  It invokes USLOSS_Syscall()
     * The system call handler calls a function named spawn() -- note lower
     * case -- that extracts the arguments from the sysargs pointer, and
     * checks them for possible errors.  This function then calls spawnReal().
     *
     * Here, we only call spawnReal(), since we are already in kernel mode.
     *
     * spawnReal() will create the process by using a call to fork1 to
     * create a process executing the code in spawnLaunch().  spawnReal()
     * and spawnLaunch() then coordinate the completion of the phase 3
     * process table entries needed for the new process.  spawnReal() will
     * return to the original caller of Spawn, while spawnLaunch() will
     * begin executing the function passed to Spawn. spawnLaunch() will
     * need to switch to user-mode before allowing user code to execute.
     * spawnReal() will return to spawn(), which will put the return
     * values back into the sysargs pointer, switch to user-mode, and 
     * return to the user code that called Spawn.
     */
    pid = spawnReal("start3", start3, NULL, USLOSS_MIN_STACK, 3);

    /* Call the waitReal version of your wait code here.
     * You call waitReal (rather than Wait) because start2 is running
     * in kernel (not user) mode.
     */
    pid = waitReal(&status);

    quit(0);
    return pid;
} /* start2 */

/* 
 *check_kernel_mode
 */
void checkKernelMode(char * processName) {
    if((USLOSS_PSR_CURRENT_MODE & USLOSS_PsrGet()) == 0) {
        USLOSS_Console("check_kernal_mode(): called while in user mode, by"
                " process %s. Halting...\n", processName);
        USLOSS_Halt(1);
    }   
}

void spawn(systemArgs *args) {
    long pid;

    // check for invalid arguments
    if ((long) args->number != SYS_SPAWN) {
        args->arg4 = (void *) -1;
        return;
    }
    if ((long) args->arg3 < USLOSS_MIN_STACK) {
        args->arg4 = (void *) -1;
        return;
    }
    if ((long) args->arg4 > MINPRIORITY || (long) args->arg4 < MAXPRIORITY ) {
        args->arg4 = (void *) -1;
        return;
    }
    pid = spawnReal((char *) args->arg5, args->arg1, args->arg2, 
            (long) args->arg3, (long) args->arg4);

    args->arg1 = (void *) pid; // newly created process id; or -1
    args->arg4 = (void *) 0;   // inputs to function valid
    setUserMode();
}

int spawnReal(char *name, int (* userFunc)(char *), char *arg, int stackSize, 
        int priority) {

    int childPID;
    int mailboxID;

    childPID = fork1(name, spawnLaunch, arg, stackSize, priority);

    if (childPID < 0) {
        return childPID;
    }

    // if child has not ran yet
    if (procTable[childPID % MAXPROC].status == EMPTY) {
        mailboxID = MboxCreate(0, 0);
        procTable[childPID % MAXPROC].mboxID = mailboxID;
        procTable[childPID % MAXPROC].status = ACTIVE;
    } else {
        mailboxID = procTable[childPID % MAXPROC].mboxID;
    }

    // add child information to the process table
    procTable[childPID % MAXPROC].pid = childPID;
    strcpy(procTable[childPID % MAXPROC].name, name);
    procTable[childPID % MAXPROC].priority = priority;
    procTable[childPID % MAXPROC].userFunc = userFunc;
    procTable[childPID % MAXPROC].childProcPtr = NULL;
    procTable[childPID % MAXPROC].nextSiblingPtr = NULL;
    procTable[childPID % MAXPROC].nextSemBlock = NULL;
    if (arg == NULL) {
        procTable[childPID % MAXPROC].startArg[0] = 0;
    } else {
        strcpy(procTable[childPID % MAXPROC].startArg, arg);
    }
    procTable[childPID % MAXPROC].stackSize = stackSize;

    if (getpid() != START2_PID) {
        procTable[childPID % MAXPROC].parentPtr = 
            &procTable[getpid() % MAXPROC];
        addChildToList(&procTable[childPID % MAXPROC]);
    }

    // wake up child if blocked in spawnLaunch
    MboxCondSend(mailboxID, NULL, 0); 

    return childPID;
}

int spawnLaunch(char *arg) {
    int mailboxID;
    int pid = getpid();
    int userFuncReturnValue;

    // parent has not set up process table
    if (procTable[pid % MAXPROC].status == EMPTY) {
        procTable[pid % MAXPROC].status = ACTIVE;
        mailboxID = MboxCreate(0, 0);
        procTable[pid % MAXPROC].mboxID = mailboxID;
        MboxReceive(mailboxID, NULL, 0);
    }

    if (isZapped()) {
        setUserMode();
        Terminate(99);
    }

    setUserMode();

    // calls userFunc for child process to execute
    userFuncReturnValue = procTable[pid % MAXPROC].userFunc(
            procTable[pid % MAXPROC].startArg);
    
    Terminate(userFuncReturnValue);
    return 0;
}

void wait(systemArgs *args) {
    int status;
    long kidPID;

    // check if correct system call
    if ((long) args->number != SYS_WAIT) {
        args->arg2 = (void *) -1;
        return;
    }

    kidPID = waitReal(&status);

    procTable[getpid() % MAXPROC].status = ACTIVE;
    
    if (kidPID == -2) {
        args->arg1 = (void *) 0;
        args->arg2 = (void *) -2;
    } else {
        args->arg1 = (void *) kidPID;
        args->arg2 = ((void *) (long) status);
    }
    setUserMode();
}

int waitReal(int *status) {
    procTable[getpid() % MAXPROC].status = WAIT_BLOCK;
    return join(status);
}

void terminate(systemArgs *args) {
    procPtr3 parent = &procTable[getpid() % MAXPROC];

    if (parent->childProcPtr != NULL) {
        while (parent->childProcPtr != NULL) {
            // TODO:USLOSS_Console("zapping: %d\n", parent->childProcPtr->pid);
            zap(parent->childProcPtr->pid);
            //parent->childProcPtr->status = EMPTY;
            //parent->childProcPtr = parent->childProcPtr->nextSiblingPtr;
        }
    }

    if (parent->pid != START3_PID && parent->parentPtr != NULL) {
        removeFromChildList(&procTable[getpid() % MAXPROC]);
    }

    parent->status = EMPTY; // redundent in removeFromChildList
    quit(((int) (long) args->arg1));
}

void semCreate(systemArgs *args) {
    int index;

    if ((long) args->arg1 < 0) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    for (index = 0; index < MAXSEMS; index++) {
        if (semTable[index].status == EMPTY) {
            break;
        }
    }

    if (index == MAXSEMS) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    semTable[index].status = ACTIVE;
    semTable[index].count = (long) args->arg1;
    semTable[index].blockedList = NULL;
    semTable[index].mboxID = MboxCreate(1, 0);

    args->arg1 = ((void *) (long) index);
    args->arg4 = ((void *) (long) 0);
    setUserMode();
}

void semP(systemArgs *args) {
    int semIndex = ((int) (long) args->arg1);

    if (semIndex < 0 || semIndex > MAXSEMS) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    semStruct *semaphore = &semTable[semIndex];

    if (semaphore->status == EMPTY) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    MboxSend(semaphore->mboxID, NULL, 0);
    if (semaphore->count < 1) {
        addToSemBlockList(&procTable[getpid() % MAXPROC], semIndex);
        MboxReceive(semaphore->mboxID, NULL, 0);
        MboxReceive(procTable[getpid() % MAXPROC].mboxID, NULL, 0);
        if (semaphore->status == EMPTY) {
            setUserMode();
            Terminate(1);
        }
    } else {
        MboxReceive(semaphore->mboxID, NULL, 0);
    }
    semaphore->count--;
    args->arg4 = ((void *) (long) 0);
    setUserMode();
}

void semV(systemArgs *args) {
    int semIndex = ((int) (long) args->arg1);

    if (semIndex < 0 || semIndex > MAXSEMS) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    semStruct *semaphore = &semTable[semIndex];

    if (semaphore->status == EMPTY) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    MboxSend(semaphore->mboxID, NULL, 0);
    semaphore->count++;
    
    if (semaphore->blockedList != NULL) {
        int blockProcMboxID = semaphore->blockedList->mboxID;
        semaphore->blockedList = semaphore->blockedList->nextSemBlock;
        MboxReceive(semaphore->mboxID, NULL, 0);
        MboxSend(blockProcMboxID, NULL, 0);
    } else {
        MboxReceive(semaphore->mboxID, NULL, 0);
    }
    args->arg4 = ((void *) (long) 0);
    setUserMode();
}

void semFree(systemArgs *args) {
    int semIndex = ((int) (long) args->arg1);

    if (semIndex < 0 || semIndex > MAXSEMS) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    semStruct *semaphore = &semTable[semIndex];

    if (semaphore->status == EMPTY) {
        args->arg4 = ((void *) (long) -1);
        return;
    }

    semaphore->status = EMPTY;

    // Handle block list
    if (semaphore->blockedList != NULL) {
        while (semaphore->blockedList != NULL) {
            int privateMboxID = semaphore->blockedList->mboxID;
            MboxSend(privateMboxID, NULL, 0);
            semaphore->blockedList = semaphore->blockedList->nextSemBlock;
        }
        args->arg4 = ((void *) (long) 1);
    } else {
        args->arg4 = ((void *) (long) 0);
    }
    setUserMode();
}

void getPid(systemArgs *args) {
    args->arg1 = ((void *) (long) getpid());
    setUserMode();
}

void getTimeOfDay(systemArgs *args) {
    args->arg1 = ((void *) (long) USLOSS_Clock());
    setUserMode();
}

void cpuTime(systemArgs *args) {
    args->arg1 = ((void *) (long) readtime());
    setUserMode();
}

void setUserMode() {
    USLOSS_PsrSet(USLOSS_PsrGet() & 14);
}

void addChildToList(procPtr3 child) {
    procPtr3 parent = &procTable[getpid() % MAXPROC];
    
    if (parent->childProcPtr == NULL) {
        parent->childProcPtr = child;
    } else {
        procPtr3 sibling = parent->childProcPtr;
        while (sibling->nextSiblingPtr != NULL) {
            sibling = sibling->nextSiblingPtr;
        }
        sibling->nextSiblingPtr = child;
    }
}

/*------------------------------------------------------------------
|  Function removeFromChildList
|
|  Purpose:  Finds process in parent's childlist, removes process, reasigns
|            all important processes
|
|  Parameters:
|            procPtr process, process to be deleted
|
|  Returns:  void
|
|  Side Effects:  Process is removed from parent's childList
*-------------------------------------------------------------------*/
void removeFromChildList(procPtr3 process) {
    procPtr3 temp = process;
    // process is at the head of the linked list
    if (process == process->parentPtr->childProcPtr) {
        process->parentPtr->childProcPtr = process->nextSiblingPtr;
    } else { // process is in the middle or end of linked list
        temp = process->parentPtr->childProcPtr;
        while (temp->nextSiblingPtr != process) {
            temp = temp->nextSiblingPtr;
        }    
        temp->nextSiblingPtr = temp->nextSiblingPtr->nextSiblingPtr;
    }    
    process->status = EMPTY;
}/* removeFromChildList */

void addToSemBlockList(procPtr3 process, int semIndex) {
    procPtr3 blockList = semTable[semIndex].blockedList;
    
    if (blockList == NULL) {
        semTable[semIndex].blockedList = process;
    } else {
        procPtr3 temp = blockList;
        while (temp->nextSemBlock != NULL) {
            temp = temp->nextSemBlock;
        }
        temp->nextSemBlock = process;
    }
}
