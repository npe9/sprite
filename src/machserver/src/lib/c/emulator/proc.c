/* 
 * proc.c --
 *
 *	Miscellaneous run-time library routines for the Proc module.
 *
 * Copyright 1986 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header: /user5/kupfer/spriteserver/src/lib/c/emulator/RCS/proc.c,v 1.7 92/06/10 15:21:08 kupfer Exp $ SPRITE (Berkeley)";
#endif not lint

#include <sprite.h>
#include <mach.h>
#include <mach/message.h>
#include <mach_error.h>
#include <status.h>
#include <proc.h>
#include <stdio.h>
#include <spriteEmuInt.h>
#include <spriteSrv.h>
#include <stdlib.h>
#include <string.h>
#include <test.h>

/* forward references... */

static void GatherStrings();
static vm_address_t Realloc();


#if 0
/*
 *----------------------------------------------------------------------
 *
 * Proc_Exec --
 *
 *	Maps Proc_Exec calls into Proc_ExecEnv calls.  This routine
 *	should not return unless the process cannot be exec'ed.
 *
 *
 * Results:
 *	Error status from Proc_ExecEnv, if any.
 *
 * Side effects:
 *	Refer to Proc_ExecEnv kernel call & man page.
 *
 *----------------------------------------------------------------------
 */

int
Proc_Exec(fileName, argPtrArray, debugMe)
    char *fileName;
    char **argPtrArray;
    Boolean debugMe;
{
    int status;
    extern char **environ;
    extern char **Proc_FetchGlobalEnv();	/* temporary!! */

    /*
     * Install the system-wide environment if ours is non-existent.
     */
    if (environ == (char **) NULL) {
	environ = Proc_FetchGlobalEnv();
    }
    status = Proc_ExecEnv(fileName, argPtrArray, environ, debugMe);
    return(status);
}
#endif /* 0 */


/*
 *----------------------------------------------------------------------
 *
 * Proc_ExecEnv --
 *
 *	Exec a new program.  This routine should not return unless the
 *	process cannot be exec'ed.
 *
 * Results:
 *	Sprite status code, if there was an error.
 *
 * Side effects:
 *	Refer to Proc_ExecEnv kernel call & man page.
 *
 *----------------------------------------------------------------------
 */

