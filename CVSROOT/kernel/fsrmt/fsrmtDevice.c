/* 
 * fsDevice.c --
 *
 *	Routines to manage devices.  Remote devices are	supported.
 *	The handle for a device is uniquified using the device type and
 *	unit number.  This ensures that different filesystem names for
 *	the same device map to the same device I/O handle.
 *
 *	There are two sets of routines here.  There are routines internal
 *	to the filesystem that are used to open, close, read, write, etc.
 *	a device stream.  Then there are some external routines exported
 *	for use by device drivers.  Fs_NotifyReader and Fs_NotifyWriter
 *	are used by interrupt handlers to indicate a device is ready.
 *	Then there are a handful of conversion routines for mapping
 *	from file system block numbers to disk addresses.
 *
 * Copyright 1987 Regents of the University of California
 * All rights reserved.
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint


#include "sprite.h"

#include "fs.h"
#include "fsInt.h"
#include "../dev.bw/devFsOpTable.h"
#include "fsDevice.h"
#include "fsOpTable.h"
#include "fsDebug.h"
#include "fsFile.h"
#include "fsDisk.h"
#include "fsClient.h"
#include "fsStream.h"
#include "fsLock.h"
#include "fsMigrate.h"
#include "fsNameOpsInt.h"

#include "dev.h"
#include "rpc.h"
#include "fsStat.h"

/*
 * Parameters for RPC_FS_DEV_OPEN remote procedure call.
 * The return value from this call is a new I/O fileID.
 */
typedef struct FsDeviceRemoteOpenPrm {
    Fs_FileID	fileID;		/* I/O fileID from name server. */
    int		useFlags;	/* FS_READ | FS_WRITE ... */
    int		dataSize;	/* size of openData */
    FsUnionData	openData;	/* FsFileState, FsDeviceState or PdevState.
				 * NOTE. be careful when assigning this.
				 * bcopy() of the whole union can cause
				 * bus errors if really only a small object
				 * exists and it's at the end of a page. */
} FsDeviceRemoteOpenPrm;

/*
 * INET is defined so a file server can be used to open the
 * special device file corresponding to a kernel-based ipServer
 */
#define INET
#ifdef INET
#include "netInet.h"
/*
 * DEV_PLACEHOLDER_2	is defined in devTypesInt.h, which is not exported.
 *	This is an ugly hack, anyway, so we just define it here.  3/89
 *	The best solution is to define a new disk file type and not
 *	use FsDeviceSrvOpen at all.
 */
#define DEV_PLACEHOLDER_2	3
static int sockCounter = 0;
#endif


static void ReadNotify();
static void WriteNotify();
static void ExceptionNotify();


/*
 *----------------------------------------------------------------------
 *
 * FsDeviceHandleInit --
 *
 *	Initialize a handle for a local device.
 *
 * Results:
 *	TRUE if the handle was already found, FALSE if not.
 *
 * Side effects:
 *	Create and install a handle for the device.  It is returned locked
 *	and with its reference count incremented if SUCCESS is returned.
 *
 *----------------------------------------------------------------------
 */
Boolean
FsDeviceHandleInit(fileIDPtr, name, newHandlePtrPtr)
    Fs_FileID		*fileIDPtr;
    char		*name;
    FsDeviceIOHandle	**newHandlePtrPtr;
{
    register Boolean found;
    register FsDeviceIOHandle *devHandlePtr;

    found = FsHandleInstall(fileIDPtr, sizeof(FsDeviceIOHandle), name,
		    (FsHandleHeader **)newHandlePtrPtr);
    if (!found) {
	devHandlePtr = *newHandlePtrPtr;
	List_Init(&devHandlePtr->clientList);
	devHandlePtr->use.ref = 0;
	devHandlePtr->use.write = 0;
	devHandlePtr->use.exec = 0;
	devHandlePtr->device.serverID = fileIDPtr->serverID;
	devHandlePtr->device.type = fileIDPtr->major;
	devHandlePtr->device.unit = fileIDPtr->minor;
	devHandlePtr->device.data = (ClientData)NIL;
	FsLockInit(&devHandlePtr->lock);
	devHandlePtr->modifyTime = 0;
	devHandlePtr->accessTime = 0;
	List_Init(&devHandlePtr->readWaitList);
	List_Init(&devHandlePtr->writeWaitList);
	List_Init(&devHandlePtr->exceptWaitList);
	devHandlePtr->notifyFlags = 0;
	fsStats.object.devices++;
    }
    return(found);
}


