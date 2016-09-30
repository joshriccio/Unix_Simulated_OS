/* ------------------------------------------------------------------------
   phase2.c

   University of Arizona
   Computer Science 452

   ------------------------------------------------------------------------ */

#include <phase1.h>
#include <phase2.h>
#include <usloss.h>
#include <stdlib.h>
#include <string.h>

#include "message.h"

/* ------------------------- Prototypes ----------------------------------- */
int start1 (char *);
extern int start2 (char *);
void check_kernel_mode(char * processName);
void disableInterrupts();
void enableInterrupts();
int check_io();
void zeroMailbox(int mboxID);
void zeroSlot(int slotID);
void zeroMboxProc(int pid);
int MboxRelease(int mailboxID);
int MboxCondSend(int mailboxID, void *message, int messageSize);
int MboxCondReceive(int mailboxID, void *message,int maxMessageSize);
int waitDevice(int type, int unit, int *status);
void nullsys(systemArgs *args);
void clockHandler2(int dev, long unit);
void diskHandler(int dev, long unit);
void termHandler(int dev, long unit);
void syscallHandler(int dev, long unit);
/* -------------------------- Globals ------------------------------------- */

int debugflag2 = 0;

// the mail boxes 
mailbox MailBoxTable[MAXMBOX];

mailSlot SlotTable[MAXSLOTS];

mboxProc MboxProcTable[MAXPROC];
// also need array of mail slots, array of function ptrs to system call 
// handlers, ...
void (*systemCallVec[MAXSYSCALLS])(systemArgs *args);

int clockCounter = 0;



/* -------------------------- Functions ----------------------------------- */

/* ------------------------------------------------------------------------
   Name - start1
   Purpose - Initializes mailboxes and interrupt vector.
             Start the phase2 test process.
   Parameters - one, default arg passed by fork1, not used here.
   Returns - one to indicate normal quit.
   Side Effects - lots since it initializes the phase2 data structures.
   ----------------------------------------------------------------------- */
int start1(char *arg)
{
    int kid_pid;
    int status;

    if (DEBUG2 && debugflag2)
        USLOSS_Console("start1(): at beginning\n");

    check_kernel_mode("start1");

    // Disable interrupts
    disableInterrupts();

    // Initialize the mail box table
    for (int i = 0; i < MAXMBOX; i++) {
        MailBoxTable[i].mboxID = i;
        zeroMailbox(i);
    }

    // first seven boxes for interrupt handlers
    for (int i = 0; i < 7; i++) {
        MboxCreate(0,0);
    }

    // slots
    for (int i = 0; i < MAXSLOTS; i++) {
        SlotTable[i].slotID = i;
        zeroSlot(i);
    }
    // process table
    for (int i = 0; i < MAXPROC; i++) {
        zeroMboxProc(i);
    }

    // Initialize USLOSS_IntVec and system call handlers,
    USLOSS_IntVec[USLOSS_CLOCK_INT] = clockHandler2;
    USLOSS_IntVec[USLOSS_DISK_INT] = diskHandler;
    USLOSS_IntVec[USLOSS_TERM_INT] = termHandler;
    USLOSS_IntVec[USLOSS_SYSCALL_INT] = syscallHandler;

    for (int i = 0; i < MAXSYSCALLS; i++) {
        systemCallVec[i] = nullsys;
    }

    // allocate mailboxes for interrupt handlers.  Etc... 

    enableInterrupts();

    // Create a process for start2, then block on a join until start2 quits
    if (DEBUG2 && debugflag2)
        USLOSS_Console("start1(): fork'ing start2 process\n");
    kid_pid = fork1("start2", start2, NULL, 4 * USLOSS_MIN_STACK, 1);
    if ( join(&status) != kid_pid ) {
        USLOSS_Console("start2(): join returned something other than ");
        USLOSS_Console("start2's pid\n");
    }

    return 0;
} /* start1 */


/* ------------------------------------------------------------------------
   Name - MboxCreate
   Purpose - gets a free mailbox from the table of mailboxes and initializes it 
   Parameters - maximum number of slots in the mailbox and the max size of a msg
                sent to the mailbox.
   Returns - -1 to indicate that no mailbox was created, or a value >= 0 as the
             mailbox id.
   Side Effects - initializes one element of the mail box array. 
   ----------------------------------------------------------------------- */