int
Proc_ExecEnv(fileName, argPtrArray, envPtrArray, debugMe)
    char *fileName;		/* name of file to exec */
    char **argPtrArray;		/* array of arguments */
    char **envPtrArray;		/* array of environment variables */
    Boolean debugMe;
{
    int status;
    kern_return_t kernStatus;
    Boolean sigPending;
    vm_offset_t argStrings;	/* start of argument strings */
    mach_msg_type_number_t argStringsSize; /* bytes in argStrings */
    vm_offset_t envStrings;	/* start of environment strings */
    mach_msg_type_number_t  envStringsSize; /* bytes in envStrings */
    vm_offset_t argOffsets;	/* start of argument string offsets */
    mach_msg_type_number_t numArgs; /* number of argument strings */
    vm_offset_t envOffsets;	/* start of environment string offsets */
    mach_msg_type_number_t numEnvs; /* number of environment strings */
    vm_address_t pageBuffer = NULL; /* buffer to hold the strings and 
				     * offsets */ 
    vm_offset_t pageBufferFree = 0; /* first free location in the buffer */
    
    /* 
     * Gather up the argument and environment strings and put them in 
     * pageBuffer.  It would be nice if we could just malloc 4 arrays (2 
     * for strings and 2 for offsets), but some programs (e.g., sh) do 
     * their own memory management, and using their malloc can lead to 
     * random memory smashes.  Grumble.
     */
    GatherStrings(argPtrArray, &pageBuffer, &pageBufferFree, &argStrings,
		  &argStringsSize, &argOffsets, &numArgs);
    GatherStrings(envPtrArray, &pageBuffer, &pageBufferFree, &envStrings,
		  &envStringsSize, &envOffsets, &numEnvs);

    kernStatus = Proc_ExecEnvStub(SpriteEmu_ServerPort(), fileName,
				  (mach_msg_type_number_t)strlen(fileName)+1,
				  (Proc_OffsetTable)(pageBuffer + argOffsets),
				  numArgs,
				  (Proc_Strings)(pageBuffer + argStrings),
				  argStringsSize,
				  (Proc_OffsetTable)(pageBuffer + envOffsets),
				  numEnvs,
				  (Proc_Strings)(pageBuffer + envStrings),
				  envStringsSize,
				  debugMe, &status, &sigPending);

    /* If we get here something went wrong. */

    if (pageBuffer != NULL) {
	(void)vm_deallocate(mach_task_self(), pageBuffer,
			    round_page(pageBufferFree));
	pageBuffer = NULL;
    }

    if (kernStatus != KERN_SUCCESS) {
	status = Utils_MapMachStatus(kernStatus);
    }
    if (sigPending) {
	SpriteEmu_TakeSignals();
    }
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_Wait --
 *
 *	The "normal" interface for waiting on child processes.
 *	This procedure simply invokes the Proc_RawWait system call
 *	and retries the call if the Proc_RawWait call aborted because
 *	of a signal.  See the man page for details on what the kernel
 *	call does.
 *
 * Results:
 *	A standard Sprite return status.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Proc_Wait(numPids, pidArray, block, procIdPtr, reasonPtr, statusPtr,
	subStatusPtr, usagePtr)
    int numPids;		/* Number of entries in pidArray below.
				 * 0 means wait for ANY child. */
    Proc_PID pidArray[];	/* Array of pids to wait for. */
    Boolean block;		/* TRUE means block;  FALSE means return
				 * immediately if no children are dead. */
    Proc_PID *procIdPtr;	/* Return ID of dead/stopped process here,
				 * if non-NULL. */
    int *reasonPtr;		/* Return cause of death/stoppage here, if
				 * non-NULL. */
    int *statusPtr;		/* If process exited normally, return exit
				 * status here (if non-NULL).  Otherwise
				 * return signal # here. */
    int *subStatusPtr;		/* Return additional signal status here,
				 * if non-NULL. */
    Proc_ResUsage *usagePtr;	/* Return resource usage info here,
				 * if non-NULL. */
{
    ReturnStatus status;
    kern_return_t kernStatus;
    Proc_PID dummyPid;		/* to keep MIG happy */
    int dummyReason;		/* ditto */
    int dummyStatus;		/* ditto */
    int dummySubStatus;		/* ditto */
    Boolean sigPending;

    if (procIdPtr == NULL) {
	procIdPtr = &dummyPid;
    }
    if (reasonPtr == NULL) {
	reasonPtr = &dummyReason;
    }
    if (statusPtr == NULL) {
	statusPtr = &dummyStatus;
    }
    if (subStatusPtr == NULL) {
	subStatusPtr = &dummySubStatus;
    }

    do {
	kernStatus = Proc_WaitStub(SpriteEmu_ServerPort(), numPids,
				   (vm_address_t)pidArray, block, procIdPtr,
				   reasonPtr, statusPtr, subStatusPtr,
				   (vm_address_t)usagePtr, &status,
				   &sigPending);
	if (kernStatus != KERN_SUCCESS) {
	    status = Utils_MapMachStatus(kernStatus);
	}
	if (sigPending) {
	    SpriteEmu_TakeSignals();
	}
    } while (status == GEN_ABORTED_BY_SIGNAL);
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_Migrate --
 *
 *	The "normal" interface for invoking process migration.  This
 *	performs extra checks against the process being migrated when
 *	it is already migrated to a different machine.  
 *
 * Results:
 *	A standard Sprite return status.
 *
 * Side effects:
 *	The process is migrated home if it is not already home, then
 *	it is migrated to the node specified.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Proc_Migrate(pid, nodeID)
    Proc_PID pid;
    int	     nodeID;
{
#ifndef SPRITED_MIGRATION
#ifdef lint
    pid++;
    nodeID++;
#endif

    return GEN_NOT_IMPLEMENTED;
#else /* SPRITED_MIGRATION */
    ReturnStatus status;
    int virtualHost;
    int physicalHost;

    status = Proc_GetHostIDs(&virtualHost, &physicalHost);
    if (status != SUCCESS) {
	return(status);
    }
    if (pid == PROC_MY_PID) {
	if (nodeID != physicalHost && nodeID != virtualHost) {
	    status = Sig_Send(SIG_MIGRATE_HOME, PROC_MY_PID, FALSE);
	    if (status != SUCCESS) {
		return(status);
	    }
	}
    } else {
	int i;
	Proc_PCBInfo info;
	/*
	 * Try to avoid the race condition for migrating other processes
	 * home.  This can be removed once the kernel does remote-to-remote
	 * migration directly.
	 */
#define WAIT_MAX_TIMES 10
#define WAIT_INTERVAL 1
	(void) Sig_Send(SIG_MIGRATE_HOME, pid, TRUE);
	for (i = 0; i < WAIT_MAX_TIMES; i++) {
	    status = Proc_GetPCBInfo(Proc_PIDToIndex(pid),
				     Proc_PIDToIndex(pid), PROC_MY_HOSTID,
				     sizeof(info),
				     &info, (char *) NULL , (int *) NULL);
	    if (status != SUCCESS) {
		return(status);
	    }
	    if (info.state != PROC_MIGRATED) {
		break;
	    }
	    (void) sleep(WAIT_INTERVAL);
	}
	if (i == WAIT_MAX_TIMES) {
	    fprintf(stderr, "Unable to migrate process %x because it wouldn't migrate home.\n", pid);
	    return(FAILURE);
	}
    }
    status = Proc_RawMigrate(pid, nodeID);
    return(status);
#endif /* SPRITED_MIGRATION */
}


/*
 *----------------------------------------------------------------------
 *
 * Proc_RemoteExec --
 *
 *	The "normal" interface for invoking remote exec.  This
 *	performs extra checks against the process being migrated when
 *	it is already migrated to a different machine.  
 *
 * Results:
 *	This routine does not return if it succeeds.
 *	A standard Sprite return status is returned upon failure.
 *
 * Side effects:
 *	The process is migrated home if it is not already home, then
 *	a remote exec is performed.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Proc_RemoteExec(fileName, argPtrArray, envPtrArray, host)
    char	*fileName;	/* The name of the file to exec. */
    char	**argPtrArray;	/* The array of arguments to the exec'd 
				 * program. */
    char	**envPtrArray;	/* The array of environment variables for
				 * the exec'd program. */
    int		host;		/* ID of host on which to exec. */
{
    ReturnStatus status;
    int virtualHost;
    int physicalHost;

    status = Proc_GetHostIDs(&virtualHost, &physicalHost);
    if (status != SUCCESS) {
	return(status);
    }
    /*
     * Save a double migration if the exec is local.
     */
    if (physicalHost != host) {
	if (virtualHost != host) {
	    status = Sig_Send(SIG_MIGRATE_HOME, PROC_MY_PID, FALSE);
	    if (status != SUCCESS) {
		return(status);
	    }
	}
#ifdef SPRITED_MIGRATION
	status = Proc_RawRemoteExec(fileName, argPtrArray, envPtrArray, host);
#else
	status = GEN_NOT_IMPLEMENTED;
#endif
    } else {
	status = Proc_ExecEnv(fileName, argPtrArray, envPtrArray, FALSE);
    }
    return(status);
}


/*
 *----------------------------------------------------------------------
 *
 * GatherStrings --
 *
 *	Put all the strings in an array into a contiguous buffer.
 *	
 *	Logically, we are creating 2 new arrays, using the page buffer for
 *	storage.  One array contains the strings, one after another.  The
 *	other array contains offsets into the first array, telling where
 *	each string starts at.  Because we might be called a couple times,
 *	the page buffer might grow and move, so we refer to these arrays
 *	using their offset from the start of the page buffer, rather than
 *	using an absolute address.
 *
 * Results:
 * 	Fills in the current address of the page buffer and the first
 * 	unused location in it.  Also fills in information about where the
 * 	strings and string offsets are.  The caller is responsible for
 * 	eventually freeing the page buffer.
 * 	Note: we guarantee that there are no completely empty pages at the 
 * 	end of the buffer.  (This is so that we can infer the buffer length 
 * 	from the first free position.)  Thus if the buffer is completely 
 * 	full, the free position actually points to unallocated memory.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
GatherStrings(stringsArray, pageBufferPtr, pageBufferFreePtr, stringTablePtr,
	      stringTableBytesPtr, offsetsArrayPtr, numStringsPtr)
    char *stringsArray[];	/* the strings to gather up */
    vm_address_t *pageBufferPtr; /* IN: buffer to put the strings and offsets;
				  * OUT: current location of the buffer */
    vm_offset_t *pageBufferFreePtr; /* IN/OUT: first free location in the
				     * page buffer */
    vm_offset_t *stringTablePtr; /* OUT: where the strings start; from the 
				  * start of the page buffer  */
    mach_msg_type_number_t *stringTableBytesPtr; 
				/* OUT: bytes in the string table */
    vm_offset_t *offsetsArrayPtr; /* OUT: where the start-of-string offsets 
				   * start; from the start of the page buf */
    mach_msg_type_number_t *numStringsPtr; /* OUT: number of strings found */
{
    vm_address_t buffer;	/* the current buffer */
    vm_size_t bufferBytes;	/* bytes in the current buffer */
    vm_offset_t currOffset;	/* next offset into buffer */
    vm_offset_t offsetsArray;	/* start of offsets array; this value is 
				 * relative to the start of the buffer, but
				 * the offsets in the array are relative to
				 * the start of the strings region */
    int numStrings;		/* number of strings actually found */
    vm_offset_t stringTable;	/* start of the actual strings; relative to 
				 * start of buffer */
    int stringLength;		/* length of one string */
    int whichString;		/* index into array */

    buffer = *pageBufferPtr;
    currOffset = *pageBufferFreePtr;
    bufferBytes = round_page(currOffset);

    if (buffer == NULL) {
	buffer = Realloc(buffer, 0, vm_page_size);
	bufferBytes = vm_page_size;
    }

    /* 
     * Count up the number of strings.  We'll put the offsets first in the 
     * buffer, followed by the actual strings.  Make sure that the offsets 
     * array is properly aligned.
     */
    
    for (numStrings = 0; stringsArray[numStrings] != 0; numStrings++) {
    }
    currOffset = (currOffset + sizeof(int) - 1) & ~(sizeof(int) - 1);
    offsetsArray = currOffset;
    stringTable = currOffset + numStrings * sizeof(vm_offset_t);
    currOffset = stringTable;
    
    /* 
     *
     * For each string, copy it into the buffer, and register its offset in 
     * the offsets array.  If we're going to run off the end of the 
     * currently allocated region, make it bigger first.  (This isn't
     * actually expected to happen much.)  Note that there shouldn't be
     * enough strings to make it worth squishing out the terminal null for
     * each string.  (And leaving the nulls in will theoretically make
     * debugging easier.)
     */
    
    for (whichString = 0; whichString < numStrings; whichString++) {
	vm_offset_t *offsetsPtr;

	stringLength = strlen(stringsArray[whichString]) + 1;
	if (currOffset + stringLength > bufferBytes) {
	    buffer = Realloc(buffer, bufferBytes,
			     round_page(currOffset + stringLength));
	    bufferBytes = round_page(currOffset + stringLength);
	}
	offsetsPtr = (vm_offset_t *)(buffer + offsetsArray);
	offsetsPtr[whichString] = currOffset - stringTable;
	bcopy(stringsArray[whichString], buffer+currOffset, stringLength);
	currOffset += stringLength;
    }

    *pageBufferPtr = buffer;
    *pageBufferFreePtr = currOffset;
    *stringTablePtr = stringTable;
    *stringTableBytesPtr = currOffset - stringTable;
    *offsetsArrayPtr = offsetsArray;
    *numStringsPtr = numStrings;
}


/*
 *----------------------------------------------------------------------
 *
 * Realloc --
 *
 *	Grow a region of allocated memory.
 *
 * Results:
 *	Returns a pointer to the new region, which has a copy of the old 
 *	one.
 *
 * Side effects:
 *	The old region is deallocated.
 *
 *----------------------------------------------------------------------
 */

static vm_address_t
Realloc(oldRegion, oldSize, newSize)
    vm_address_t oldRegion;	/* the region to be grown */
    vm_size_t oldSize;		/* the current size */
    vm_size_t newSize;		/* the desired size */
{
    kern_return_t kernStatus;
    vm_address_t newRegion = 0;

    kernStatus = vm_allocate(mach_task_self(), &newRegion, newSize, TRUE);
    if (kernStatus != KERN_SUCCESS) {
	Test_PutMessage("exec: can't allocate buffer for strings: ");
	Test_PutMessage(mach_error_string(kernStatus));
	Test_PutMessage("\n");
	abort();
    }

    bcopy(oldRegion, newRegion, oldSize);
    (void)vm_deallocate(mach_task_self(), oldRegion, oldSize);

    return newRegion;
}
