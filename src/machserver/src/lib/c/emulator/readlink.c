/* 
 * readlink.c --
 *
 *	UNIX readlink() for the Sprite server.
 *
 * Copyright 1986, 1992 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that this copyright
 * notice appears in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header: /user5/kupfer/spriteserver/src/lib/c/emulator/RCS/readlink.c,v 1.1 92/03/13 20:41:30 kupfer Exp $ SPRITE (Berkeley)";
#endif /* not lint */

#include <mach.h>
#include <mach/message.h>
#include <sprite.h>
#include <compatInt.h>
#include <spriteEmuInt.h>
#include <spriteSrv.h>
#include <string.h>


/*
 *----------------------------------------------------------------------
 *
 * readlink --
 *
 *	Procedure to map from Unix readlink system call to Sprite Fs_ReadLink.
 *
 * Results:
 *	UNIX_ERROR is returned upon error, with the actual error code
 *	stored in errno.  Upon success, the number of bytes actually
 *	read (ie. the length of the link's target pathname) is returned.
 *	
 *
 * Side effects:
 *	The buffer is filled with the number of bytes indicated by
 *	the length parameter.  
 *
 *----------------------------------------------------------------------
 */

int
readlink(link, buffer, numBytes)
    char *link;			/* name of link file to read */
    char *buffer;		/* pointer to buffer area */
    int numBytes;		/* number of bytes to read */
{
    ReturnStatus status;
    int amountRead;		/* place to hold number of bytes read */
    kern_return_t kernStatus;
    mach_msg_type_number_t linkNameLength = strlen(link) + 1;
    Boolean sigPending;

    kernStatus = Fs_ReadLinkStub(SpriteEmu_ServerPort(), link,
				 linkNameLength, numBytes,
				 (vm_address_t)buffer, &status,
				 &amountRead, &sigPending);
    if (kernStatus != KERN_SUCCESS) {
	status = Utils_MapMachStatus(kernStatus);
    }
    if (sigPending) {
	SpriteEmu_TakeSignals();
    }
    if (status != SUCCESS) {
	errno = Compat_MapCode(status);
	return(UNIX_ERROR);
    } else {
	/*
	 * Sprite's Fs_ReadLink includes the terminating null character
	 * in the character count return (amountRead) while Unix doesn't.
	 *
	 * ** NOTE ** this check can go away  once all hosts are running
	 * kernels that fix this before returning the value.
	 */
	if (buffer[amountRead-1] == '\0') {
	    amountRead--;
	}
	
	return(amountRead);
    }
}