int MboxCreate(int slots, int slot_size) {
    check_kernel_mode("MboxCreate");
    disableInterrupts();

    // Error checking for parameters
    if (slots < 0) {
        enableInterrupts();
        return -1;
    }
    if (slot_size < 0 || slot_size > MAX_MESSAGE) {
        enableInterrupts();
        return -1;
    }

    for (int i = 0; i < MAXMBOX; i++) {
        if (MailBoxTable[i].status == EMPTY) {
            MailBoxTable[i].numSlots = slots;
            MailBoxTable[i].slotsUsed = 0;
            MailBoxTable[i].slotSize = slot_size;
            MailBoxTable[i].status = USED;
            enableInterrupts();
            return i;
        }
    }
    enableInterrupts();
    return -1;
} /* MboxCreate */


/* ------------------------------------------------------------------------
   Name - MboxSend
   Purpose - Put a message into a slot for the indicated mailbox.
             Block the sending process if no slot available.
   Parameters - mailbox id, pointer to data of msg, # of bytes in msg.
   Returns - zero if successful, -1 if invalid args.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int MboxSend(int mbox_id, void *msg_ptr, int msg_size) {
    check_kernel_mode("MboxSend");
    disableInterrupts();

    // error check parameters
    if (mbox_id > MAXMBOX || mbox_id < 0) {
        enableInterrupts();
        return -1;
    }

    mailboxPtr mbptr = &MailBoxTable[mbox_id];

    if (msg_size > mbptr->slotSize) {
        enableInterrupts();
        return -1;
    }

    // Add process to Process Table
    int pid = getpid();
    MboxProcTable[pid % MAXPROC].pid = pid;
    MboxProcTable[pid % MAXPROC].status = ACTIVE;

    // Block if no available slots. Add to next blockSendList
    if (mbptr->numSlots <= mbptr->slotsUsed && mbptr->blockRecvList == NULL) {
        if (mbptr->blockSendList == NULL) {
            mbptr->blockSendList = &MboxProcTable[pid % MAXPROC];
        } else {
            mboxProcPtr temp = mbptr->blockSendList;
            while (temp->nextBlockSend != NULL) {
                temp = temp->nextBlockSend;
            }
            temp->nextBlockSend = &MboxProcTable[pid % MAXPROC];
        }
        blockMe(SEND_BLOCK);
        if(MboxProcTable[pid % MAXPROC].mboxReleased){
          enableInterrupts();  
          return -3;
        }
    }

    // check if process on recieve block list
    if (mbptr->blockRecvList != NULL) {
        if (msg_size > mbptr->blockRecvList->msgSize) {
            enableInterrupts();
            return -1;
        }
        memcpy(mbptr->blockRecvList->message, msg_ptr, msg_size);
        mbptr->blockRecvList->msgSize = msg_size;
        int recvPid = mbptr->blockRecvList->pid;
        mbptr->blockRecvList = mbptr->blockRecvList->nextBlockRecv;
        unblockProc(recvPid);
        enableInterrupts();
        return isZapped() ? -3 : 0;
    }
    
    // find an empty slot in SlotTable
    int slot;
    int i;
    for (i = 0; i < MAXSLOTS; i++) {
        if (SlotTable[i].status == EMPTY) {
           slot = i;
           break;
        }
    }
    if (i == MAXSLOTS) {
        USLOSS_Console("MboxCondSend(): No slots in system. Halting...\n");
        USLOSS_Halt(1);
    }

    // initialize slot
    SlotTable[slot].mboxID = mbptr->mboxID;
    SlotTable[slot].status = USED;
    memcpy(SlotTable[slot].message, msg_ptr, msg_size);
    SlotTable[slot].msgSize = msg_size;

    // place found slot on slotList
    slotPtr temp = mbptr->slotList;
    if (temp == NULL) {
        mbptr->slotList = &SlotTable[slot];
    } else {
        while (temp->nextSlot != NULL) {
            temp = temp->nextSlot;
        }
        temp->nextSlot = &SlotTable[slot];
    }

    mbptr->slotsUsed++;

    enableInterrupts();
    return isZapped() ? -3 : 0;
} /* MboxSend */


/* ------------------------------------------------------------------------
   Name - MboxReceive
   Purpose - Get a msg from a slot of the indicated mailbox.
             Block the receiving process if no msg available.
   Parameters - mailbox id, pointer to put data of msg, max # of bytes that
                can be received.
   Returns - actual size of msg if successful, -1 if invalid args.
   Side Effects - none.
   ----------------------------------------------------------------------- */
