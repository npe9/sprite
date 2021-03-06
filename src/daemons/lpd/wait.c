/* 
 * wait.c --
 *
 *	Procedure to map from Unix wait system call to Sprite.
 *
 * Copyright (C) 1986 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header: wait.c,v 1.5 88/07/29 18:54:36 ouster Exp $ SPRITE (Berkeley)";
#endif not lint

#include <sprite.h>
#include <proc.h>
#include <spriteTime.h>

#include "compatInt.h"

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>

char *XXXX;


/*
 *----------------------------------------------------------------------
 *
 * wait --
 *
 *	Procedure to map from Unix wait system call to Sprite Proc_Wait.
 *
 * Results:
 *	If wait returns due to a stopped or terminated child process,
 *	the process ID of the child is returned to the calling process.
 *	In addition, if statusPtr is non-null then fields in *statusPtr
 *	will be set to contain the exit status of the child whose process
 *	ID is returned.
 *
 *	Otherwise, UNIX_ERROR is returned and errno is set to indicate
 *	the error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
wait(statusPtr)
union wait *statusPtr;
{
    ReturnStatus status;	/* result returned by Proc_Wait */
    int pid;			/* process ID of child */
    int reason;			/* reason child exited */
    int childStatus;		/* returnStatus of child */
    int subStatus;		/* additional signal status */
    int	unixSignal;

    status = Proc_Wait(0, (int *) NULL, PROC_WAIT_BLOCK, &pid, &reason,
		    &childStatus, &subStatus, (Proc_ResUsage *) NULL);
    if (status != SUCCESS) {
	errno = Compat_MapCode(status);
	return(UNIX_ERROR);
    } else {
	if (statusPtr != NULL)  {
	    statusPtr->w_status = 0;
	    if (reason == PROC_TERM_SUSPENDED) {
		(void)Compat_SpriteSignalToUnix(childStatus, &unixSignal);
		statusPtr->w_stopval = WSTOPPED;
		statusPtr->w_stopsig = unixSignal;
	    } else if (reason == PROC_TERM_SIGNALED ||
		       reason == PROC_TERM_RESUMED) {
		(void)Compat_SpriteSignalToUnix(childStatus, &unixSignal);
		statusPtr->w_termsig = unixSignal;
		/* NEED TO HANDLE coredump FIELD */
	    } else {
		statusPtr->w_retcode = childStatus;
	    }
	}
	return((int) pid);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * wait3 --
 *
 *	Procedure to map from Unix wait3 system call to Sprite Proc_Wait.
 *
 * Results:
 *	If wait returns due to a stopped or terminated child process,
 *	the process ID of the child is returned to the calling process.
 *	In addition, if statusPtr is non-null then fields in *statusPtr
 *	will be set to contain the exit status of the child whose process
 *	ID is returned.
 *
 *	Otherwise, UNIX_ERROR is returned and errno is set to indicate
 *	the error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

wait3(statusPtr, options, unixRusagePtr)
    union	wait	*statusPtr;
    int			options;
    struct	rusage	*unixRusagePtr;
{
    Proc_ResUsage spriteRusage;
    ReturnStatus status;	/* result returned by Proc_Wait */
    int pid;			/* process ID of child */
    int reason;			/* reason child exited */
    int childStatus;		/* returnStatus of child */
    int subStatus;		/* additional signal status */
    int	flags;

foobar:
    flags = 0;
    if (!(options & WNOHANG)) {
	flags |= PROC_WAIT_BLOCK;
    }
    if (options & WUNTRACED) {
	flags |= PROC_WAIT_FOR_SUSPEND;
    }

    status = Proc_Wait(0, (int *) NULL, flags, &pid, &reason, 
		&childStatus, &subStatus, &spriteRusage);
    if (status != SUCCESS) {
	errno = Compat_MapCode(status);
	return(UNIX_ERROR);
    } else {
	if (statusPtr != NULL)  {
	    int	unixSignal;
	    statusPtr->w_status = 0;
	    switch (reason) {
	    case PROC_TERM_SUSPENDED: XXXX = "PROC_TERM_SUSPENDED"; break;
	    case PROC_TERM_SIGNALED: XXXX = "PROC_TERM_SIGNALED"; break;
	    case PROC_TERM_RESUMED: XXXX = "PROC_TERM_RESUMED"; break;
	    default: XXXX = "????"; break;
	    }
	    if (reason == PROC_TERM_SUSPENDED) {
		(void)Compat_SpriteSignalToUnix(childStatus, &unixSignal);
		statusPtr->w_stopval = WSTOPPED;
		statusPtr->w_stopsig = unixSignal;
	    } else if (reason == PROC_TERM_RESUMED) {
		goto foobar;
	    } else if (reason == PROC_TERM_SIGNALED ||
		       reason == PROC_TERM_RESUMED) {
		(void)Compat_SpriteSignalToUnix(childStatus, &unixSignal);
		statusPtr->w_termsig = unixSignal;
		/* NEED TO HANDLE coredump FIELD */
	    } else {
		statusPtr->w_retcode = childStatus;
	    }
	}
	if (unixRusagePtr != NULL) {
	    /*
	     * Return the total time used by the process and all its children.
	     */
	    Time totalKTime;
	    Time totalUTime;

	    bzero((char *) unixRusagePtr, sizeof(*unixRusagePtr));
	    Time_Add(spriteRusage.userCpuUsage, spriteRusage.childUserCpuUsage,
						&totalUTime);
	    Time_Add(spriteRusage.kernelCpuUsage,
		     spriteRusage.childKernelCpuUsage, &totalKTime);
	    unixRusagePtr->ru_utime.tv_sec = totalUTime.seconds;
	    unixRusagePtr->ru_utime.tv_usec = totalUTime.microseconds;
	    unixRusagePtr->ru_stime.tv_sec = totalKTime.seconds;
	    unixRusagePtr->ru_stime.tv_usec = totalKTime.microseconds;
	    unixRusagePtr->ru_nvcsw = spriteRusage.numWaitEvents;
	    unixRusagePtr->ru_nivcsw = spriteRusage.numQuantumEnds;
	}
	return((int) pid);
    }
}
