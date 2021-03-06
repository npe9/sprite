/* 
 * signals.c --
 *
 * Copyright 1988 Regents of the University of California.
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 * This contains routines that deal with Sprite signals.  See the man pages
 * on signals for an explanation of the Sprite signaling facilities.  The
 * only thing that is explained in these comments is the implementation of
 * these facilities.
 *
 * SYNCHRONIZATION
 *
 * Whenever the signal state of a process is modified, the sig monitor lock
 * is usually grabbed, and the process is usually locked (e.g., LocalSend).
 * Code that doesn't obtain the sig monitor lock is in migration and
 * signals initialization (e.g., fork & exec); the process is locked in
 * these cases.  Currently (18-Dec-1991) code that doesn't lock the process
 * (e.g., some calls to SigClearPendingMask) only works on the current
 * process (i.e., a process is changing its own pending signals list), and
 * is therefore unlikely to interfere with the code that didn't obtain the
 * sig monitor lock.  (XXX This really ought to get cleaned up.)
 * 
 * When the signal state is looked at no locking is done.  It is assumed
 * that there are two ways that the signal state will be looked at:
 *
 *      1) A process in the middle of executing a system call
 *         will check to see if any signals are pending before waiting for
 *         an extended period (i.e. waiting for a read to complete).  If
 *         not then it will go to sleep until either a signal comes in or
 *         the thing that it is waiting for completes.  This does not
 *         require any synchronization on reading because the routine
 *         which is used to put a process to sleep (see Sync_WaitEventInt)
 *         will check for signals with the master lock down before the
 *         process is put to sleep.  If there are signals pending, then
 *         the sleep call will return immediately.  Otherwise if a signal
 *         comes in after the process goes to sleep then it will be
 *         awakened by the Sig_Send which calls Sync_WakeWaitingProcess which
 *	   synchronizes correctly with the Sync_Wait calls.  Thus there
 *	   is no way to miss a signal in this case.
 *
 *	2) A process is returning to user mode after trapping into the kernel
 *	   for some reason and it wants to see if signals are pending before
 *	   it returns.  In this case the trap handler (see Exc_Trap) will 
 *	   disable interrupts before checking to see if signals are pending.  
 *	   If they are then it will enable interrupts and process the signal.
 *	   Otherwise it will return to user mode with interrupts being enabled
 *	   on the return to user mode.  If a signal came in when 
 *	   interrupts were disabled then once interrupts are enabled the 
 *	   process will be interrupted and return back into the kernel.  
 *	   Likewise once the user process returns to user mode if a signal is 
 *	   delivered then the user process will be interrupted.  Interruption
 *	   is possible of course only on a multi-processor. Once interrupted it
 *	   will be forced back into the kernel where it will discover a
 *	   signal.  Thus a signal cannot be missed in this case either.
 *
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint

#include <sprite.h>
#include <stdlib.h>
#include <sig.h>
#include <sync.h>
#include <dbg.h>
#include <list.h>
#include <proc.h>
#include <procMigrate.h>
#include <status.h>
#include <sched.h>
#include <sigInt.h>
#include <rpc.h>
#include <net.h>
#include <vm.h>
#include <bstring.h>
#include <stdio.h>

unsigned int 	sigBitMasks[SIG_NUM_SIGNALS];
int		sigDefActions[SIG_NUM_SIGNALS];
int		sigCanHoldMask;

Sync_Lock	sigLock;
Sync_Condition	signalCondition;

static void LocalSend _ARGS_((Proc_ControlBlock *procPtr, int sigNum, int code,
			Address addr));


/*
 *----------------------------------------------------------------------
 *
 * Sig_Init --
 *
 *	Initialize the signal data structures.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The set of bit masks and the set of default actions are set up.
 *
 *----------------------------------------------------------------------
 */