int MboxReceive(int mbox_id, void *msg_ptr, int msg_size) {
    check_kernel_mode("MboxReceive");
    disableInterrupts();

    if (MailBoxTable[mbox_id].status == EMPTY) {
        enableInterrupts();
        return -1;
    }

    mailboxPtr mbptr = &MailBoxTable[mbox_id];

    if (msg_size < 0) {
        enableInterrupts();
        return -1;
    }

    // Add process to Process Table
    int pid = getpid();
    MboxProcTable[pid % MAXPROC].pid = pid;
    MboxProcTable[pid % MAXPROC].status = ACTIVE;
    MboxProcTable[pid % MAXPROC].message = msg_ptr;
    MboxProcTable[pid % MAXPROC].msgSize = msg_size;

    slotPtr slotptr = mbptr->slotList;
    
    if (slotptr == NULL) {  // block because no message available
        if (mbptr->blockRecvList == NULL) {
            mbptr->blockRecvList = &MboxProcTable[pid % MAXPROC];
        } else {
            mboxProcPtr temp = mbptr->blockRecvList;
            while (temp->nextBlockRecv != NULL) {
                temp = temp->nextBlockRecv;
            }
            temp->nextBlockRecv = &MboxProcTable[pid % MAXPROC];
        }
        blockMe(RECV_BLOCK);
        if(MboxProcTable[pid % MAXPROC].mboxReleased || isZapped()){
           enableInterrupts(); 
           return -3;
        }
        enableInterrupts();
        return MboxProcTable[pid % MAXPROC].msgSize;
    } else {
        if (slotptr->msgSize > msg_size) {
            enableInterrupts();
            return -1;
        }
        memcpy(msg_ptr, slotptr->message, slotptr->msgSize);
        mbptr->slotList = slotptr->nextSlot;
        int msgSize = slotptr->msgSize;
        zeroSlot(slotptr->slotID);
        mbptr->slotsUsed--;
        
        // wake up a process blocked send
        if (mbptr->blockSendList != NULL) {
            int pid = mbptr->blockSendList->pid;
            mbptr->blockSendList = mbptr->blockSendList->nextBlockSend;
            unblockProc(pid);
        }
        enableInterrupts();
        return isZapped() ? -3 : msgSize;
    }
} /* MboxReceive */

int MboxRelease(int mailboxID) {
    check_kernel_mode("MboxRelease");
    disableInterrupts();

    if (mailboxID < 0 || mailboxID >= MAXMBOX) {
        enableInterrupts();
        return -1;
    }
    if (MailBoxTable[mailboxID].status == EMPTY) {
        enableInterrupts();
        return -1;
    }
    mailboxPtr mbptr = &MailBoxTable[mailboxID];

    if (mbptr->blockSendList == NULL && mbptr->blockRecvList == NULL) {
        zeroMailbox(mailboxID);
        enableInterrupts();
        return isZapped() ? -3 : 0;
    } else {
        while (mbptr->blockSendList != NULL) {
            mbptr->blockSendList->mboxReleased = 1;
            int pid = mbptr->blockSendList->pid;
            mbptr->blockSendList = mbptr->blockSendList->nextBlockSend;
            unblockProc(pid);
            disableInterrupts();
        }
        while (mbptr->blockRecvList != NULL) {
            mbptr->blockRecvList->mboxReleased = 1;
            int pid = mbptr->blockRecvList->pid;
            mbptr->blockRecvList = mbptr->blockRecvList->nextBlockRecv;
            unblockProc(pid);
            disableInterrupts();
        }
    }
    enableInterrupts();
    return isZapped() ? -3 : 0;
}