/*
 *----------------------------------------------------------------------
 *
 * FsDeviceSrvOpen --
 *
 *	This routine sets up an ioFileID for the device based on the
 *	device file found on the name server.  This is called on the name
 *	server in two cases, when a client is doing an open, and when
 *	it is doing a Get/Set attributes on a device file name.   At
 *	open time some additional attributes are returned to the client
 *	for caching at the I/O server, and a streamID is chosen.
 *	Note that no state is kept about the device client here on the
 *	name server.  The device client open routine sets up that state.
 *
 * Results:
 *	SUCCESS, *ioFileIDPtr has the I/O file ID, and *clientDataPtr
 *	references the device state.
 *
 * Side effects:
 *	Choose the fileID for the clients stream.
 *	Allocates memory to hold the stream data.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceSrvOpen(handlePtr, openArgsPtr, openResultsPtr)
    FsLocalFileIOHandle	*handlePtr;	/* A handle obtained by FsLocalLookup.
					 * Should be locked upon entry,
					 * is unlocked upon exit. */
     FsOpenArgs		*openArgsPtr;	/* Standard open arguments */
     FsOpenResults	*openResultsPtr;/* For returning ioFileID, streamID,
					 * and FsDeviceState */
{
    register FsFileDescriptor *descPtr = handlePtr->descPtr;
    register FsDeviceState *deviceDataPtr;
    register Fs_FileID *ioFileIDPtr = &openResultsPtr->ioFileID;

    ioFileIDPtr->serverID = descPtr->devServerID;
    if (ioFileIDPtr->serverID == FS_LOCALHOST_ID) {
	/*
	 * Map this "common" or "generic" device to the instance of
	 * the device on the process's logical home node.
	 */
	ioFileIDPtr->serverID = openArgsPtr->migClientID;
    }
    ioFileIDPtr->major = descPtr->devType;
    ioFileIDPtr->minor = descPtr->devUnit;
#ifdef INET
    /*
     * Hack in support for sockets by mapping a special device type
     * to sockets.  This needs to be replaced with a new type of disk file.
     */
    if (descPtr->devType == DEV_PLACEHOLDER_2) {
	ioFileIDPtr->major = rpc_SpriteID;
	ioFileIDPtr->minor = sockCounter++;
	switch(descPtr->devUnit) {
	    case NET_IP_PROTOCOL_IP:
		ioFileIDPtr->type = FS_RAW_IP_STREAM;
		break;
	    case NET_IP_PROTOCOL_UDP:
		ioFileIDPtr->type = FS_UDP_STREAM;
		break;
	    case NET_IP_PROTOCOL_TCP:
		ioFileIDPtr->type = FS_TCP_STREAM;
		break;
	    default:
		ioFileIDPtr->major = descPtr->devType;
		ioFileIDPtr->minor = descPtr->devUnit;
		if (ioFileIDPtr->serverID == openArgsPtr->clientID) {
		    ioFileIDPtr->type = FS_LCL_DEVICE_STREAM;
		} else {
		    ioFileIDPtr->type = FS_RMT_DEVICE_STREAM;
		}
		break;
	}
    } else
#endif
    if (ioFileIDPtr->serverID == openArgsPtr->clientID) {
	ioFileIDPtr->type = FS_LCL_DEVICE_STREAM;
    } else {
	ioFileIDPtr->type = FS_RMT_DEVICE_STREAM;
    }
    if (openArgsPtr->useFlags != 0) {
	/*
	 * Truely preparing for an open.
	 * Return the current modify/access times for the I/O server's cache.
	 */
	deviceDataPtr = mnew(FsDeviceState);
	deviceDataPtr->modifyTime = descPtr->dataModifyTime;
	deviceDataPtr->accessTime = descPtr->accessTime;
	/*
	 * Choose a streamID for the client that points to the device server.
	 */
	FsStreamNewID(ioFileIDPtr->serverID, &openResultsPtr->streamID);
	deviceDataPtr->streamID = openResultsPtr->streamID;

	openResultsPtr->streamData = (ClientData)deviceDataPtr;
	openResultsPtr->dataSize = sizeof(FsDeviceState);
    }
    FsHandleUnlock(handlePtr);
    return(SUCCESS);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceCltOpen --
 *
 *	Complete opening of a local device.  This is called on the I/O
 *	server and sets up state concerning this client.  The device
 *	driver open routine is called to set up the device.  If that
 *	succeeds then the device's Handle is installed and set up.
 *	This includes incrementing client's use counts and the
 *	global use counts in the handle.
 *
 * Results:
 *	SUCCESS or a device dependent open error code.
 *
 * Side effects:
 *	Sets up and installs the device's ioHandle.  The device-type open
 *	routine is called on the I/O server.  The handle is unlocked
 *	upon exit, but its reference count has been incremented.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceCltOpen(ioFileIDPtr, flagsPtr, clientID, streamData, name, ioHandlePtrPtr)
    register Fs_FileID	*ioFileIDPtr;	/* I/O fileID */
    int			*flagsPtr;	/* FS_READ | FS_WRITE ... */
    int			clientID;	/* Host doing the open */
    ClientData		streamData;	/* Reference to FsDeviceState struct */
    char		*name;		/* File name for error msgs */
    FsHandleHeader	**ioHandlePtrPtr;/* Return - a locked handle set up for
					 * I/O to a device, NIL if failure. */
{
    ReturnStatus 		status;
    Boolean			found;
    register FsDeviceState	*deviceDataPtr;
    register FsDeviceIOHandle	*devHandlePtr;
    FsDeviceIOHandle		*tDevHandlePtr;	/* tempory devHandlePtr */
    register Fs_Stream		*streamPtr;
    register int		flags = *flagsPtr;

    deviceDataPtr = (FsDeviceState *)streamData;
    *ioHandlePtrPtr = (FsHandleHeader *)NIL;

    found = FsDeviceHandleInit(ioFileIDPtr, name, &tDevHandlePtr);
    devHandlePtr = tDevHandlePtr;
    /*
     * The device driver open routine gets the device specification,
     * the useFlags, and a token passed to Fs_NotifyReader
     * or Fs_NotifyWriter when the device becomes ready for I/O.
     */
    if (DEV_TYPE_INDEX(devHandlePtr->device.type) >= devNumDevices) {
	status = FS_DEVICE_OP_INVALID;
    } else {
	status = (*devFsOpTable[DEV_TYPE_INDEX(devHandlePtr->device.type)].open)
		    (&devHandlePtr->device, flags, (Fs_NotifyToken)devHandlePtr);
    }
    if (status == SUCCESS) {
	if (!found) {
	    /*
	     * Absorb the I/O attributes returned from the SrvOpen routine
	     * on the file server.
	     */
	    devHandlePtr->modifyTime = deviceDataPtr->modifyTime;
	    devHandlePtr->accessTime = deviceDataPtr->accessTime;
	}
	/*
	 * Trace the client, and then update our overall use counts.
	 * The client is recorded at the stream level to support migration,
	 * and at the I/O handle level for regular I/O.
	 */
	(void)FsIOClientOpen(&devHandlePtr->clientList, clientID, flags, FALSE);

	streamPtr = FsStreamAddClient(&deviceDataPtr->streamID, clientID,
			(FsHandleHeader *)devHandlePtr, flags,
			name, (Boolean *)NIL, (Boolean *)NIL);
	FsHandleRelease(streamPtr, TRUE);

	devHandlePtr->use.ref++;
	if (flags & FS_WRITE) {
	    devHandlePtr->use.write++;
	}
	*ioHandlePtrPtr = (FsHandleHeader *)devHandlePtr;
	FsHandleUnlock(devHandlePtr);
    } else {
	FsHandleRelease(devHandlePtr, TRUE);
    }
    free((Address) deviceDataPtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsRmtDeviceCltOpen --
 *
 *      Set up the stream IO handle for a remote device.  This makes
 *	an RPC to the I/O server, which will invoke FsDeviceCltOpen there,
 *	and then sets up the remote device handle.
 *
 * Results:
 *	SUCCESS or a device dependent open error code.
 *
 * Side effects:
 *	Sets up and installs the remote device's ioHandle.
 *	The use counts on the handle are updated.
 *	The handle is returned unlocked, but with a new
 *	reference than gets released when the device is closed.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsRmtDeviceCltOpen(ioFileIDPtr, flagsPtr, clientID, streamData, name, ioHandlePtrPtr)
    Fs_FileID		*ioFileIDPtr;	/* I/O fileID */
    int			*flagsPtr;	/* FS_READ | FS_WRITE ... */
    int			clientID;	/* Host doing the open */
    ClientData		streamData;	/* FsDeviceState */
    char		*name;		/* Device file name */
    FsHandleHeader	**ioHandlePtrPtr;/* Return - a handle set up for
					 * I/O to a device, NIL if failure. */
{
    register ReturnStatus 	status;

    /*
     * Do a device open at the I/O server.  We set the ioFileID type so
     * that the local device client open procedure gets called at the I/O
     * server, as opposed to the local pipe (or whatever) open routine.
     * NAME note: are not passing the file name to the I/O server.
     */
    ioFileIDPtr->type = FS_LCL_DEVICE_STREAM;
    status = FsDeviceRemoteOpen(ioFileIDPtr, *flagsPtr,	sizeof(FsDeviceState),
				streamData);
    if (status == SUCCESS) {
	ioFileIDPtr->type = FS_RMT_DEVICE_STREAM;
	FsRemoteIOHandleInit(ioFileIDPtr, *flagsPtr, name, ioHandlePtrPtr);
    } else {
	*ioHandlePtrPtr = (FsHandleHeader *)NIL;
    }
    free((Address)streamData);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsRemoteIOHandleInit --
 *
 *	Initialize a handle for a remote device/pseudo-device/whatever.
 *
 * Results:
 *	Sets its *ioHandlePtrPtr to reference the installed handle.
 *
 * Side effects:
 *	Create and install a handle for remote thing.  The handle is
 *	returned unlocked.  The recovery use counts are incremented
 *	to reflect the use of the handle.
 *
 *----------------------------------------------------------------------
 */
void
FsRemoteIOHandleInit(ioFileIDPtr, useFlags, name, newHandlePtrPtr)
    Fs_FileID		*ioFileIDPtr;		/* Remote IO File ID */
    int			useFlags;		/* Stream usage flags */
    char		*name;			/* File name */
    FsHandleHeader	**newHandlePtrPtr;	/* Return - installed handle */
{
    register Boolean found;
    register FsRecoveryInfo *recovPtr;

    found = FsHandleInstall(ioFileIDPtr, sizeof(FsRemoteIOHandle), name,
	    newHandlePtrPtr);
    recovPtr = &((FsRemoteIOHandle *)*newHandlePtrPtr)->recovery;
    if (!found) {
	FsRecoveryInit(recovPtr);
	fsStats.object.remote++;
    }
    recovPtr->use.ref++;
    if (useFlags & FS_WRITE) {
	recovPtr->use.write++;
    }
    FsHandleUnlock(*newHandlePtrPtr);
}

/*----------------------------------------------------------------------
 *
 * FsDeviceRemoteOpen --
 *
 *	Client side stub to open a remote device, remote named pipe,
 *	or remote pseudo stream.  Uses the RPC_FS_DEV_OPEN remote
 *	procedure call to invoke the pipe, device, or pseudo device
 *	open routine on the I/O server.  We are given an ioFileID from
 *	the name server, although we just use the sererID part here.
 *	The I/O server returns a new fileID that connects to the device.
 *
 * Results:
 *	(May) modify *ioFileIDPtr.  Returns a status from the RPC or the
 *	I/O device driver.
 *
 * Side effects:
 *	Sets up a recovery reboot call-back for the I/O server if the remote
 *	device open succeeds.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceRemoteOpen(ioFileIDPtr, useFlags, inSize, inBuffer)
    Fs_FileID	*ioFileIDPtr;	/* Indicates I/O server.  This is modified
				 * by the I/O server and returned to our
				 * caller for use in the dev/pipe/etc handle */
    int		useFlags;	/* FS_READ | FS_WRITE ... */
    int		inSize;		/* Size of input parameters */
    ClientData	inBuffer;	/* Input data for remote server */
{
    ReturnStatus	status;		/* Return code from RPC */
    Rpc_Storage		storage;	/* Specifies inputs/outputs to RPC */
    FsDeviceRemoteOpenPrm param;

    param.fileID = *ioFileIDPtr;
    param.useFlags = useFlags;
    param.dataSize = inSize;
    if (inSize > 0) {
	bcopy((Address)inBuffer, (Address)&param.openData, inSize);
    }
    storage.requestParamPtr = (Address) &param;
    storage.requestParamSize = sizeof(FsDeviceRemoteOpenPrm);
    storage.requestDataPtr = (Address) NIL;
    storage.requestDataSize = 0;
    storage.replyParamPtr = (Address) ioFileIDPtr;
    storage.replyParamSize = sizeof(Fs_FileID);
    storage.replyDataPtr = (Address) NIL;
    storage.replyDataSize = 0;

    status = Rpc_Call(ioFileIDPtr->serverID, RPC_FS_DEV_OPEN, &storage);

    if (status == SUCCESS) {
	/*
	 * Register a callback with the recovery module.  This ensures that
	 * someone is paying attention to the I/O server and the filesystem
	 * will get called back when the I/O server reboots.
	 */
	Recov_RebootRegister(ioFileIDPtr->serverID, FsReopen, (ClientData)NIL);
    }
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_RpcDevOpen --
 *
 *	Server stub for the RPC_FS_DEV_OPEN call.
 *	This host is the IO server for a handle.  This message from the
 *	remote host indicates that a client process there will be
 *	using us as the IO server.  This adds that client to the handle's
 *	client list for recovery and consistency checks.  This then branches
 *	to the file type client-open procedure to set up an I/O
 *	handle for the (device, pipe, pseudo-device).
 *
 * Results:
 *	If this procedure returns SUCCESS then a reply has been sent to
 *	the client.  If the arguments are bad then an error is 
 *	returned and the main RPC server level sends back an error reply.
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
Fs_RpcDevOpen(srvToken, clientID, command, storagePtr)
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
    FsHandleHeader	*hdrPtr;	/* I/O handle created by type-specific
					 * open routine. */
    ReturnStatus	status;
    FsDeviceRemoteOpenPrm *paramPtr;
    register int	dataSize;
    ClientData		streamData;

    /*
     * Call the client-open routine to set up an I/O handle here on the
     * I/O server for the device.  This is either the device, pipe, or
     * named pipe open routine.  We allocate storage and copy the stream
     * data so the CltOpen routine can free it, as it expects to do.
     * NAME note: we don't have a name for the device here.
     */
    paramPtr = (FsDeviceRemoteOpenPrm *) storagePtr->requestParamPtr;
    dataSize = paramPtr->dataSize;
    if (dataSize > 0) {
	streamData = (ClientData)malloc(dataSize);
	bcopy((Address)&paramPtr->openData, (Address)streamData, dataSize);
    } else {
	streamData = (ClientData)NIL;
    }
    paramPtr->fileID.type = FsMapRmtToLclType(paramPtr->fileID.type);
    if (paramPtr->fileID.type < 0) {
	return(GEN_INVALID_ARG);
    }
    status = (fsStreamOpTable[paramPtr->fileID.type].cltOpen)
		    (&paramPtr->fileID, &paramPtr->useFlags,
		     clientID, streamData, (char *)NIL, &hdrPtr);
    if (status == SUCCESS) {
	/*
	 * Return the fileID to the other host so it can
	 * set up its own I/O handle.
	 */
	storagePtr->replyParamPtr = (Address)&hdrPtr->fileID;
	storagePtr->replyParamSize = sizeof(Fs_FileID);
    }
    Rpc_Reply(srvToken, status, storagePtr, (int (*)())NIL, (ClientData)NIL);
    return(SUCCESS);	/* So that Rpc_Server doesn't send error reply */
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceClose --
 *
 *	Close a stream to a device.  We just need to clean up our state with
 *	the device driver.  The file server that named the device file keeps
 *	no state about us, so we don't have to contact it.
 *
 * FIX ME: need to write back access/modify times to name server
 *	Can use fsAttrOpTable and the nameInfoPtr->fileID as long
 *	if the shadow stream on the I/O server is set up enough.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Calls the device type close routine.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceClose(streamPtr, clientID, procID, flags, size, data)
    Fs_Stream		*streamPtr;	/* Stream to device */
    int			clientID;	/* HostID of client closing */
    Proc_PID		procID;		/* ID of closing process */
    int			flags;		/* Flags from the stream being closed */
    int			size;		/* Should be zero */
    ClientData		data;		/* IGNORED */
{
    ReturnStatus		status;
    register FsDeviceIOHandle	*devHandlePtr =
	    (FsDeviceIOHandle *)streamPtr->ioHandlePtr;
    Boolean			cache = FALSE;

    if (!FsIOClientClose(&devHandlePtr->clientList, clientID, flags, &cache)) {
	printf("FsDeviceClose, client %d unknown for device <%d,%d>\n",
		  clientID, devHandlePtr->hdr.fileID.major,
		  devHandlePtr->hdr.fileID.minor);
	FsHandleRelease(devHandlePtr, TRUE);
	return(FS_STALE_HANDLE);
    }
    /*
     * Decrement use counts.
     */
    FsLockClose(&devHandlePtr->lock, &streamPtr->hdr.fileID);

    devHandlePtr->use.ref--;
    if (flags & FS_WRITE) {
	devHandlePtr->use.write--;
    }
    /*
     * Call the driver's close routine to clean up.
     */
    status = (*devFsOpTable[DEV_TYPE_INDEX(devHandlePtr->device.type)].close)
		(&devHandlePtr->device, flags, devHandlePtr->use.ref,
			devHandlePtr->use.write);

    if (devHandlePtr->use.ref < 0 || devHandlePtr->use.write < 0) {
	panic("FsDeviceClose <%d,%d> ref %d, write %d\n",
	    devHandlePtr->hdr.fileID.major, devHandlePtr->hdr.fileID.minor,
	    devHandlePtr->use.ref, devHandlePtr->use.write);
    }
    /*
     * We don't bother to remove the handle here if the device isn't
     * being used.  Instead we let the handle get scavenged.
     */
    FsHandleRelease(devHandlePtr, TRUE);

    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsDeviceClientKill --
 *
 *	Called when a client is assumed down.  This cleans up the
 *	references due to the client.
 *	
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Removes the client list entry for the client and adjusts the
 *	use counts on the file.  This unlocks the handle.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
void
FsDeviceClientKill(hdrPtr, clientID)
    FsHandleHeader	*hdrPtr;	/* File to clean up */
    int			clientID;	/* Host assumed down */
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)hdrPtr;
    register int flags;
    int refs, writes, execs;

    /*
     * Remove the client from the list of users, and see what it was doing.
     */
    FsIOClientKill(&devHandlePtr->clientList, clientID, &refs, &writes, &execs);

    FsLockClientKill(&devHandlePtr->lock, clientID);

    if (refs > 0) {
	/*
	 * Set up flags to emulate a close by the client.
	 */
	flags = FS_READ;
	if (writes) {
	    flags |= FS_WRITE;
	}
	/*
	 * Decrement use counts and call the driver close routine.
	 */
	devHandlePtr->use.ref -= refs;
	if (flags & FS_WRITE) {
	    devHandlePtr->use.write -= writes;
	}
	(void)(*devFsOpTable[DEV_TYPE_INDEX(devHandlePtr->device.type)].close)
		(&devHandlePtr->device, flags, devHandlePtr->use.ref,
		    devHandlePtr->use.write);
    
	if (devHandlePtr->use.ref < 0 || devHandlePtr->use.write < 0) {
	    panic( "FsDeviceClose <%d,%d> ref %d, write %d\n",
		hdrPtr->fileID.major, hdrPtr->fileID.minor,
		devHandlePtr->use.ref, devHandlePtr->use.write);
	}
    }
    FsHandleUnlock(devHandlePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FsRemoteIOClose --
 *
 *	Close a stream to a remote device/pipe.  We just need to clean up our
 *	connection to the I/O server.  (The file server that named the
 *	device file keeps no state about us, so we don't have to contact it.)
 *	We make an RPC to the I/O server which invokes close routine there.
 *	We also update our own use counts.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	RPC to the I/O server to invoke FsDeviceClose/FsPipeClose.
 *	Release the remote I/O handle.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsRemoteIOClose(streamPtr, clientID, procID, flags, dataSize, closeData)
    Fs_Stream		*streamPtr;	/* Stream to remote object */
    int			clientID;	/* ID of closing host */
    Proc_PID		procID;		/* ID of closing process */
    int			flags;		/* Flags from the stream being closed */
    int			dataSize;	/* Size of *closeData, or Zero */
    ClientData		closeData;	/* Copy of cached I/O attributes. */
{
    ReturnStatus		status;
    register FsRemoteIOHandle *rmtHandlePtr =
	    (FsRemoteIOHandle *)streamPtr->ioHandlePtr;

    /*
     * Decrement local references.
     */
    rmtHandlePtr->recovery.use.ref--;
    if (flags & FS_WRITE) {
	rmtHandlePtr->recovery.use.write--;
    }

    if (rmtHandlePtr->recovery.use.ref < 0 ||
	rmtHandlePtr->recovery.use.write < 0) {
	panic( "FsRemoteIOClose: <%d,%d> ref %d write %d\n",
	    rmtHandlePtr->hdr.fileID.major, rmtHandlePtr->hdr.fileID.minor,
	    rmtHandlePtr->recovery.use.ref,
	    rmtHandlePtr->recovery.use.write);
    }

    if (!FsHandleValid(streamPtr->ioHandlePtr)) {
	status = FS_STALE_HANDLE;
    } else {
	status = FsRemoteClose(streamPtr, clientID, procID, flags,
			       dataSize, closeData);
    }
    /*
     * Check the number of users with the handle still locked, then
     * remove the handle if we aren't using it anymore.
     */
    if (status == SUCCESS && rmtHandlePtr->recovery.use.ref == 0) {
	FsRecoverySyncLockCleanup(&rmtHandlePtr->recovery);
	FsHandleRelease(rmtHandlePtr, TRUE);
	FsHandleRemove(rmtHandlePtr);
	fsStats.object.remote--;
    } else {
	FsHandleRelease(rmtHandlePtr, TRUE);
    }
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsDeviceScavenge --
 *
 *	Called periodically to see if this handle is still needed.
 *	
 *
 * Results:
 *	TRUE if the handle was removed.
 *
 * Side effects:
 *	Removes the handle if it isn't referenced anymore and there
 *	are no remote clients.
 *
 * ----------------------------------------------------------------------------
 *
 */
Boolean
FsDeviceScavenge(hdrPtr)
    FsHandleHeader	*hdrPtr;	/* File to clean up */
{
    register FsDeviceIOHandle *handlePtr = (FsDeviceIOHandle *)hdrPtr;

    if (handlePtr->use.ref == 0) {
	/*
	 * Remove handles for devices with no users.
	 */
	FsWaitListDelete(&handlePtr->readWaitList);
	FsWaitListDelete(&handlePtr->writeWaitList);
	FsWaitListDelete(&handlePtr->exceptWaitList);
	FsHandleRemove(handlePtr);
	fsStats.object.devices--;
	return(TRUE);
    } else {
        FsHandleUnlock(handlePtr);
	return(FALSE);
    }
}

/*
 * Parameters for a device reopen RPC used to reestablish state on
 * the I/O server for a device.
 */
typedef struct FsRmtDeviceReopenParams {
    Fs_FileID	fileID;		/* File ID of file to reopen. MUST BE FIRST! */
    FsUseCounts use;		/* Device usage information */
} FsRmtDeviceReopenParams;

/*
 *----------------------------------------------------------------------
 *
 * FsRmtDeviceReopen --
 *
 *	Reopen a remote device at the I/O server.  This sets up and conducts an 
 *	RPC_FS_DEV_REOPEN remote procedure call to re-open the remote device.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *	
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsRmtDeviceReopen(hdrPtr, clientID, inData, outSizePtr, outDataPtr)
    FsHandleHeader	*hdrPtr;	/* Device I/O handle to reopen */
    int			clientID;	/* Client doing the reopen */
    ClientData		inData;		/* IGNORED */
    int			*outSizePtr;	/* Size of returned data, 0 here */
    ClientData		*outDataPtr;	/* Returned data, NIL here */
{
    register FsRemoteIOHandle	*handlePtr = (FsRemoteIOHandle *)hdrPtr;
    ReturnStatus		status;
    int				outSize;
    FsRmtDeviceReopenParams	reopenParams;

    /*
     * Set up reopen parameters.  fileID must be first in order
     * to use the generic FsSpriteReopen/Fs_RpcReopen stubs.
     */
    reopenParams.fileID = handlePtr->hdr.fileID;
    reopenParams.use = handlePtr->recovery.use;

    outSize = 0;
    status = FsSpriteReopen(hdrPtr, sizeof(reopenParams),
			    (Address)&reopenParams, &outSize, (Address)NIL);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceReopen --
 *
 *	Reopen a device here on the I/O server.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *	
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceReopen(hdrPtr, clientID, inData, outSizePtr, outDataPtr)
    FsHandleHeader	*hdrPtr;	/* NIL on the I/O server */
    int			clientID;	/* Client doing the reopen */
    ClientData		inData;		/* Ref. to FsRmtDeviceReopenParams */
    int			*outSizePtr;	/* Size of returned data, 0 here */
    ClientData		*outDataPtr;	/* Returned data, NIL here */
{
    FsDeviceIOHandle	*devHandlePtr;
    FsUseCounts		oldUse;
    Boolean		found;
    ReturnStatus	status;
    register		devIndex;
    register FsRmtDeviceReopenParams *paramPtr =
	    (FsRmtDeviceReopenParams *)inData;

    *outDataPtr = (ClientData) NIL;
    *outSizePtr = 0;

    found = FsDeviceHandleInit(&paramPtr->fileID, (char *)NIL, &devHandlePtr); 

    devIndex = DEV_TYPE_INDEX(devHandlePtr->device.type);
    if (devIndex >= devNumDevices) {
	status = FS_DEVICE_OP_INVALID;
    } else {
	/*
	 * First check to see if we already know about the client.  If
	 * so, avoid the device driver re-open to avoid having transient
	 * communication failures wipe out connections to devices.
	 */
	FsIOClientStatus(&devHandlePtr->clientList, clientID, &oldUse);
	if (found &&
	    oldUse.ref == paramPtr->use.ref &&
	    oldUse.write == paramPtr->use.write) {
	    status == SUCCESS;
	} else {
	    status = (*devFsOpTable[devIndex].reopen)(&devHandlePtr->device,
		    paramPtr->use.ref, paramPtr->use.write,
		    (Fs_NotifyToken)devHandlePtr);
	    if (status == SUCCESS) {
		/*
		 * Update client use state to reflect the reopen.
		 */
		(void)FsIOClientReopen(&devHandlePtr->clientList, clientID,
					 &paramPtr->use);
	    }
	}
    }
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsDeviceRelease --
 *
 *	Release a reference from a Device I/O handle.
 *	
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Release the I/O handle.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceRelease(hdrPtr, flags)
    FsHandleHeader *hdrPtr;	/* File being encapsulated */
    int flags;			/* Use flags from the stream */
{
    panic( "FsDeviceRelease called\n");
    FsHandleRelease(hdrPtr, FALSE);
    return(SUCCESS);
}


/*
 * ----------------------------------------------------------------------------
 *
 * FsRemoteIORelease --
 *
 *	Release a reference on a remote I/O handle.  This decrements
 *	recovery use counts as well as releasing the handle.
 *	
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	Decrement recovery use counts and release the I/O handle.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsRemoteIORelease(hdrPtr, flags)
    FsHandleHeader *hdrPtr;	/* File being encapsulated */
    int flags;			/* Use flags from the stream */
{
    register FsRemoteIOHandle *rmtHandlePtr = (FsRemoteIOHandle *)hdrPtr;

    FsHandleLock(rmtHandlePtr);
    rmtHandlePtr->recovery.use.ref--;
    if (flags & FS_WRITE) {
	rmtHandlePtr->recovery.use.write--;
    }
    if (flags & FS_EXECUTE) {
	rmtHandlePtr->recovery.use.exec--;
    }
    FsHandleRelease(rmtHandlePtr, TRUE);
    return(SUCCESS);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsDeviceMigrate --
 *
 *	This takes care of transfering references from one client to the other.
 *	A useful side-effect of this routine is	to properly set the type in
 *	the ioFileID, either FS_LCL_DEVICE_STREAM or FS_RMT_DEVICE_STREAM.
 *	In the latter case FsRmtDevoceMigrate is called to do all the work.
 *
 * Results:
 *	An error status if the I/O handle can't be set-up.
 *	Otherwise SUCCESS is returned, *flagsPtr may have the FS_RMT_SHARED
 *	bit set, and *sizePtr and *dataPtr are set to reference FsDeviceState.
 *
 * Side effects:
 *	Sets the correct stream type on the ioFileID.
 *	Shifts client references from the srcClient to the destClient.
 *	Set up and return FsDeviceState for use by the MigEnd routine.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceMigrate(migInfoPtr, dstClientID, flagsPtr, offsetPtr, sizePtr, dataPtr)
    FsMigInfo	*migInfoPtr;	/* Migration state */
    int		dstClientID;	/* ID of target client */
    int		*flagsPtr;	/* In/Out Stream usage flags */
    int		*offsetPtr;	/* Return - new stream offset */
    int		*sizePtr;	/* Return - sizeof(FsDeviceState) */
    Address	*dataPtr;	/* Return - pointer to FsDeviceState */
{
    FsDeviceIOHandle			*devHandlePtr;
    Boolean				closeSrcClient;

    if (migInfoPtr->ioFileID.serverID != rpc_SpriteID) {
	/*
	 * The device was local, which is why we were called, but is now remote.
	 */
	migInfoPtr->ioFileID.type = FS_RMT_DEVICE_STREAM;
	return(FsRmtDeviceMigrate(migInfoPtr, dstClientID, flagsPtr, offsetPtr,
		sizePtr, dataPtr));
    }
    migInfoPtr->ioFileID.type = FS_LCL_DEVICE_STREAM;
    if (!FsDeviceHandleInit(&migInfoPtr->ioFileID, (char *)NIL, &devHandlePtr)){
	printf(
		"FsDeviceMigrate, I/O handle <%d,%d> not found\n",
		 migInfoPtr->ioFileID.major, migInfoPtr->ioFileID.minor);
	return(FS_FILE_NOT_FOUND);
    }
    /*
     * At the stream level, add the new client to the set of clients
     * for the stream, and check for any cross-network stream sharing.
     */
    FsStreamMigClient(migInfoPtr, dstClientID, (FsHandleHeader *)devHandlePtr,
		    &closeSrcClient);
    /*
     * Adjust use counts on the I/O handle to reflect any new sharing.
     */
    FsMigrateUseCounts(migInfoPtr->flags, closeSrcClient, &devHandlePtr->use);

    /*
     * Move the client at the I/O handle level.
     */
    FsIOClientMigrate(&devHandlePtr->clientList, migInfoPtr->srcClientID,
			dstClientID, migInfoPtr->flags, closeSrcClient);

    *sizePtr = 0;
    *dataPtr = (Address)NIL;
    *flagsPtr = migInfoPtr->flags;
    *offsetPtr = migInfoPtr->offset;
    /*
     * We don't need this reference on the I/O handle; there is no change.
     */
    FsHandleRelease(devHandlePtr, TRUE);
    return(SUCCESS);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsRmtDeviceMigrate --
 *
 *	This takes care of transfering references from one client to the other.
 *	A useful side-effect of this routine is	to properly set the type in
 *	the ioFileID, either FS_LCL_DEVICE_STREAM or FS_RMT_DEVICE_STREAM.
 *	In the latter case FsDevoceMigrate is called to do all the work.
 *
 * Results:
 *	An error status if the I/O handle can't be set-up.
 *	Otherwise SUCCESS is returned, *flagsPtr may have the FS_RMT_SHARED
 *	bit set, and *sizePtr and *dataPtr are set to reference FsDeviceState.
 *
 * Side effects:
 *	Sets the correct stream type on the ioFileID.
 *	Shifts client references from the srcClient to the destClient.
 *	Set up and return FsDeviceState for use by the MigEnd routine.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsRmtDeviceMigrate(migInfoPtr, dstClientID, flagsPtr, offsetPtr, sizePtr, dataPtr)
    FsMigInfo	*migInfoPtr;	/* Migration state */
    int		dstClientID;	/* ID of target client */
    int		*flagsPtr;	/* In/Out Stream usage flags */
    int		*offsetPtr;	/* Return - the new stream offset */
    int		*sizePtr;	/* Return - sizeof(FsDeviceState) */
    Address	*dataPtr;	/* Return - pointer to FsDeviceState */
{
    register ReturnStatus		status;

    if (migInfoPtr->ioFileID.serverID == rpc_SpriteID) {
	/*
	 * The device was remote, which is why we were called, but is now local.
	 */
	migInfoPtr->ioFileID.type = FS_LCL_DEVICE_STREAM;
	return(FsDeviceMigrate(migInfoPtr, dstClientID, flagsPtr, offsetPtr,
		sizePtr, dataPtr));
    }
    migInfoPtr->ioFileID.type = FS_RMT_DEVICE_STREAM;
    status = FsNotifyOfMigration(migInfoPtr, flagsPtr, offsetPtr,
				0, (Address)NIL);
    if (status != SUCCESS) {
	printf( "FsRmtDeviceMigrate, server error <%x>\n",
	    status);
    } else {
	*dataPtr = (Address)NIL;
	*sizePtr = 0;
    }
    return(status);
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsDeviceMigEnd --
 *
 *	Complete setup of a FS_DEVICE_STREAM after migration to the I/O server.
 *	The migrate routine has done the work of shifting use counts
 *	at the stream and I/O handle level.  This routine's job is
 *	to increment the low level I/O handle reference count to reflect
 *	the existence of a new stream to the I/O handle.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceMigEnd(migInfoPtr, size, data, hdrPtrPtr)
    FsMigInfo	*migInfoPtr;	/* Migration state */
    int		size;		/* Zero */
    ClientData	data;		/* NIL */
    FsHandleHeader **hdrPtrPtr;	/* Return - handle for the file */
{
    register FsDeviceIOHandle *devHandlePtr;

    devHandlePtr = FsHandleFetchType(FsDeviceIOHandle, &migInfoPtr->ioFileID);
    if (devHandlePtr == (FsDeviceIOHandle *)NIL) {
	panic( "FsDeviceMigEnd, no handlel\n");
	return(FAILURE);
    } else {
	FsHandleUnlock(devHandlePtr);
	*hdrPtrPtr = (FsHandleHeader *)devHandlePtr;
	return(SUCCESS);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * FsRemoteIOMigEnd --
 *
 *	Create a FS_RMT_DEVICE_STREAM or FS_RMT_PIPE_STREAM after migration.
 *	The srvMigrate routine has done most all the work.
 *	We just grab a reference on the I/O handle for the stream.
 *
 * Results:
 *	Sets the I/O handle.
 *
 * Side effects:
 *	May install the handle.  Ups use and reference counts.
 *
 * ----------------------------------------------------------------------------
 *
 */
/*ARGSUSED*/
ReturnStatus
FsRemoteIOMigEnd(migInfoPtr, size, data, hdrPtrPtr)
    FsMigInfo	*migInfoPtr;	/* Migration state */
    int		size;		/* Zero */
    ClientData	data;		/* NIL */
    FsHandleHeader **hdrPtrPtr;	/* Return - handle for the file */
{
    register FsRemoteIOHandle *rmtHandlePtr;
    register FsRecoveryInfo *recovPtr;
    Boolean found;

    found = FsHandleInstall(&migInfoPtr->ioFileID, sizeof(FsRemoteIOHandle),
		(char *)NIL, hdrPtrPtr);
    rmtHandlePtr = (FsRemoteIOHandle *)*hdrPtrPtr;
    recovPtr = &rmtHandlePtr->recovery;
    if (!found) {
	FsRecoveryInit(recovPtr);
	fsStats.object.remote++;
    }
    recovPtr->use.ref++;
    if (migInfoPtr->flags & FS_WRITE) {
	recovPtr->use.write++;
    }
    FsHandleUnlock(rmtHandlePtr);
    return(SUCCESS);
}

/*
 *----------------------------------------------------------------------
 *
 * FsRmtDeviceVerify --
 *
 *	Verify that the remote client is known for the device, and return
 *	a locked pointer to the device's I/O handle.
 *
 * Results:
 *	A pointer to the I/O handle for the device, or NIL if
 *	the client is bad.
 *
 * Side effects:
 *	The handle is returned locked and with its refCount incremented.
 *	It should be released with FsHandleRelease.
 *
 *----------------------------------------------------------------------
 */

FsHandleHeader *
FsRmtDeviceVerify(fileIDPtr, clientID, domainTypePtr)
    Fs_FileID	*fileIDPtr;	/* Client's I/O file ID */
    int		clientID;	/* Host ID of the client */
    int		*domainTypePtr;	/* Return - FS_LOCAL_DOMAIN */
{
    register FsDeviceIOHandle	*devHandlePtr;
    register FsClientInfo	*clientPtr;
    Boolean			found = FALSE;

    fileIDPtr->type = FsMapRmtToLclType(fileIDPtr->type);
    if (fileIDPtr->type != FS_LCL_DEVICE_STREAM) {
	printf("FsRmtDeviceVerify, bad file ID type\n");
	return((FsHandleHeader *)NIL);
    }
    devHandlePtr = FsHandleFetchType(FsDeviceIOHandle, fileIDPtr);
    if (devHandlePtr != (FsDeviceIOHandle *)NIL) {
	LIST_FORALL(&devHandlePtr->clientList, (List_Links *) clientPtr) {
	    if (clientPtr->clientID == clientID) {
		found = TRUE;
		break;
	    }
	}
	if (!found) {
	    FsHandleRelease(devHandlePtr, TRUE);
	    devHandlePtr = (FsDeviceIOHandle *)NIL;
	}
    }
    if (!found) {
	printf("FsRmtDeviceVerify, client %d not known for device <%d,%d>\n",
	    clientID, fileIDPtr->major, fileIDPtr->minor);
    }
    if (domainTypePtr != (int *)NIL) {
	*domainTypePtr = FS_LOCAL_DOMAIN;
    }
    return((FsHandleHeader *)devHandlePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceRead --
 *
 *      Read on a stream connected to a local peripheral device.
 *	This branches to the driver routine after setting up buffers.
 *	This is called from Fs_Read and from Fs_RpcRead.
 *
 * Results:
 *	SUCCESS unless there was an address error or I/O error.
 *
 * Side effects:
 *	Read the device.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceRead(streamPtr, readPtr, remoteWaitPtr, replyPtr)
    Fs_Stream		*streamPtr;	/* Open stream to the device. */
    Fs_IOParam		*readPtr;	/* Read parameter block. */
    Sync_RemoteWaiter	*remoteWaitPtr;	/* Process info for remote waiting */
    Fs_IOReply		*replyPtr;	/* Signal to return, if any,
					 * plus the amount read. */
{
    register FsDeviceIOHandle	*devHandlePtr =
	    (FsDeviceIOHandle *)streamPtr->ioHandlePtr;
    register ReturnStatus status;
    register Address	readBuffer;
    register Fs_Device	*devicePtr;
    Address userBuffer;

    FsHandleLock(devHandlePtr);
    /*
     * Because the read could take a while and we aren't mapping in
     * buffers, we have to allocate an extra buffer here so the
     * buffer address is valid when the device's interrupt handler
     * does its DMA.
     */
    if (readPtr->flags & FS_USER) {
	userBuffer = readPtr->buffer;
	readPtr->buffer = (Address)malloc(readPtr->length);
    }

    /*
     * Put the process onto the read-wait list before attempting the read.
     * This is to prevent races with the device's notification which
     * happens from an interrupt handler.
     */
    FsWaitListInsert(&devHandlePtr->readWaitList, remoteWaitPtr);
    devicePtr = &devHandlePtr->device;
    status = (*devFsOpTable[DEV_TYPE_INDEX(devicePtr->type)].read)(devicePtr,
		readPtr, replyPtr);
    if (readPtr->flags & FS_USER) {
        if (Vm_CopyOut(replyPtr->length, readPtr->buffer, userBuffer) != SUCCESS) {
	    if (status == SUCCESS) {
		status = SYS_ARG_NOACCESS;
	    }
	}
	free(readPtr->buffer);
	readPtr->buffer = userBuffer;
    }
    if (status != FS_WOULD_BLOCK) {
	FsWaitListRemove(&devHandlePtr->readWaitList, remoteWaitPtr);
    }
    devHandlePtr->accessTime = fsTimeInSeconds;
    fsStats.gen.deviceBytesRead += replyPtr->length;
    FsHandleUnlock(devHandlePtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceWrite --
 *
 *      Write on a stream connected to a local peripheral device.
 *	This is called from Fs_Write and Fs_RpcWrite.
 *
 * Results:
 *	SUCCESS unless there was an address error or I/O error.
 *
 * Side effects:
 *	Write to the device.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceWrite(streamPtr, writePtr, remoteWaitPtr, replyPtr)
    Fs_Stream		*streamPtr;	/* Open stream to the device. */
    Fs_IOParam		*writePtr;	/* Read parameter block */
    Sync_RemoteWaiter	*remoteWaitPtr;	/* Process info for remote waiting */
    Fs_IOReply		*replyPtr;	/* Signal to return, if any */
{
    register FsDeviceIOHandle	*devHandlePtr =
	    (FsDeviceIOHandle *)streamPtr->ioHandlePtr;
    ReturnStatus	status = SUCCESS;
    register Address	writeBuffer;
    register Fs_Device	*devicePtr = &devHandlePtr->device;
    Address		userBuffer;

    FsHandleLock(devHandlePtr);
    /*
     * Because the write could take a while and we aren't mapping in
     * buffers, we have to allocate an extra buffer here so the
     * buffer address is valid when the device's interrupt handler
     * does its DMA.
     */
    if (writePtr->flags & FS_USER) {
	userBuffer = writePtr->buffer;
        writePtr->buffer = (Address)malloc(writePtr->length);
	if (Vm_CopyIn(writePtr->length, userBuffer, writePtr->buffer) != SUCCESS) {
	    status = SYS_ARG_NOACCESS;
	}
    }
    if (status == SUCCESS) {
	/*
	 * Put the process onto the write-wait list before attempting the write.
	 * This is to prevent races with the device's notification which
	 * happens from an interrupt handler.
	 */
	FsWaitListInsert(&devHandlePtr->writeWaitList, remoteWaitPtr);
	status = (*devFsOpTable[DEV_TYPE_INDEX(devicePtr->type)].write)(devicePtr,
		writePtr, replyPtr);
	if (status != FS_WOULD_BLOCK) {
	    FsWaitListRemove(&devHandlePtr->writeWaitList, remoteWaitPtr);
	}
	devHandlePtr->modifyTime = fsTimeInSeconds;
	fsStats.gen.deviceBytesWritten += replyPtr->length;
    }

    if (writePtr->flags & FS_USER) {
	free(writePtr->buffer);
	writePtr->buffer = userBuffer;
    }
    FsHandleUnlock(devHandlePtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceSelect --
 *
 *      Select on a stream connected to a local peripheral device.  This
 *	ensures that the calling process is on a waiting list, then calls
 *	the device driver's select routine.  If the select succeeds, then
 *	the wait list items are removed.  The ordering of this is done to
 *	prevent races between the select routine and the notification that
 *	occurs at interrupt time.
 *
 * Results:
 *	A return from the driver, should be SUCCESS unless the
 *	device is offline or something.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceSelect(hdrPtr, waitPtr, readPtr, writePtr, exceptPtr)
    FsHandleHeader	*hdrPtr;	/* Handle on device to select */
    Sync_RemoteWaiter	*waitPtr;	/* Process info for remote waiting */
    int 		*readPtr;	/* Bit to clear if non-readable */
    int 		*writePtr;	/* Bit to clear if non-writeable */
    int 		*exceptPtr;	/* Bit to clear if non-exceptable */
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)hdrPtr;
    register Fs_Device	*devicePtr = &devHandlePtr->device;
    register ReturnStatus status;

    FsHandleLock(devHandlePtr);
    if (waitPtr != (Sync_RemoteWaiter *)NIL) {
	if (*readPtr) {
	    FsWaitListInsert(&devHandlePtr->readWaitList, waitPtr);
	}
	if (*writePtr) {
	    FsWaitListInsert(&devHandlePtr->writeWaitList, waitPtr);
	}
	if (*exceptPtr) {
	    FsWaitListInsert(&devHandlePtr->exceptWaitList, waitPtr);
	}
    }
    status = (*devFsOpTable[DEV_TYPE_INDEX(devicePtr->type)].select)(devicePtr,
		    readPtr, writePtr, exceptPtr);

    if (waitPtr != (Sync_RemoteWaiter *)NIL) {
	if (*readPtr != 0) {
	    FsWaitListRemove(&devHandlePtr->readWaitList, waitPtr);
	}
	if (*writePtr != 0) {
	    FsWaitListRemove(&devHandlePtr->writeWaitList, waitPtr);
	}
	if (*exceptPtr != 0) {
	    FsWaitListRemove(&devHandlePtr->exceptWaitList, waitPtr);
	}
    }
    FsHandleUnlock(devHandlePtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceIOControl --
 *
 *      Write on a stream connected to a peripheral device.  Called from
 *	FsDomainWrite.
 *
 * Results:
 *	SUCCESS unless there was an address error or I/O error.
 *
 * Side effects:
 *	Write to the device.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
FsDeviceIOControl(streamPtr, ioctlPtr, replyPtr)
    Fs_Stream *streamPtr;		/* Stream to a device. */
    Fs_IOCParam *ioctlPtr;		/* I/O Control parameter block */
    Fs_IOReply *replyPtr;		/* Return length and signal */
{
    register FsDeviceIOHandle *devHandlePtr =
	    (FsDeviceIOHandle *)streamPtr->ioHandlePtr;
    register Fs_Device	*devicePtr = &devHandlePtr->device;
    register ReturnStatus status;
    static Boolean warned = FALSE;

    FsHandleLock(devHandlePtr);
    switch (ioctlPtr->command) {
	case IOC_LOCK:
	case IOC_UNLOCK:
	    status = FsIocLock(&devHandlePtr->lock, ioctlPtr,
			&streamPtr->hdr.fileID);
	    break;
	default:
	    status = (*devFsOpTable[DEV_TYPE_INDEX(devicePtr->type)].ioctl)
		    (devicePtr, ioctlPtr, replyPtr);
	    break;
    }
    FsHandleUnlock(devHandlePtr);
    return(status);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceGetIOAttr --
 *
 *      Get the I/O attributes for a device.  A copy of the access and
 *	modify times are kept at the I/O server.  This routine is called
 *	either from Fs_GetAttrStream or Fs_RpcGetIOAttr to update
 *	the initial copy of the attributes obtained from the name server.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceGetIOAttr(fileIDPtr, clientID, attrPtr)
    Fs_FileID			*fileIDPtr;	/* FileID of device */
    int 			clientID;	/* IGNORED */
    register Fs_Attributes	*attrPtr;	/* Attributes to update */
{
    register FsDeviceIOHandle *devHandlePtr;

    devHandlePtr = FsHandleFetchType(FsDeviceIOHandle, fileIDPtr);
    if (devHandlePtr != (FsDeviceIOHandle *)NIL) {
	if (devHandlePtr->accessTime > attrPtr->accessTime.seconds) {
	    attrPtr->accessTime.seconds = devHandlePtr->accessTime;
	}
	if (devHandlePtr->modifyTime > attrPtr->dataModifyTime.seconds) {
	    attrPtr->dataModifyTime.seconds = devHandlePtr->modifyTime;
	}
	FsHandleRelease(devHandlePtr, TRUE);
    }
    return(SUCCESS);
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceSetIOAttr --
 *
 *      Set the I/O attributes for a device.  A copy of the access and
 *	modify times are kept at the I/O server.  This routine is called
 *	either from Fs_SetAttrStream or Fs_RpcSetIOAttr to update
 *	the cached copy of the attributes.
 *
 * Results:
 *	SUCCESS.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
/*ARGSUSED*/
ReturnStatus
FsDeviceSetIOAttr(fileIDPtr, attrPtr, flags)
    Fs_FileID			*fileIDPtr;	/* FileID of device */
    register Fs_Attributes	*attrPtr;	/* Attributes to copy */
    int				flags;		/* What attrs to set */
{
    register FsDeviceIOHandle *devHandlePtr;

    if (flags & FS_SET_TIMES) {
	devHandlePtr = FsHandleFetchType(FsDeviceIOHandle, fileIDPtr);
	if (devHandlePtr != (FsDeviceIOHandle *)NIL) {
	    devHandlePtr->accessTime = attrPtr->accessTime.seconds;
	    devHandlePtr->modifyTime = attrPtr->dataModifyTime.seconds;
	    FsHandleRelease(devHandlePtr, TRUE);
	}
    }
    return(SUCCESS);
}

/*
 *----------------------------------------------------------------------
 *
 * Fs_DevNotifyReader --
 *
 *	Fs_DevNotifyReader is available to device driver interrupt handlers
 *	that need to notify waiting processes that the device is readable.
 *	It schedules a process level call to ReadNotify, which
 *	in turn iterates down the list of handles for the device waking up
 *	all read waiters.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Schedules a call to DevListNotify, which in turn calls
 *	FsWaitListNotify to schedule any waiting readers.
 *
 *----------------------------------------------------------------------
 */
void
Fs_DevNotifyReader(notifyToken)
    Fs_NotifyToken notifyToken;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)notifyToken;

    if ((devHandlePtr == (FsDeviceIOHandle *)NIL) ||
	(devHandlePtr->notifyFlags & FS_READABLE)) {
	return;
    }
    if (devHandlePtr->hdr.fileID.type != FS_LCL_DEVICE_STREAM) {
	printf("Fs_DevNotifyReader, bad handle\n");
    }
    devHandlePtr->notifyFlags |= FS_READABLE;
    Proc_CallFunc(ReadNotify, (ClientData) devHandlePtr, 0);
}

static void
ReadNotify(data, callInfoPtr)
    ClientData		data;
    Proc_CallInfo	*callInfoPtr;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)data;
    if (devHandlePtr->hdr.fileID.type != FS_LCL_DEVICE_STREAM) {
	printf("ReadNotify, lost device handle\n");
    } else {
	devHandlePtr->notifyFlags &= ~FS_READABLE;
	FsWaitListNotify(&devHandlePtr->readWaitList);
    }
    callInfoPtr->interval = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Fs_DevNotifyWriter --
 *
 *	Fs_DevNotifyWriter is available to device driver interrupt handlers
 *	that need to notify waiting processes that the device is writeable.
 *	It schedules a process level call to Fs_WaitListNotifyStub on the
 *	devices's write wait list.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Schedules a call to Fs_WaitListNotifyStub.
 *
 *----------------------------------------------------------------------
 */
void
Fs_DevNotifyWriter(notifyToken)
    Fs_NotifyToken notifyToken;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)notifyToken;

    if ((devHandlePtr == (FsDeviceIOHandle *)NIL) ||
	(devHandlePtr->notifyFlags & FS_WRITABLE)) {
	return;
    }
    if (devHandlePtr->hdr.fileID.type != FS_LCL_DEVICE_STREAM) {
	printf("Fs_DevNotifyWriter, bad handle\n");
	return;
    }
    devHandlePtr->notifyFlags |= FS_WRITABLE;
    Proc_CallFunc(WriteNotify, (ClientData) devHandlePtr, 0);
}

static void
WriteNotify(data, callInfoPtr)
    ClientData		data;
    Proc_CallInfo	*callInfoPtr;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)data;
    if (devHandlePtr->hdr.fileID.type != FS_LCL_DEVICE_STREAM) {
	printf( "WriteNotify, lost device handle\n");
    } else {
	devHandlePtr->notifyFlags &= ~FS_WRITABLE;
	FsWaitListNotify(&devHandlePtr->writeWaitList);
    }
    callInfoPtr->interval = 0;
}



/*
 *----------------------------------------------------------------------
 *
 * Fs_DevNotifyException --
 *
 *	This is available to device driver interrupt handlers
 *	that need to notify waiting processes that there is an exception
 *	on the device.  This is only useful for processes waiting on
 *	exceptions in select.  This is not currently used.
 *	It schedules a process level call to Fs_WaitListNotifyStub on the
 *	devices's exception wait list.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Schedules a call to Fs_WaitListNotifyStub.
 *
 *----------------------------------------------------------------------
 */
void
Fs_DevNotifyException(notifyToken)
    Fs_NotifyToken notifyToken;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)notifyToken;

    if (devHandlePtr == (FsDeviceIOHandle *)NIL) {
	return;
    }
    Proc_CallFunc(ExceptionNotify, (ClientData) devHandlePtr, 0);
}

static void
ExceptionNotify(data, callInfoPtr)
    ClientData		data;
    Proc_CallInfo	*callInfoPtr;
{
    register FsDeviceIOHandle *devHandlePtr = (FsDeviceIOHandle *)data;
    FsWaitListNotify(&devHandlePtr->exceptWaitList);
    callInfoPtr->interval = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * FsDeviceBlockIO --
 *
 *	Map a file system block address to a block device block address 
 *	perform the requested operation.
 *
 * NOTE: This routine is temporary and should be replaced when the file system
 *	 is converted to use the async block io interface.
 *
 * Results:
 *	The return status of the operation.
 *
 * Side effects:
 *	Blocks may be written or read.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
FsDeviceBlockIO(readWriteFlag, devicePtr, fragNumber, numFrags, buffer)
    int readWriteFlag;		/* FS_READ or FS_WRITE */
    Fs_Device *devicePtr;	/* Specifies device type to do I/O with */
    int fragNumber;		/* CAREFUL, fragment index, not block index.
				 * This is relative to start of device. */
    int numFrags;		/* CAREFUL, number of fragments, not blocks */
    Address buffer;		/* I/O buffer */
{
    ReturnStatus status;	/* General return code */
    int firstSector;		/* Starting sector of transfer */
    DevBlockDeviceRequest	request;
    DevBlockDeviceHandle	*handlePtr;
    int				transferCount;

    handlePtr = (DevBlockDeviceHandle *) (devicePtr->data);
    if ((fragNumber % FS_FRAGMENTS_PER_BLOCK) != 0) {
	/*
	 * The I/O doesn't start on a block boundary.  Transfer the
	 * first few extra fragments to get things going on a block boundary.
	 */
	register int extraFrags;

	extraFrags = FS_FRAGMENTS_PER_BLOCK -
		    (fragNumber % FS_FRAGMENTS_PER_BLOCK);
	if (extraFrags > numFrags) {
	    extraFrags = numFrags;
	}
	firstSector = Fs_BlocksToSectors(fragNumber, handlePtr->clientData);
	request.operation = readWriteFlag;
	request.startAddress = firstSector * DEV_BYTES_PER_SECTOR;
	request.startAddrHigh = 0;
	request.bufferLen = extraFrags * FS_FRAGMENT_SIZE;
	request.buffer = buffer;
	status = Dev_BlockDeviceIOSync(handlePtr, &request, &transferCount);
	extraFrags = transferCount / FS_FRAGMENT_SIZE;
	fragNumber += extraFrags;
	buffer += transferCount;
	numFrags -= extraFrags;
	if (status != SUCCESS) {
	    return(status);
	}
    }
    while (numFrags >= FS_FRAGMENTS_PER_BLOCK) {
	/*
	 * Transfer whole blocks.
	 */
	firstSector = Fs_BlocksToSectors(fragNumber, handlePtr->clientData);
	request.operation = readWriteFlag;
	request.startAddress = firstSector * DEV_BYTES_PER_SECTOR;
	request.startAddrHigh = 0;
	request.bufferLen = FS_BLOCK_SIZE;
	request.buffer = buffer;
	status = Dev_BlockDeviceIOSync(handlePtr, &request, &transferCount);
	fragNumber += FS_FRAGMENTS_PER_BLOCK;
	buffer += FS_BLOCK_SIZE;
	numFrags -= FS_FRAGMENTS_PER_BLOCK;
	if (status != SUCCESS) {
	    return(status);
	}
    }
    if (numFrags > 0) {
	/*
	 * Transfer the left over fragments.
	 */
	firstSector = Fs_BlocksToSectors(fragNumber, handlePtr->clientData);
	request.operation = readWriteFlag;
	request.startAddress = firstSector * DEV_BYTES_PER_SECTOR;
	request.startAddrHigh = 0;
	request.bufferLen = numFrags * FS_FRAGMENT_SIZE;
	request.buffer = buffer;
	status = Dev_BlockDeviceIOSync(handlePtr, &request, &transferCount);
    }
    return(status);
}