void
Sig_Init()
{
    int	i;

    Sync_LockInitDynamic(&sigLock, "Sig:sigLock");

    for (i = SIG_MIN_SIGNAL; i < SIG_NUM_SIGNALS; i++) {
	sigBitMasks[i] = Sig_NumberToMask(i);
	sigDefActions[i] = SIG_KILL_ACTION;
    }

    /* 
     * Note that SIG_RESUME uses the "kill" action, even though it's not 
     * actually used to kill the process.
     */
    sigDefActions[SIG_DEBUG]		= SIG_DEBUG_ACTION;
    sigDefActions[SIG_ARITH_FAULT]	= SIG_DEBUG_ACTION;
    sigDefActions[SIG_ILL_INST] 	= SIG_DEBUG_ACTION;
    sigDefActions[SIG_ADDR_FAULT] 	= SIG_DEBUG_ACTION;
    sigDefActions[SIG_BREAKPOINT] 	= SIG_DEBUG_ACTION;
    sigDefActions[SIG_TRACE_TRAP] 	= SIG_DEBUG_ACTION;
    sigDefActions[SIG_MIGRATE_TRAP] 	= SIG_MIGRATE_ACTION;
    sigDefActions[SIG_MIGRATE_HOME] 	= SIG_MIGRATE_ACTION;
    sigDefActions[SIG_SUSPEND]		= SIG_SUSPEND_ACTION;
    sigDefActions[SIG_TTY_INPUT]	= SIG_SUSPEND_ACTION;
    sigDefActions[SIG_URGENT]		= SIG_IGNORE_ACTION;
    sigDefActions[SIG_CHILD]		= SIG_IGNORE_ACTION;
    sigDefActions[SIG_TTY_SUSPEND]	= SIG_SUSPEND_ACTION;
    sigDefActions[SIG_TTY_OUTPUT]	= SIG_SUSPEND_ACTION;
    sigDefActions[SIG_IO_READY]		= SIG_IGNORE_ACTION;
    sigDefActions[SIG_WINDOW_CHANGE]	= SIG_IGNORE_ACTION;

    sigCanHoldMask = 
	      ~(sigBitMasks[SIG_ARITH_FAULT] | sigBitMasks[SIG_ILL_INST] |
		sigBitMasks[SIG_ADDR_FAULT]  | sigBitMasks[SIG_KILL] |
		sigBitMasks[SIG_BREAKPOINT]  | sigBitMasks[SIG_TRACE_TRAP] |
		sigBitMasks[SIG_MIGRATE_HOME] | sigBitMasks[SIG_SUSPEND]);
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_ProcInit --
 *
 *	Initialize the signal data structures for the first process.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Signal state initialized.
 *
 *----------------------------------------------------------------------
 */
void
Sig_ProcInit(procPtr)
    register	Proc_ControlBlock	*procPtr;
{
    procPtr->sigHoldMask = 0;
    procPtr->sigPendingMask = 0;
    bcopy((Address)sigDefActions,(Address)procPtr->sigActions,
              sizeof(sigDefActions));
    bzero((Address)procPtr->sigMasks,sizeof(procPtr->sigMasks)); 
    bzero((Address)procPtr->sigCodes,sizeof(procPtr->sigCodes));
    procPtr->sigFlags = 0;
}



/*
 *----------------------------------------------------------------------
 *
 * Sig_Fork --
 *
 *	Copy over the parents signal state into the child.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Signal state copied from parent to child and pending mask cleared in
 *	child.  Migration is held until the first return into user mode.
 *
 *----------------------------------------------------------------------
 */
void
Sig_Fork(parProcPtr, childProcPtr)
    register	Proc_ControlBlock	*parProcPtr;
    register	Proc_ControlBlock	*childProcPtr;
{
    /*
     * Copy the parent's signal state to the child.  Set up migration
     * to be held initially.  On the first return to user mode, after
     * signals are processed, migration will be reenabled.
     */
    childProcPtr->sigHoldMask = parProcPtr->sigHoldMask |
	    Sig_NumberToMask(SIG_MIGRATE_TRAP);
    childProcPtr->sigPendingMask = 0;
    bcopy((Address)parProcPtr->sigActions, 
	  (Address)childProcPtr->sigActions,
	  sizeof(childProcPtr->sigActions)); 
    bcopy((Address)parProcPtr->sigMasks, 
	  (Address)childProcPtr->sigMasks,
    	  sizeof(childProcPtr->sigMasks)); 
    bzero((Address)childProcPtr->sigCodes,sizeof(childProcPtr->sigCodes));
    childProcPtr->sigFlags = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_Exec --
 *
 *	Clear all signal handlers on exec.  Assumed called with the proc
 * 	table entry locked such that signals against this process are
 *	prevented.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	All signal handlers are cleared and the pending mask is cleared.
 *
 *----------------------------------------------------------------------
 */
void
Sig_Exec(procPtr)
    Proc_ControlBlock	*procPtr;
{
    register	int	*actionPtr;
    register	int	i;

    for (i = SIG_MIN_SIGNAL, actionPtr = &procPtr->sigActions[SIG_MIN_SIGNAL]; 
	 i < SIG_NUM_SIGNALS;
	 i++, actionPtr++) {
	if (*actionPtr > SIG_SUSPEND_ACTION) {
	    /*
	     * The action contains a signal handler to call.  Reset back to
	     * the default action.
	     */
	    *actionPtr = sigDefActions[i];
	    procPtr->sigMasks[i] = 0;
	}
    }
    procPtr->sigPendingMask = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_ChangeState --
 *
 *	Set the entire signal state of the process to that given.  When
 *	setting the state verify that improper signals are not blocked or
 *	ignored.  The process is assumed to be locked.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The signal actions and hold mask will be set for the process.  
 *	Might change the suspend/resume flags in the PCB.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
Sig_ChangeState(procPtr, actions, sigMasks, pendingMask, sigCodes, holdMask)
    register	Proc_ControlBlock	*procPtr;
    int					actions[];
    register	int			sigMasks[];
    int					pendingMask;
    int					sigCodes[];
    int					holdMask;
{
    register	int	i;
    register	int	*actionPtr;

    LOCK_MONITOR;

    for (i = SIG_MIN_SIGNAL, actionPtr = &actions[SIG_MIN_SIGNAL]; 
	 i < SIG_NUM_SIGNALS; 
	 i++, actionPtr++) {
	if (i == SIG_KILL) {
	    continue;
	}
	procPtr->sigActions[i] = *actionPtr;
	if (*actionPtr == SIG_IGNORE_ACTION) {
	    /*
	     * If is ignore action then make sure that is not one of the
	     * signals that cannot be ignored.  If not then remove the signal
	     * from the pending mask.
	     */
	    if (sigBitMasks[i] & sigCanHoldMask) {
		pendingMask &= ~sigBitMasks[i];
	    } else {
		procPtr->sigActions[i] = sigDefActions[i];
	    }
	} else if (*actionPtr > SIG_NUM_ACTIONS) {
	    /*
	     * If greater than one of the actions then must be the address
	     * of a signal handler so store the signal mask.
	     */
	    procPtr->sigMasks[i] = sigMasks[i] & sigCanHoldMask;
	}
    }

    procPtr->sigPendingMask = pendingMask;
    
    /* 
     * Make sure the suspend/resume flags are consistent with the pending 
     * signals mask.
     */
    procPtr->genFlags &= ~PROC_RESUME_PROCESS;
    if (pendingMask & Sig_NumberToMask(SIG_SUSPEND)) {
	procPtr->genFlags |= PROC_PENDING_SUSPEND;
    } else {
	procPtr->genFlags &= ~PROC_PENDING_SUSPEND;
    }

    procPtr->sigHoldMask = holdMask & sigCanHoldMask;
    bcopy((Address) sigCodes, (Address) procPtr->sigCodes,
          sizeof(procPtr->sigCodes));
    procPtr->specialHandling = 1;

    UNLOCK_MONITOR;
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_UserSend --
 *	Send a signal to a process.  Call the internal routine to do the
 *	work.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus	
Sig_UserSend(sigNum, pid, familyID)
    int		sigNum;		/* The signal to send. */
    Proc_PID	pid;		/* The id number of the process or process
				   family. */
    Boolean	familyID;	/* Whether the id is a process id or a process
				   group id. */
{
    return(Sig_Send(sigNum, SIG_NO_CODE, pid, familyID, (Address)0));
}


/*
 *----------------------------------------------------------------------
 *
 * LocalSend --
 *
 *	Send a signal to a process on the local machine.  It assumed that the
 *	process is locked down when we are called.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Signal pending mask and code modified.  The process's suspend and 
 *	resume flags might also get changed.
 *
 *----------------------------------------------------------------------
 */

ENTRY static void
LocalSend(procPtr, sigNum, code, addr)
    register	Proc_ControlBlock	*procPtr;
    int					sigNum;
    int					code;
    Address				addr;
{
    int	sigBitMask;

    LOCK_MONITOR;

    /*
     * Signals can't be sent to kernel processes unless the system is being
     * shutdown since kernel processes never get the opportunity to handle
     * signals.
     */
    if ((procPtr->genFlags & PROC_KERNEL) && !sys_ShuttingDown) {
	UNLOCK_MONITOR;
	return;
    }

    if ((procPtr->sigActions[sigNum] == SIG_DEBUG_ACTION) &&
	proc_KillMigratedDebugs && (procPtr->genFlags & PROC_FOREIGN)) {
	/*
	 * Kill the process rather than letting it go silently into that
	 * good night (on the wrong machine).   Debugging migrated
	 * processes is nasty.  It would be nice if we could redirect
	 * the printf to the process's home node, too.
	 */
	sigNum = SIG_KILL;
	if (proc_MigDebugLevel > 1) {
	    printf("Warning: killing a migrated process that would have gone into the debugger, pid %x rpid %x uid %d.\n",
	        procPtr->processID, (int) procPtr->peerProcessID, 
		procPtr->userID);

	}
    }

    /*
     * Only send the signal if it shouldn't be ignored and if it isn't
     * a signal to migrate an unmigrated process.  (The latter can easily
     * happen when signalling a process family to migrate home.)
     */
    if ((procPtr->sigActions[sigNum] != SIG_IGNORE_ACTION) &&
	!((sigNum == SIG_MIGRATE_HOME) && (procPtr->peerHostID == NIL))) {

	if (sigNum == SIG_RESUME) {
	    /* 
	     * Resume the suspended process.
	     */
	    Proc_ResumeProcess(procPtr, FALSE);
	}
	if (procPtr->sigActions[sigNum] == SIG_SUSPEND_ACTION &&
		   procPtr->state == PROC_SUSPENDED) {
	    /*
	     * Are sending a suspend signal to a process that is already
	     * suspended.  In this case just notify the parent that the 
	     * process has been suspended.  This is necessary because resume
	     * signals are sent by processes to debugged processes which do not
	     * really get resumed.  However, the signaling process will not
	     * be informed that the process it sent the signal to did not get
	     * resumed (SIG_RESUME works regardless whether it actually 
	     * resumes anything or not).  Thus a process may believe that
	     * a process is running even though it really isn't and it may
	     * send a suspend signal to an already suspended process.
	     *
	     * There is a potential race here between a process getting
	     * suspended and us checking here but it doesn't matter.  If
	     * it gets suspended after we check then the parent will get 
	     * notified anyway.
	     */
	    Proc_InformParent(procPtr, PROC_SUSPEND_STATUS);
	} else if (sigNum != SIG_RESUME ||
		procPtr->sigActions[sigNum] != SIG_KILL_ACTION) {
	    sigBitMask = sigBitMasks[sigNum];
	    procPtr->sigPendingMask |= sigBitMask;
	    procPtr->sigCodes[sigNum] = code;
	    procPtr->sigAddr = (int)addr;
	    if (sigNum == SIG_SUSPEND) {
		/* 
		 * Set the "pending suspend" flag in case the process is 
		 * resumed before it actually suspends.  Clear the "resume" 
		 * flag in case the process is resumed and then suspended 
		 * again before the first suspend is processed.
		 */
		procPtr->genFlags |= PROC_PENDING_SUSPEND;
		procPtr->genFlags &= ~PROC_RESUME_PROCESS;
	    }
	    if (procPtr->sigHoldMask & sigBitMask & ~sigCanHoldMask) {
		/*
		 * We received a signal that was blocked but can't be blocked
		 * by users.  It only can be blocked if we are in the middle of
		 * executing a signal handler for the signal.  So we set things
		 * up to take the default action and make the signal unblocked
		 * so that we don't get an infinite loop of errors.
		 */
		procPtr->sigHoldMask &= ~sigBitMask;
		procPtr->sigActions[sigNum] = sigDefActions[sigNum];
	    }
	    procPtr->specialHandling = 1;
	    /*
	     * If the process is waiting then wake it up.
	     */
	    Sync_WakeWaitingProcess(procPtr);
	    if (sigNum == SIG_KILL || sigNum == SIG_MIGRATE_TRAP ||
		sigNum == SIG_MIGRATE_HOME) {
		if (sigNum == SIG_KILL && procPtr->state == PROC_NEW &&
		    (procPtr->genFlags & PROC_FOREIGN)) {
		    /*
		     * The process was only partially created.  We can't make
		     * it runnable so we have to reclaim it directly.
		     * Do this in the background so that
		     * Proc_DestroyMigratedProc has to wait for Sig_Send
		     * to unlock the process and we avoid a race condition.
		     */
		    Proc_CallFunc(Proc_DestroyMigratedProc,
				  (ClientData) procPtr->processID, 0);
		} else {
		    /*
		     * Resume the process so that we can perform the signal.
		     * If we're killing it, we tell Proc_ResumeProcess so it
		     * will even wake up a debugged process.
		     */
		    Proc_ResumeProcess(procPtr,
				       (sigNum == SIG_KILL) ? TRUE : FALSE);
		}
	    }
	}
    }
    UNLOCK_MONITOR;
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_SendProc --
 *
 *	Store the signal in the pending mask and store the code for the given
 *	process.  The code and addr are passed to the user interrupt
 *	handler.  The code indicates the cause of the signal.  The addr
 *	indicates the address of the fault.
 *
 *	NOTE: Assumes that we are called without the master lock down and
 *	with the process locked.
 *
 * Results:
 *	In the case of a local process, SUCCESS is returned.  If the process
 *	is migrated, error conditions such as RPC_TIMEOUT may be returned.
 *
 * Side effects:
 *	Signal pending mask and code modified.  If the process being signalled
 *	is migrated, an RPC is sent.  If the process is local, the sched_Mutex
 *	master lock is grabbed.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Sig_SendProc(procPtr, sigNum, code, addr)
    register	Proc_ControlBlock *procPtr;
    int				  sigNum;
    int				  code;
    Address			  addr;
{
    ReturnStatus status;

    /*
     * Make sure that the signal is in range.
     */
    if (sigNum < SIG_MIN_SIGNAL || sigNum >= SIG_NUM_SIGNALS) {
	if (sigNum == 0) {
	    return(SUCCESS);
	} else {
	    return(SIG_INVALID_SIGNAL);
	}
    }

    /*
     * Handle migrated processes specially. There's a race condition
     * when sending a signal to a migrated process, since it can
     * migrate back to this host while we're doing it.  Therefore,
     * if the problem was that the process didn't exist, check
     * to see if it has migrated back to this host (it's no longer MIGRATED).
     * We don't have to check for MIGRATING, since SigMigSend waits for
     * a migration in progress to complete.   Also make sure that while the
     * signal is sent and the process is unlocked, it processID doesn't change.
     */
    if (procPtr->state == PROC_MIGRATED ||
        (procPtr->genFlags & PROC_MIGRATING)) {
	Proc_PID processID;
	processID = procPtr->processID;
	status = SigMigSend(procPtr, sigNum, code, addr);
	if (processID != procPtr->processID) {
	    return(status);
	}
	if ((status != PROC_INVALID_PID) ||
	    (procPtr->state == PROC_MIGRATED)) {
	    return(status);
	}
    }
    if (procPtr->state == PROC_EXITING) {
	return(PROC_INVALID_PID);
    } else if (procPtr->state == PROC_NEW) {
	if (procPtr->genFlags & PROC_FOREIGN && proc_MigDebugLevel > 0) {
	    printf("Warning: got signal for process %x before migration complete.\n",
		   procPtr->processID);
	}
	return(PROC_INVALID_PID);
    } else {
	LocalSend(procPtr, sigNum, code, addr);
	return(SUCCESS);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_Send --
 *
 *	Send a signal to a process.  This entails marking the signal into
 *	the signal pending mask for the process and waking up the process
 *	if it is asleep.   
 *
 * Results:
 *	An error is the signal or the process id are invalid.  SUCCESS 
 *	otherwise.
 *
 * Side effects:
 *	The signal information in the proc table for the process that
 *	is being sent the signal may be modified.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus	
Sig_Send(sigNum, code, id, familyID, addr)
    int		sigNum;		/* The signal to send. */
    int		code;		/* The code that goes with the signal. */
    Proc_PID	id;		/* The id number of the process or process
				   family. */
    Boolean	familyID;	/* Whether the id is a process id or a process
				   group id. */
    Address	addr;		/* The address of the fault */
{
    register	Proc_ControlBlock	*procPtr;
    Proc_PCBLink			*procLinkPtr;
    ReturnStatus			status;
    List_Links				*familyList;
    int					userID;
    int					hostID;

    if (!Proc_ComparePIDs(id, PROC_MY_PID)) {
	hostID = Proc_GetHostID(id);
	if (hostID != rpc_SpriteID) {
	    /*
	     * Send a remote signal.
	     */
	    if (hostID == NET_BROADCAST_HOSTID ||
		hostID >  NET_NUM_SPRITE_HOSTS || hostID < 0) {
		return(PROC_INVALID_PID);
	    } else {
		return(SigSendRemoteSignal(hostID, sigNum, code, id,
					   familyID, addr));
	    }
	}
    }
    /*
     * Get the pointer to the control block if this is a valid process id.
     */
    if (!familyID) {
	if (Proc_ComparePIDs(id, PROC_MY_PID)) {
	    procPtr = Proc_GetEffectiveProc();
	    if (procPtr == (Proc_ControlBlock *) NIL) {
		panic("Sig_Send: procPtr == NIL\n");
	    }
	    Proc_Lock(procPtr);
	} else {
	    procPtr = Proc_LockPID(id);
	    if (procPtr == (Proc_ControlBlock *) NIL) {
		return(PROC_INVALID_PID);
	    }
	    if (!Proc_HasPermission(procPtr->effectiveUserID)) {
		Proc_Unlock(procPtr);
		return(PROC_UID_MISMATCH);
	    }
	}
	status = Sig_SendProc(procPtr, sigNum, code, addr);
	Proc_Unlock(procPtr);
    } else {
	Proc_PID *pidArray;
	int i;
	int numProcs;
	
	status = Proc_LockFamily((int)id, &familyList, &userID);
	if (status != SUCCESS) {
	    return(status);
	}
	if (!Proc_HasPermission(userID)) {
            Proc_UnlockFamily((int)id);
            return(PROC_UID_MISMATCH);
        }

	/*
	 * Send a signal to everyone in the given family.  We do this
	 * by grabbing a list of process IDs and then sending the signals
	 * with the family not locked, to avoid deadlocks resulting from
	 * signals being sent with the family locked.
	 */

	numProcs = 0;
	LIST_FORALL(familyList, (List_Links *) procLinkPtr) {
	    numProcs++;
	}
	pidArray = (Proc_PID *) malloc(numProcs * sizeof(Proc_PID));
	i = 0;
	LIST_FORALL(familyList, (List_Links *) procLinkPtr) {
	    procPtr = procLinkPtr->procPtr;
	    Proc_Lock(procPtr);
	    pidArray[i] = procPtr->processID;
	    Proc_Unlock(procPtr);
	    i++;
	    if (i > numProcs) {
		panic("Sig_Send: process family changed size while locked.\n");
		free((Address) pidArray);
		return(FAILURE);
	    }
	}
	Proc_UnlockFamily((int)id);
	for (i = 0; i < numProcs; i++) {
	    procPtr = Proc_LockPID(pidArray[i]);
	    if (procPtr == (Proc_ControlBlock *) NIL) {
		/*
		 * Race condition: process got removed.
		 */
		continue;
	    }
	    status = Sig_SendProc(procPtr, sigNum, code, addr);
	    Proc_Unlock(procPtr); 
	    if (status != SUCCESS) {
		break;
	    }
	}
	free((Address) pidArray);
    }

    return(status);
}

typedef struct {
	int		sigNum;
	int		code;
	Proc_PID	id;
	Boolean		familyID;
	int		effUid;
	Address		addr;
} SigParms;


/*
 *----------------------------------------------------------------------
 *
 * SigSendRemoteSignal --
 *
 *	Send a signal to a process on a remote machine.
 *
 * Results:
 *	Return the status from the remote machine.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus	
SigSendRemoteSignal(hostID, sigNum, code, id, familyID, addr)
    int		hostID;		/* Host to send message to. */
    int		sigNum;		/* Signal to send. */
    int		code;		/* Code to send. */
    Proc_PID	id;		/* ID to send it to. */
    Boolean	familyID;	/* TRUE if are sending to a process family. */
    Address	addr;		/* Address of signal. */
{
    SigParms		sigParms;
    Rpc_Storage		storage;
    Proc_ControlBlock	*procPtr;

    sigParms.sigNum = sigNum;
    sigParms.code = code;
    sigParms.id = id;
    sigParms.familyID = familyID;
    procPtr = Proc_GetEffectiveProc();
    sigParms.effUid = procPtr->effectiveUserID;
    sigParms.addr = addr;

    storage.requestParamPtr = (Address)&sigParms;
    storage.requestParamSize = sizeof(sigParms);
    storage.requestDataPtr = (Address)NIL;
    storage.requestDataSize = 0;
    storage.replyParamPtr = (Address)NIL;
    storage.replyParamSize = 0;
    storage.replyDataPtr = (Address)NIL;
    storage.replyDataSize = 0;

    return(Rpc_Call(hostID, RPC_SIG_SEND, &storage));

}


/*
 *----------------------------------------------------------------------
 *
 * Sig_RpcSend --
 *
 *	Stub to handle a remote signal RPC.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Reply is sent.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus	
Sig_RpcSend(srvToken, clientID, command, storagePtr)
    ClientData 		 srvToken;	/* Handle on server process passed to
				 	 * Rpc_Reply */
    int 		 clientID;	/* Sprite ID of client host */
    int 		 command;	/* Command identifier */
    register Rpc_Storage *storagePtr;	/* The request fields refer to the 
					 * request buffers and also indicate 
					 * the exact amount of data in the 
					 * request buffers.  The reply fields 
					 * are initialized to NIL for the
				 	 * pointers and 0 for the lengths.  
					 * This can be passed to Rpc_Reply */
{
    SigParms		*sigParmsPtr;
    ReturnStatus	status;
    Proc_ControlBlock	*procPtr;
    int			effUid;

    sigParmsPtr = (SigParms *) storagePtr->requestParamPtr;
    procPtr = Proc_GetCurrentProc();
    effUid = procPtr->effectiveUserID;
    procPtr->effectiveUserID = sigParmsPtr->effUid;
    status = Sig_Send(sigParmsPtr->sigNum, sigParmsPtr->code, sigParmsPtr->id,
		      sigParmsPtr->familyID, sigParmsPtr->addr);
    procPtr->effectiveUserID = effUid;
    Rpc_Reply(srvToken, status, storagePtr, (int(*)())NIL, (ClientData)NIL);
    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_SetHoldMask --
 *
 *	Set the signal hold mask for the current process.  Return the
 *	old mask.  No synchronization required since the only process
 *	that can modify the hold mask is this process.
 *
 * Results:
 *	Error if the place to store the old mask is invalid,  SUCCESS
 *	otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus	
Sig_SetHoldMask(newMask, oldMaskPtr)
    int	newMask;	/* Mask to set the hold mask to. */
    int	*oldMaskPtr;	/* Where to store the old mask. */
{
    register	Proc_ControlBlock	*procPtr;

    /*
     * Get out the old mask value and store the new one.
     */

    procPtr = Proc_GetActualProc();

    if (oldMaskPtr != USER_NIL) {
	if (Vm_CopyOut(sizeof(procPtr->sigHoldMask), 
		       (Address) &(procPtr->sigHoldMask),
		       (Address) oldMaskPtr) != SUCCESS) {
	    return(SYS_ARG_NOACCESS);
	}
    }

    procPtr->sigHoldMask = newMask & sigCanHoldMask;
    procPtr->specialHandling = 1;

    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_SetAction --
 *
 *	Set the action for a particular signal.
 *
 * Results:
 *	Error if the action, signal, or handler is invalid.
 *
 * Side effects:
 *	The sigAction and sigMasks fields may be modified for the
 *	particular signal.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus	
Sig_SetAction(sigNum, newActionPtr, oldActionPtr)
    int		sigNum;	       /* The signal for which the action is to be 
				  set. */
    Sig_Action	*newActionPtr; /* The actions to take for the signal. */
    Sig_Action	*oldActionPtr; /* The action that was taken for the signal. */
{
    Proc_ControlBlock	*procPtr;
    Address		dummy;
    Sig_Action		action;

    /*
     * Make sure that the signal is in range.
     */
    if (sigNum < SIG_MIN_SIGNAL || sigNum >= SIG_NUM_SIGNALS || 
	sigNum == SIG_KILL || sigNum == SIG_SUSPEND) {
	return(SIG_INVALID_SIGNAL);
    }

    procPtr = Proc_GetActualProc();

    /* 
     * Copy out the current action.  There are two cases:
     *
     *    1) The current action really contains a handler to call.  Thus
     *	     the current action is SIG_HANDLE_ACTION.
     *	  2) The current action is one of the other four actions.
     */

    if (oldActionPtr != (Sig_Action *) USER_NIL) {
	if (procPtr->sigActions[sigNum] > SIG_NUM_ACTIONS) {
	    action.action = SIG_HANDLE_ACTION;
	    action.handler = (int (*)())procPtr->sigActions[sigNum];
	    action.sigHoldMask = procPtr->sigMasks[sigNum];
	} else {
	    if (procPtr->sigActions[sigNum] == sigDefActions[sigNum]) {
		action.action = SIG_DEFAULT_ACTION;
	    } else {
		action.action = procPtr->sigActions[sigNum];
	    }
	}
	if (Vm_CopyOut(sizeof(action), (Address) &action, 
		(Address) oldActionPtr) != SUCCESS) {
	    return(SYS_ARG_NOACCESS);
	}
    }

    /*
     * Copy in the action to take.
     */

    if (Vm_CopyIn(sizeof(action), (Address) newActionPtr, 
		(Address) &action) != SUCCESS) {
	return(SYS_ARG_NOACCESS);
    }

    /*
     * Make sure that the action is valid.
     */

    if (action.action < 0 || action.action > SIG_NUM_ACTIONS) {
	return(SIG_INVALID_ACTION);
    }

    if (action.action == SIG_DEFAULT_ACTION) {
	action.action = sigDefActions[sigNum];
    }

    /*
     * Store the action.  If it is SIG_HANDLE_ACTION then the handler is stored
     * in place of the action.
     */

    if (action.action == SIG_HANDLE_ACTION) {
	if (Vm_CopyIn(4, (Address) ((unsigned int) (action.handler)), 
			(Address) &dummy) != SUCCESS) {
	    return(SYS_ARG_NOACCESS);
	}
	procPtr->sigMasks[sigNum] = 
		(sigBitMasks[sigNum] | action.sigHoldMask) & sigCanHoldMask;
	procPtr->sigActions[sigNum] = (unsigned int) action.handler;
    } else if (action.action == SIG_IGNORE_ACTION) {

	/*
	 * Only actions that can be blocked can be ignored.  This prevents a
	 * user from ignoring a signal such as a bus error which would cause
	 * the process to take a bus error repeatedly.
	 */

	if (sigBitMasks[sigNum] & sigCanHoldMask) {
	    procPtr->sigActions[sigNum] = SIG_IGNORE_ACTION;
	    Proc_Lock(procPtr);
	    SigClearPendingMask(procPtr, sigNum);
	    if (sigNum == SIG_SUSPEND) {
		procPtr->genFlags &= ~(PROC_PENDING_SUSPEND |
				       PROC_RESUME_PROCESS);
	    }
	    Proc_Unlock(procPtr);
	} else {
	    return(SIG_INVALID_SIGNAL);
	}
	procPtr->sigMasks[sigNum] = 0;
    } else {
	procPtr->sigActions[sigNum] = action.action;
	procPtr->sigMasks[sigNum] = 0;
    }

    return(SUCCESS);
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_Pause --
 *
 *	Atomically change signal hold mask and wait for a signal to arrive.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ENTRY ReturnStatus	
Sig_Pause(sigHoldMask)
    int	sigHoldMask;	/* The value that the mask of held signals is to be set
			   to while waiting for a signal to arrive. */
{
    register	Proc_ControlBlock	*procPtr;
    ReturnStatus status;
    int migMask;

    LOCK_MONITOR;

    procPtr = Proc_GetActualProc();

    /*
     * The signal mask cannot be restored until the signal handler has
     * had a chance to be called for the signal that caused Sig_Pause
     * to return.  To allow this the current hold mask is stored in the
     * proc table and the flag sigPause is set to be true to indicate that
     * the hold mask has to be restored after the signal handler has had a
     * chance to be called.
     */

    procPtr->oldSigHoldMask = procPtr->sigHoldMask;
    procPtr->sigFlags |= SIG_PAUSE_IN_PROGRESS;
    procPtr->sigHoldMask = sigHoldMask & sigCanHoldMask;
    procPtr->specialHandling = 1;

    /*
     * Wait on the signal condition.  As it turns out since a signal
     * wakes up the process regardless what it is sleeping on, this condition
     * variable is never broadcasted on, but we have to wait on something in 
     * order to release the monitor lock.
     *
     * Don't let a Sig_Pause be interrupted by a migrate trap signal.
     * So, if none of the signal bits are set besides migration-related
     * signals, and a migration-related signal bit is set, let the user-level
     * code retry  the signal.
     */
    (void) Sync_Wait(&signalCondition, TRUE);

    migMask = (Sig_NumberToMask(SIG_MIGRATE_TRAP)) |
	(Sig_NumberToMask(SIG_MIGRATE_HOME));
    if ((! (procPtr->sigPendingMask & ~migMask)) &&
	(procPtr->sigPendingMask & migMask)) {
	status = GEN_ABORTED_BY_SIGNAL;
    } else {
	status = SUCCESS;
    }
    
    UNLOCK_MONITOR;

    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * SigClearPendingMask --
 *
 *	Remove the given signal from the pending mask.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Pending mask for process modified.
 *
 *----------------------------------------------------------------------
 */

ENTRY void
SigClearPendingMask(procPtr, sigNum)
    register	Proc_ControlBlock	*procPtr;
    int					sigNum;
{
    LOCK_MONITOR;

    procPtr->sigPendingMask &= ~sigBitMasks[sigNum];

    UNLOCK_MONITOR;
}

/*
 *---------------------------------------------------------------------------
 *
 * Routines for signal handlers --
 *
 * A signal handler is called right before a process is to return to 
 * user space.  In order to do this the current state before the signal
 * is taken must be saved, the signal handler called, and then the state
 * restored when the signal handler returns.  It is this modules responsibility
 * to handle the signal state;  all of the actual saving and restoring of
 * machine state and the calling of the handler is done in the machine
 * dependent routines in the mach module.
 */


/*
 *----------------------------------------------------------------------
 *
 * Sig_Handle --
 *
 *	Set things up so that the signal handler is called for one of the
 *	signals that are pending for the current process.  This is done
 *	by saving the old trap stack and modifying the current one.
 *
 * Results:
 *	Return TRUE if a signal is setup to be handled by the user.
 *
 * Side effects:
 *	*trapStackPtr is modified and also the user stack is modified.
 *
 *----------------------------------------------------------------------
 */
Boolean		
Sig_Handle(procPtr, sigStackPtr, pcPtr)
    register	Proc_ControlBlock	*procPtr;
    register	Sig_Stack		*sigStackPtr;
    Address				*pcPtr;
{
    int					sigs;
    int					sigNum;
    unsigned	int			*bitMaskPtr;
    int					sigBitMask;

    /*
     * Find out which signals are pending.
     */
    sigs = procPtr->sigPendingMask & ~procPtr->sigHoldMask;
    if (sigs == 0) {
	return(FALSE);
    }

    /*
     * Check for the signal SIG_KILL.  This is processed specially because
     * it is how processes that have some problem such as being unable
     * to write to swap space on the file server are destroyed.
     */
    if (sigs & sigBitMasks[SIG_KILL]) {
	if (procPtr->sigCodes[SIG_KILL] != SIG_NO_CODE) {
	    Proc_ExitInt(PROC_TERM_DESTROYED, 
			procPtr->sigCodes[SIG_KILL], 0);
	} else {
	    Proc_ExitInt(PROC_TERM_SIGNALED, SIG_KILL, 0);
	}
    }

    for (sigNum = SIG_MIN_SIGNAL, bitMaskPtr = &sigBitMasks[SIG_MIN_SIGNAL];
	 !(sigs & *bitMaskPtr);
	 sigNum++, bitMaskPtr++) {
    }

    SigClearPendingMask(procPtr, sigNum);

    /*
     * Process the signal.
     */
    switch (procPtr->sigActions[sigNum]) {
	case SIG_IGNORE_ACTION:
	    printf("Warning: %s\n",
	    "Sig_Handle:  An ignored signal was in a signal pending mask.");
	    return(FALSE);

	case SIG_KILL_ACTION:
	    if (sigNum == SIG_KILL || !(procPtr->genFlags & PROC_DEBUGGED)) {
		Proc_ExitInt(PROC_TERM_SIGNALED, sigNum,
			procPtr->sigCodes[sigNum]);
		panic("Sig_Handle: Proc_Exit returned!\n");
	    } else {
		/* Fall through */
	    }

	case SIG_SUSPEND_ACTION:
	case SIG_DEBUG_ACTION:
	    /* 
	     * A suspended process and a debugged process are basically
	     * the same.  A suspended process can be debugged just like
	     * a process in the debug state.   The only difference is that
	     * a suspended process does not go onto the debug list; it can
	     * only be debugged by a debugger that specifically asks for
	     * it.
	     *
	     * Suspend the process.
	     */
	    Proc_SuspendProcess(procPtr,
			procPtr->sigActions[sigNum] == SIG_DEBUG_ACTION,
			PROC_TERM_SIGNALED, sigNum, 
			procPtr->sigCodes[sigNum]);
	    return(FALSE);

	case SIG_MIGRATE_ACTION:
	    /*
	     * If the process was in the middle of a page fault,
	     * its PC in the trap stack is not useable.
	     * Reset the pending condition but hold it until we get out of
	     * the kernel.
	     */
	    if (!Mach_CanMigrate(procPtr)) {
		LocalSend(procPtr, sigNum, procPtr->sigCodes[sigNum],
		    (Address)procPtr->sigAddr);
		procPtr->sigHoldMask |= Sig_NumberToMask(SIG_MIGRATE_TRAP);
		return(FALSE);
	    }

	    /*
	     * Double-check against process not allowed to migrate.  This
	     * can happen if a process migrates, opens a pdev as master,
	     * and gets signalled to migrate home.
	     */
	    if (procPtr->genFlags & PROC_DONT_MIGRATE) {
		if (proc_MigDebugLevel > 0) {
		    printf("Proc_Migrate: process %x is not allowed to migrate.\n",
			       procPtr->processID);
		}
		return(FALSE);
	    }
	    if (procPtr->peerHostID != NIL) {
		if (proc_MigDebugLevel > 6) {
		    printf("Sig_Handle calling Proc_MigrateTrap for process %x.\n",
			       procPtr->processID);
		}
		Proc_MigrateTrap(procPtr);
	    }
	    return(FALSE);

	case SIG_DEFAULT_ACTION:
	    panic("Sig_Handle: SIG_DEFAULT_ACTION found in array of actions?\n");
    }

    /*
     * Set up our part of the signal stack.
     */
    sigStackPtr->sigNum = sigNum;
    sigStackPtr->sigCode = procPtr->sigCodes[sigNum];
    sigStackPtr->sigAddr = procPtr->sigAddr;
    /*
     * If this signal handler is being called after a call to Sig_Pause then
     * the real signal hold mask has to be restored after the handler returns.
     * This is assured by pushing the real hold mask which is stored in 
     * the proc table onto the stack.
     */
    if (procPtr->sigFlags & SIG_PAUSE_IN_PROGRESS) {
	procPtr->sigFlags &= ~SIG_PAUSE_IN_PROGRESS;
	sigStackPtr->contextPtr->oldHoldMask = procPtr->oldSigHoldMask;
    } else {
	sigStackPtr->contextPtr->oldHoldMask = procPtr->sigHoldMask;
    }

    procPtr->sigHoldMask |= procPtr->sigMasks[sigNum];
    sigBitMask = sigBitMasks[sigNum];
    if (sigBitMask & ~sigCanHoldMask) {
	/*
	 * If this is a non-blockable signal then add it to the hold mask
	 * so that if we get it again we know that it can't be handled.
	 */
	procPtr->sigHoldMask |= sigBitMask;
    }
    procPtr->specialHandling = 1;
    *pcPtr = (Address)procPtr->sigActions[sigNum];
    return(TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_Return --
 *
 *	Process a return from signal.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The trap stack is modified.
 *
 *----------------------------------------------------------------------
 */
void		
Sig_Return(procPtr, sigStackPtr)
    register Proc_ControlBlock	*procPtr;	/* Process that is returning
						 * from a signal. */
    Sig_Stack			*sigStackPtr;	/* Signal stack. */
{
    procPtr->sigHoldMask = sigStackPtr->contextPtr->oldHoldMask;
    procPtr->specialHandling = 1;
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_AllowMigration --
 *
 *	Set up a process to allow migration.  This is a special call
 *	because normally the SIG_MIGRATE_TRAP signal is not holdable in
 *	the first place.
 *
 *	This could be a macro and be called directly from
 *	Mach_StartUserProc, once things are stable....
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The process's hold mask is modified.
 *
 *----------------------------------------------------------------------
 */
void		
Sig_AllowMigration(procPtr)
    register Proc_ControlBlock	*procPtr;	/* process to modify */
{
    if (procPtr->sigHoldMask &&
	(procPtr->sigHoldMask & sigBitMasks[SIG_MIGRATE_TRAP])) {
    	procPtr->sigHoldMask &= ~sigBitMasks[SIG_MIGRATE_TRAP];
	procPtr->specialHandling = 1;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Sig_CheckForKill --
 *
 *	Check if a process has a kill signal and kill it if so.
 *	Otherwise return.  this is for calling in difficult places where
 *	we can't allow any signals that would be handled in user mode to
 *	occur.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Process may be killed.
 *
 *----------------------------------------------------------------------
 */
void		
Sig_CheckForKill(procPtr)
    Proc_ControlBlock	*procPtr;
{
    int					sigs;

    /*
     * Find out which signals are pending.
     */
    sigs = procPtr->sigPendingMask & ~procPtr->sigHoldMask;
    if (sigs == 0) {
	return;
    }

    /*
     * Check for the signal SIG_KILL.  This is processed specially because
     * it is how processes that have some problem such as being unable
     * to write to swap space on the file server are destroyed.
     */
    if (sigs & sigBitMasks[SIG_KILL]) {
	if (procPtr->sigCodes[SIG_KILL] != SIG_NO_CODE) {
	    Proc_ExitInt(PROC_TERM_DESTROYED, 
			procPtr->sigCodes[SIG_KILL], 0);
	} else {
	    Proc_ExitInt(PROC_TERM_SIGNALED, SIG_KILL, 0);
	}
    }
    return;
}