//This is basically the same as mboxsend, except it doesnt block
int MboxCondSend(int mbox_id, void *msg_ptr, int msg_size){
    check_kernel_mode("MboxCondSend");
    disableInterrupts();

    // error check parameters
    if (mbox_id > MAXMBOX || mbox_id < 0) {
        enableInterrupts();
        return -1;
    }

    mailboxPtr mbptr = &MailBoxTable[mbox_id];

    if (mbptr->numSlots != 0 && msg_size > mbptr->slotSize) {
        enableInterrupts();
        return -1;
    }

    // Add process to Process Table
    int pid = getpid();
    MboxProcTable[pid % MAXPROC].pid = pid;
    MboxProcTable[pid % MAXPROC].status = ACTIVE;

    // No empty slots in mailbox or no slots in system
    if (mbptr->numSlots != 0 && mbptr->numSlots == mbptr->slotsUsed) {
        return -2;
    }

    // zero lost mailbox and no process blocked on recveive list
    if (mbptr->blockRecvList == NULL && mbptr->numSlots == 0) {
        return -1;
    }

    // check if process on recieve block list
    if (mbptr->blockRecvList != NULL) {
        if (msg_size > mbptr->blockRecvList->msgSize) {
            enableInterrupts();
            return -1;
        }
        memcpy(mbptr->blockRecvList->message, msg_ptr, msg_size);
        mbptr->blockRecvList->msgSize = msg_size;
        int recvPid = mbptr->blockRecvList->pid;
        mbptr->blockRecvList = mbptr->blockRecvList->nextBlockRecv;
        unblockProc(recvPid);
        enableInterrupts();
        return isZapped() ? -3 : 0;
    }
    
    // find an empty slot in SlotTable
    int slot;
    int i;
    for (i = 0; i < MAXSLOTS; i++) {
        if (SlotTable[i].status == EMPTY) {
           slot = i;
           break;
        }
    }
    if (i == MAXSLOTS) {
        return -2;
    }

    // initialize slot
    SlotTable[slot].mboxID = mbptr->mboxID;
    SlotTable[slot].status = USED;
    memcpy(SlotTable[slot].message, msg_ptr, msg_size);
    SlotTable[slot].msgSize = msg_size;

    // place found slot on slotList
    slotPtr temp = mbptr->slotList;
    if (temp == NULL) {
        mbptr->slotList = &SlotTable[slot];
    } else {
        while (temp->nextSlot != NULL) {
            temp = temp->nextSlot;
        }
        temp->nextSlot = &SlotTable[slot];
    }

    mbptr->slotsUsed++;

    enableInterrupts();
    return isZapped() ? -3 : 0;
}

int MboxCondReceive(int mbox_id, void *msg_ptr,int msg_size){
    check_kernel_mode("MboxReceive");
    disableInterrupts();

    if (MailBoxTable[mbox_id].status == EMPTY) {
        enableInterrupts();
        return -1;
    }

    mailboxPtr mbptr = &MailBoxTable[mbox_id];

    if (msg_size < 0) {
        enableInterrupts();
        return -1;
    }

    // Add process to Process Table
    int pid = getpid();
    MboxProcTable[pid % MAXPROC].pid = pid;
    MboxProcTable[pid % MAXPROC].status = ACTIVE;
    MboxProcTable[pid % MAXPROC].message = msg_ptr;
    MboxProcTable[pid % MAXPROC].msgSize = msg_size;

    slotPtr slotptr = mbptr->slotList;
    
    if (slotptr == NULL) {  // No message available
        enableInterrupts();
        return -2;
    } else {
        if (slotptr->msgSize > msg_size) {
            enableInterrupts();
            return -1;
        }
        memcpy(msg_ptr, slotptr->message, slotptr->msgSize);
        mbptr->slotList = slotptr->nextSlot;
        int msgSize = slotptr->msgSize;
        zeroSlot(slotptr->slotID);
        mbptr->slotsUsed--;
        
        // wake up a process blocked send
        if (mbptr->blockSendList != NULL) {
            int pid = mbptr->blockSendList->pid;
            mbptr->blockSendList = mbptr->blockSendList->nextBlockSend;
            unblockProc(pid);
        }
        enableInterrupts();
        return isZapped() ? -3 : msgSize;
    }
}

int waitDevice(int type, int unit, int *status){
    check_kernel_mode("MboxReceive");
    disableInterrupts();

    int returnCode;
    int deviceID;
    int clockID = 0;
    int diskID[] = {1, 2};
    int termID[] = {3, 4, 5, 6};

    switch (type) {
        case USLOSS_CLOCK_INT:
            deviceID = clockID;
            break;
        case USLOSS_DISK_INT:
            if (unit >  1 || unit < 0) {
                USLOSS_Console("waitDevice(): invalid unit\n");
            }
            deviceID = diskID[unit];
            break;
        case USLOSS_TERM_INT:
            if (unit >  3 || unit < 0) {
                USLOSS_Console("waitDevice(): invalid unit\n");
            }
            deviceID = termID[unit];
            break;
        default:
            USLOSS_Console("waitDevice(): invalid device or unit type\n");
    }
    returnCode = MboxReceive(deviceID, status, sizeof(int));
    return returnCode == -3 ? -1 : 0;
}

