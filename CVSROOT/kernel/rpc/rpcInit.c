/* 
 * rpcInit.c --
 *
 *	Initialize the data structures needed by the RPC system.
 *
 * Copyright (C) 1985 Regents of the University of California
 * All rights reserved.
 */

#ifndef lint
static char rcsid[] = "$Header$ SPRITE (Berkeley)";
#endif not lint

#include "sprite.h"
#include "rpc.h"
#include "rpcClient.h"
#include "rpcServer.h"
#include "rpcTrace.h"
#include "vm.h"
#include "timer.h"
#include "net.h"


/*
 *----------------------------------------------------------------------
 *
 * Rpc_Init --
 *
 *      Allocate and set up the tables used by the RPC system.  This
 *      should be called after virtual memory allocation can be done and
 *      before any RPCs are attempted.  This allocates the Client Channel
 *	data structures and some stuff for the Rpc Servers' state.  The
 *	number of client channels is fixed by rpcNumChannels, but the
 *	number of RPC server processes can grow via the Rpc_Deamon process.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocate space for tables, and set the initial state for the
 *	client channels and the servers.
 *
 *----------------------------------------------------------------------
 */
void
Rpc_Init()
{
    register int i;
    register Net_ScatterGather *bufferPtr;
    register int frag;
    Net_EtherAddress etherAddress;

    /*
     * Initialize some time parameters.  The 'rpc' structure is used in
     * the RpcDoCall code.  The variables are used here for initialization
     */
    rpc.retryMsec = rpcRetryMsec;
    rpc.retryWait = rpcRetryMsec * timer_IntOneMillisecond;
    rpc.maxAckMsec = rpcMaxAckMsec;
    rpc.maxAckWait = rpcMaxAckMsec * timer_IntOneMillisecond;
    rpc.maxTimeoutMsec = rpcMaxTimeoutMsec;
    rpc.maxTimeoutWait = rpcMaxTimeoutMsec * timer_IntOneMillisecond;
    rpc.maxTries = rpcMaxTries;
    rpc.maxAcks = rpcMaxAcks;

    Trace_Init(rpcTraceHdrPtr, RPC_TRACE_LEN, sizeof(RpcHdr), 0);

    rpcServiceTime[0] = (Rpc_Histogram *)NIL;
    rpcCallTime[0] = (Rpc_Histogram *)NIL;
    for (i=1 ; i<=RPC_LAST_COMMAND ; i++) {
	rpcServiceTime[i] = Rpc_HistInit(RPC_NUM_HIST_BUCKETS, 1024);
	rpcCallTime[i] = Rpc_HistInit(RPC_NUM_HIST_BUCKETS, 1024);
    }

    /*
     * The client channel table is kept as a pointer to an array of pointers
     * to client channels.  First allocate the table of pointers and then
     * allocate storage for each channel.
     */
    rpcChannelPtrPtr = (RpcClientChannel **)
	    Vm_RawAlloc(rpcNumChannels * sizeof(RpcClientChannel *));

    for (i=0 ; i<rpcNumChannels ; i++) {
	register RpcClientChannel *chanPtr;

	chanPtr = (RpcClientChannel *)Vm_RawAlloc(sizeof(RpcClientChannel));
	rpcChannelPtrPtr[i] = chanPtr;

	chanPtr->state = CHAN_FREE;
	chanPtr->index = i;
	chanPtr->serverID = -1;
	chanPtr->mutex = 0;
	chanPtr->waitCondition.waiting = FALSE;

	/*
	 * Precompute some scatter/gather vector elements.  Buffer space
	 * for the RPC headers is part of the channel struct so the
	 * scatter/gather elements that point to the RPC header can
	 * be set up once here.  Similarly, the channel
	 * of the request RPC header is set up once here as it
	 * is always the same.  Finally, the conditionPtr that is part
	 * of the scatter/gather element is cleared because we don't
	 * use this feature of the network module.
	 */

	bufferPtr = &chanPtr->request.rpcHdrBuffer;
	bufferPtr->bufAddr = (Address)&chanPtr->requestRpcHdr;
	bufferPtr->length = sizeof(RpcHdr);
	bufferPtr->conditionPtr = (Sync_Condition *)NIL;
	chanPtr->requestRpcHdr.version = RPC_NATIVE_VERSION;
	chanPtr->requestRpcHdr.channel = chanPtr->index;
	chanPtr->request.paramBuffer.conditionPtr = (Sync_Condition *)NIL;
	chanPtr->request.dataBuffer.conditionPtr = (Sync_Condition *)NIL;
#ifdef RPC_TEST_BYTE_SWAP
	bufferPtr = &chanPtr->swapRequest.rpcHdrBuffer;
	bufferPtr->bufAddr = (Address)&chanPtr->swapRequestRpcHdr;
	bufferPtr->length = sizeof(RpcHdr);
	bufferPtr->conditionPtr = (Sync_Condition *)NIL;
	chanPtr->requestRpcHdr.version = RPC_NATIVE_VERSION;
	chanPtr->requestRpcHdr.channel = chanPtr->index;
	chanPtr->request.paramBuffer.conditionPtr = (Sync_Condition *)NIL;
	chanPtr->request.dataBuffer.conditionPtr = (Sync_Condition *)NIL;
#endif RPC_TEST_BYTE_SWAP

	for (frag=0 ; frag < RPC_MAX_NUM_FRAGS ; frag++) {

	    bufferPtr = &chanPtr->fragment[frag].rpcHdrBuffer;
	    bufferPtr->bufAddr = (Address)&chanPtr->fragRpcHdr[frag];
	    bufferPtr->length = sizeof(RpcHdr);
	    bufferPtr->conditionPtr = (Sync_Condition *)NIL;
	    chanPtr->fragRpcHdr[frag].version = RPC_NATIVE_VERSION;
	    chanPtr->fragRpcHdr[frag].channel = chanPtr->index;
	    chanPtr->fragment[frag].paramBuffer.conditionPtr =
			(Sync_Condition *)NIL;
	    chanPtr->fragment[frag].dataBuffer.conditionPtr =
			(Sync_Condition *)NIL;
	}

	bufferPtr = &chanPtr->reply.rpcHdrBuffer;
	bufferPtr->bufAddr = (Address)&chanPtr->replyRpcHdr;
	bufferPtr->length = sizeof(RpcHdr);
	bufferPtr->conditionPtr = (Sync_Condition *)NIL;
	chanPtr->reply.paramBuffer.conditionPtr = (Sync_Condition *)NIL;
	chanPtr->reply.dataBuffer.conditionPtr = (Sync_Condition *)NIL;
    }

    /*
     * Set our preferred inter-fragment delay based on machine type.
     * This is a microsecond value.  Our output rate starts the same
     * as the input rate, although MyDelay could increase if a machine
     * senses that it is overloaded.
     */

    RpcGetMachineDelay(&rpcMyDelay, &rpcOutputRate);

    /*
     * Initialize the servers' state table.  Most slots are left
     * uninitialized.  They get filled in by Rpc_Deamon when it creates
     * new server processes.  After creation, a server process
     * claims a table entry with RpcServerInstall.
     */
    rpcServerPtrPtr = (RpcServerState **)
	    Vm_RawAlloc(rpcMaxServers * sizeof(RpcServerState *));
    for (i=0 ; i<rpcMaxServers ; i++) {
	rpcServerPtrPtr[i] = (RpcServerState *)NIL;
    }

    /*
     * Ask the net module to set up our Sprite ID.  It uses either
     * existing (compiled in) addresses or Reverse ARP.  If we can't
     * figure out our ID we use zero and rely on the RPC server to
     * propogate our Sprite ID back in the first RPC reply message.
     */
    Mach_GetEtherAddress(&etherAddress);
    rpc_SpriteID = Net_AddrToID(0, NET_ROUTE_ETHER, (ClientData)&etherAddress);
    if (rpc_SpriteID < 0) {
	rpc_SpriteID = 0;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RpcInitServerState --
 *
 *	Initialize a server state table entry.  This is called before
 *	a server process is created to set up its state.  The state is
 *	used as the primary communication mechanism between the server
 *	process and the rest of the world.
 *
 * Results:
 *	A pointer to an initialized server state table entry.  This
 *	value needs to be saved in a table somewhere by the caller.
 *
 * Side effects:
 *	Allocate memory with Vm_RawAlloc.  Give initial values to
 *	all the elements of the table entry.  The "state" field of
 *	the table is set to SRV_NOTREADY and a server process has
 *	to claim the table entry with RpcServerInstall.
 *
 *----------------------------------------------------------------------
 */
RpcServerState *
RpcInitServerState(index)
    int index;		/* Caller's index of returned info.  This is saved
			 * in the table and used as a hint to clients */
{
    register RpcServerState *srvPtr;	/* Server state that is initialized */
    register Net_ScatterGather *bufferPtr;	/* Tmp pointer to io vector
					 * element.  Some of these are set
					 * up here to reference headers also
					 * kept in the server's state. */
    register int frag;			/* Index into array of headers used
					 * for fragmenting */

    srvPtr = (RpcServerState *)Vm_RawAlloc(sizeof(RpcServerState));

    srvPtr->state = SRV_NOTREADY;
    srvPtr->freeReplyProc = (int (*)())NIL;
    srvPtr->freeReplyData = (ClientData)NIL;
    srvPtr->index = index;
    srvPtr->clientID = -1;
    srvPtr->channel = -1;
    srvPtr->mutex = 0;
    srvPtr->waitCondition.waiting = FALSE;
    /*
     * The sequence number of the client's last request is saved
     * in our reply header.  We initialize it here.
     */
    srvPtr->replyRpcHdr.ID = 0;

    /*
     * Set up the buffer address for the RPC header of replies
     * and acks to point to the headers kept here in the server's state.
     */
    
    bufferPtr = &srvPtr->reply.rpcHdrBuffer;
    bufferPtr->bufAddr = (Address)&srvPtr->replyRpcHdr;
    bufferPtr->length = sizeof(RpcHdr);
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;
    srvPtr->reply.paramBuffer.conditionPtr = (Sync_Condition *)NIL;
    srvPtr->reply.dataBuffer.conditionPtr = (Sync_Condition *)NIL;
    for (frag=0 ; frag < RPC_MAX_NUM_FRAGS ; frag++) {
	bufferPtr = &srvPtr->fragment[frag].rpcHdrBuffer;
	bufferPtr->bufAddr = (Address)&srvPtr->fragRpcHdr[frag];
	bufferPtr->length = sizeof(RpcHdr);
	bufferPtr->conditionPtr = (Sync_Condition *)NIL;
	srvPtr->fragment[frag].paramBuffer.conditionPtr = (Sync_Condition *)NIL;
	srvPtr->fragment[frag].dataBuffer.conditionPtr = (Sync_Condition *)NIL;
    }

    bufferPtr = &srvPtr->ack.rpcHdrBuffer;
    bufferPtr->bufAddr = (Address)&srvPtr->ackRpcHdr;
    bufferPtr->length = sizeof(RpcHdr);
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    bufferPtr = &srvPtr->ack.paramBuffer;
    bufferPtr->bufAddr = (Address)NIL;
    bufferPtr->length = 0;
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    bufferPtr = &srvPtr->ack.dataBuffer;
    bufferPtr->bufAddr = (Address)NIL;
    bufferPtr->length = 0;
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    /*
     * Set up the scatter vector for input requests to the server.
     * Allocate buffer space for the largest possible request.
     */
    
    bufferPtr = &srvPtr->request.rpcHdrBuffer;
    bufferPtr->bufAddr = (Address)&srvPtr->requestRpcHdr;
    bufferPtr->length = sizeof(RpcHdr);
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    bufferPtr = &srvPtr->request.paramBuffer;
    bufferPtr->bufAddr = Vm_RawAlloc(RPC_MAX_PARAM_SIZE);
    bufferPtr->length = RPC_MAX_PARAM_SIZE;
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    bufferPtr = &srvPtr->request.dataBuffer;
    bufferPtr->bufAddr = Vm_RawAlloc(RPC_MAX_DATA_SIZE);
    bufferPtr->length = RPC_MAX_DATA_SIZE;
    bufferPtr->conditionPtr = (Sync_Condition *)NIL;

    /*
     * Initialize temporaries.
     */
    srvPtr->actualParamSize = 0;
    srvPtr->actualDataSize = 0;

    return(srvPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Rpc_Start --
 *
 *      Conduct the preliminary RPC's neccesary to start up the client
 *      side of the RPC system.  A Get Time RPC is done to initialize the
 *      boot time stamp.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Do a Get Time RPC to initialize rpcBootID;
 *
 *----------------------------------------------------------------------
 */
void
Rpc_Start()
{
    Time bootTime;	/* Time returned from the default server */
    int tzMinutes;	/* Minutes west of Greenwich */
    int tzDST;		/* Daylight savings flag */
    ReturnStatus status;	/* Status code from the RPC */
    char dateString[40];/* To hold a printable version of the time */
    Net_EtherAddress etherAddr;
    int seconds;
    int spriteID;

    /*
     * Do a Sprite reverse Arp to discover our Sprite ID.  If it's still
     * zero after this that inhibits the RPC system.  In that case we'd
     * better be a diskfull machine so we find out our SpriteID by
     * the user program that installs routes.  See Net_InstallRoute.
     */
    
    Mach_GetEtherAddress(&etherAddr);
    spriteID = Net_RevArp(&etherAddr);
    if (spriteID > 0) {
	rpc_SpriteID = spriteID;
	Sys_Printf("Reverse Arp, setting Sprite ID to %d\n", spriteID);
    }

    Rpc_StampTest();

    status = Rpc_GetTime(RPC_BROADCAST_SERVER_ID, &bootTime, &tzMinutes,
						 &tzDST);
    if (status != SUCCESS) {
	Timer_Ticks ticks;

	Sys_Printf("Rpc_Start: error (%x) from Get Time RPC\n", status);
	Timer_GetCurrentTicks(&ticks);
	Timer_TicksToTime(ticks, &bootTime);
    } else {
	Timer_SetTimeOfDay(bootTime, tzMinutes, tzDST);
    }
    rpcBootID = bootTime.seconds;

    /*
     * Convert from Greenwich Standard minutes to local minutes
     * and print the time on the console.
     */
    seconds = bootTime.seconds + tzMinutes * 60;
    Time_ToAscii(seconds, FALSE, dateString);
    Sys_Printf("%s\n", dateString);
}

/*
 *----------------------------------------------------------------------
 *
 * Rpc_MaxSizes --
 *
 *      This function returns the maximum amount of data that can be sent
 *      in one RPC.  A remote procedure has its inputs and outputs packed
 *      into two buffers called the "data area" and the "parameter area".
 *      Two values are returned, the maximums for the parameter and data
 *      areas.
 *
 * Results:
 *	The first parameter gets the maximum data size, the
 *	second gets the maximum parameter size.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Rpc_MaxSizes(maxDataSizePtr, maxParamSizePtr)
    int *maxDataSizePtr;
    int *maxParamSizePtr;
{
    if (maxDataSizePtr != (int *)NIL){
	*maxDataSizePtr = RPC_MAX_DATASIZE;
    }
    if (maxParamSizePtr != (int *)NIL){
	*maxParamSizePtr = RPC_MAX_PARAMSIZE;
    }
}