void check_kernel_mode(char * processName) {
    if((USLOSS_PSR_CURRENT_MODE & USLOSS_PsrGet()) == 0) {
        USLOSS_Console("check_kernal_mode(): called while in user mode, by"
                " process %s. Halting...\n", processName);
        USLOSS_Halt(1);
    } 
}

void enableInterrupts() {
    USLOSS_PsrSet(USLOSS_PsrGet() | USLOSS_PSR_CURRENT_INT);
}

/*
 * Disables the interrupts.
 */
void disableInterrupts() {
    USLOSS_PsrSet( USLOSS_PsrGet() & ~USLOSS_PSR_CURRENT_INT );
} /* disableInterrupts */

int check_io() {
    for (int i = 0; i < 7; i++) {   // TODO: Constant for handler mailboxes
        if (MailBoxTable[i].blockRecvList != NULL) {
            return 1;
        }
    }
    return 0;
}

void zeroMailbox(int mboxID) {
    MailBoxTable[mboxID].numSlots = -1;
    MailBoxTable[mboxID].slotsUsed = -1;
    MailBoxTable[mboxID].slotSize = -1;
    MailBoxTable[mboxID].blockSendList = NULL;
    MailBoxTable[mboxID].blockRecvList = NULL;
    MailBoxTable[mboxID].slotList = NULL;
    MailBoxTable[mboxID].status = EMPTY;
}

void zeroSlot(int slotID) {
    SlotTable[slotID].mboxID = -1;
    SlotTable[slotID].status = EMPTY;
    SlotTable[slotID].nextSlot = NULL;
}

void zeroMboxProc(int pid) {
   MboxProcTable[pid % MAXPROC].pid = -1; 
   MboxProcTable[pid % MAXPROC].status = EMPTY; 
   MboxProcTable[pid % MAXPROC].message = NULL; 
   MboxProcTable[pid % MAXPROC].msgSize = -1; 
   MboxProcTable[pid % MAXPROC].mboxReleased = 0; 
   MboxProcTable[pid % MAXPROC].nextBlockSend = NULL; 
   MboxProcTable[pid % MAXPROC].nextBlockRecv = NULL; 
}

/* an error method to handle invalid syscalls */
void nullsys(systemArgs *args)
{
    USLOSS_Console("nullsys(): Invalid syscall. Halting...\n");
    USLOSS_Halt(1);
} /* nullsys */

void clockHandler2(int dev, long unit) {
    check_kernel_mode("clockHandler2");
    disableInterrupts();

    if (dev != USLOSS_CLOCK_INT || unit != 0) {
        USLOSS_Console("clockHandler2(): wrong device or unit\n");
        USLOSS_Halt(1);
    }
    int status;

    clockCounter++;
    if (clockCounter >= 5) {
        USLOSS_DeviceInput(USLOSS_CLOCK_INT, 0, &status);
        MboxCondSend(0, &status, sizeof(int));
        clockCounter = 0;
    }
    timeSlice();
    enableInterrupts();
} /* clockHandler */

void diskHandler(int dev, long unit) {
    check_kernel_mode("diskHandler");
    disableInterrupts();

    if (dev != USLOSS_DISK_INT || unit < 0 || unit > 1) {
        USLOSS_Console("diskHandler(): wrong device or unit\n");
        USLOSS_Halt(1);
    }
    int status;
    int mailboxID = unit + 1;

    USLOSS_DeviceInput(USLOSS_DISK_INT, unit, &status);
    MboxCondSend(mailboxID, &status, sizeof(int));
    enableInterrupts();
} /* diskHandler */

void termHandler(int dev, long unit) {
    check_kernel_mode("termHandler");
    disableInterrupts();

    if (dev != USLOSS_TERM_INT || unit < 0 || unit > 3) {
        USLOSS_Console("termHandler(): wrong device or unit\n");
        USLOSS_Halt(1);
    }
    int status;
    int mailboxID = unit + 3;

    USLOSS_DeviceInput(USLOSS_TERM_INT, unit, &status);
    MboxCondSend(mailboxID, &status, sizeof(int));
    enableInterrupts();
} /* termHandler */

void syscallHandler(int dev, long unit) {
    check_kernel_mode("syscallHandler");
    disableInterrupts();

    if (dev != USLOSS_SYSCALL_INT || unit < 0 || unit > MAXSYSCALLS) {
        USLOSS_Console("syscallHandler(): wrong device or unit\n");
        USLOSS_Halt(1);
    }
    (*systemCallVec[unit])(NULL);
    enableInterrupts();
} /* syscallHandler */
