/* 
 * lfsSegUsage.c --
 *
 *	Routines and data structures providing knowledge more segments 
 *	block usage.  This module managers the segment usage array and
 *	implements the selection of the next segment to write and to
 *	clean.  This module should be notified of all block deallocation
 *	inorder to make intelligent choices.  The module also detects 
 *	"logwrap" and starts with block cleaner and generates "df"
 *	numbers.
 *
 * 	For each LFS, the module classifies segment into one of three classes:
 *	clean, dirty, or full.   A segment is clean if it
 *	contains no live data.  A segment is dirty if it is not clean
 *	and has fewer activeBytes than permitable. A segment is full if
 *	it is neither clean nor dirty.
 *
 * Copyright 1989 Regents of the University of California
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
#endif /* not lint */

#include <lfsInt.h>
#include <lfsSeg.h>
#include <lfsStableMemInt.h>
#include <lfsSegUsageInt.h>

#include <fsdm.h>


/*
 *----------------------------------------------------------------------
 *
 * LfsSegUsageFreeBlocks --
 *
 *	Inform the segment usage manager that blocks are no long needed.
 *
 * Results:
 *	SUCCESS if the blocks are valid.
 *
 * Side effects:
 *	Seg usage map.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
LfsSegUsageFreeBlocks(lfsPtr, blockSize, blockArrayLen, blockArrayPtr)
    Lfs		*lfsPtr;	/* File system of interest. */
    int		blockSize;	/* Size in bytes of blocks to free. */
    int	         blockArrayLen; /* Number of elements in blockArrayPtr. */
    LfsDiskAddr *blockArrayPtr;	        /* Array of disk addresses. */
{
    int		i;
    LfsDiskAddr	diskAddress;

    for (i = 0; i < blockArrayLen; i++) {
	diskAddress = *blockArrayPtr;
	if (!LfsIsNilDiskAddr(diskAddress)) {
	    LfsSetNilDiskAddr(blockArrayPtr);
	    LFS_STATS_INC(lfsPtr->stats.segusage.blocksFreed);
	    LFS_STATS_ADD(lfsPtr->stats.segusage.bytesFreed,  blockSize);
	    LfsSetSegUsage(lfsPtr,
		LfsDiskAddrToSegmentNum(lfsPtr, diskAddress), -blockSize);
	}
	blockArrayPtr++;
    }
    return SUCCESS;
}

/*
 *----------------------------------------------------------------------
 *
 * SegUsageAllocateBytes --
 *
 *	Inform the file system for the need to allocate some bytes.
 *
 * Results:
 *	SUCCESS if the allocation works, failure otherwise.
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
ReturnStatus 
LfsSegUsageAllocateBytes(lfsPtr, numBytes)
       Lfs	*lfsPtr;	/* File system of interest. */
       int	numBytes;       /* Number of file system bytes needed. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);
    int blocks;

    blocks = LfsBytesToBlocks(lfsPtr, numBytes);
    if (cp->freeBlocks - blocks > usagePtr->params.minFreeBlocks) { 
	return SUCCESS;
    }
    return FS_NO_DISK_SPACE;
}

/*
 *----------------------------------------------------------------------
 *
 * SegUsageFreeBytes --
 *
 *	Inform the file system that the previously allocated bytes are 
 *	nolonger needed or have already been allocated on disk.
 *
 * Results:
 *	SUCCESS if the allocation works, failure otherwise.
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
ReturnStatus 
LfsSegUsageFreeBytes(lfsPtr,numBytes)
       Lfs	*lfsPtr;	/* File system of interest. */
       int	numBytes;       /* Number of file system bytes to free. */
{
    return SUCCESS;
}

/*
 *----------------------------------------------------------------------
 *
 * LfsCheckRead --
 *
 *	Check to see if it is ok to read the specified byte range. 
 *
 * Results:
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */
void
LfsCheckRead(lfsPtr,diskAddress, numBytes)
       Lfs	*lfsPtr;	/* File system of interest. */
       LfsDiskAddr   diskAddress;  /* Disk address of read. */
       int	numBytes;          /* Number of bytes being read. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    register LfsSegUsageEntry  *s;
	int segNo, segNo2, blocks;
    ReturnStatus      status;
    LfsDiskAddr newDiskAddr;
    LfsStableMemEntry smemEntry;

    segNo = LfsDiskAddrToSegmentNum(lfsPtr, diskAddress);
    if ((segNo < 0) || (segNo >=  usagePtr->params.numberSegments)) {
	panic("LfsOkToRead bad segment number %d\n", segNo);
	return;
    }
    status = LfsStableMemFetch(&(usagePtr->stableMem),segNo,0,&smemEntry);
    if (status != SUCCESS) {
	panic("LfsOkToRead can't fetch usage array block.\n");
    }
    s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);

    if (s->flags & LFS_SEG_USAGE_CLEAN) {
	panic("LfsOkToRead read from clean segment\n");
	LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, FALSE);
	return;
    }
    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, FALSE);
    blocks = LfsBytesToBlocks(lfsPtr, numBytes)-1;
    LfsDiskAddrPlusOffset(diskAddress, blocks, &newDiskAddr);
    segNo2 = LfsDiskAddrToSegmentNum(lfsPtr, newDiskAddr);
    if (segNo2 != segNo) {
	printf("LfsOkToRead read over segment boundary.\n");
    }

}

/*
 *----------------------------------------------------------------------
 *
 * LfsSetSegUsage --
 *
 *	Set the usage level of the specified segment.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Seg usage array may be modified.
 *
 *----------------------------------------------------------------------
 */

void
LfsSetSegUsage(lfsPtr, segNumber, activeBytes)
    Lfs		*lfsPtr;	/* File system of interest. */
    int		segNumber; 	/* Segment number in file system. */
    int		activeBytes;	/* Usage level Change. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);
    register LfsSegUsageEntry *s;
    ReturnStatus      status;
    LfsStableMemEntry smemEntry;

    LFS_STATS_INC(lfsPtr->stats.segusage.usageSet);
    if ((segNumber < 0) || (segNumber >= usagePtr->params.numberSegments)) {
	panic("LfsSetSegUsage bad segment number %d\n", segNumber);
	return;
    }
    if (activeBytes == 0) {
	printf("LfsSetSegUsage: SegNo %d activeBytes %d\n", segNumber, 
			activeBytes);
    }
    /*
     * We special case the current segment we are writing to.
     */
    if (segNumber == cp->currentSegment) {
	int oldActiveBytes = cp->curSegActiveBytes;
	cp->curSegActiveBytes += activeBytes;
	if (cp->curSegActiveBytes < 0) {
	     printf("LfsSetSegUsage: Warning activeBytes for segment %d is %d\n",
		    segNumber, cp->curSegActiveBytes);
	    cp->curSegActiveBytes = 0;
	}
	cp->freeBlocks += (LfsBytesToBlocks(lfsPtr, oldActiveBytes) - 
			   LfsBytesToBlocks(lfsPtr, cp->curSegActiveBytes));
	return;
    }
    status = LfsStableMemFetch(&(usagePtr->stableMem), segNumber, 
				LFS_STABLE_MEM_MAY_DIRTY, &smemEntry);
    if (status != SUCCESS) {
	panic("LfsSetSegUsage can't fetch usage array block.\n");
	return;
    }
    s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);


    activeBytes = s->activeBytes + activeBytes;
    if (activeBytes < 0) {
	 printf("LfsSetSegUsage: Warning activeBytes for segment %d is %d\n",
		segNumber, activeBytes);
	activeBytes = 0;
    }
    cp->freeBlocks += (LfsBytesToBlocks(lfsPtr, s->activeBytes) - 
				LfsBytesToBlocks(lfsPtr, activeBytes));
    if (s->flags & LFS_SEG_USAGE_CLEAN) {
	panic("LfsSetSegUsage called on a clean segment (%d)\n", segNumber);
	LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, FALSE);
	return;
    }
    /*
     * Is it moving onto dirty list?
     */
    if (activeBytes <= cp->dirtyActiveBytes) {
	if (s->flags & LFS_SEG_USAGE_DIRTY) { 
	    /*
	     * All ready on dirty list then do nothing.
	     */
	    s->activeBytes = activeBytes;
	    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, TRUE);
	    return;
	}
        s->activeBytes = activeBytes;
	s->flags |= LFS_SEG_USAGE_DIRTY;
	cp->numDirty++;
	LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, TRUE);
	return;
    }
    /*
     * Segment is not clean or dirty just full.
     */
    if (s->flags & LFS_SEG_USAGE_DIRTY) { 
	s->flags &= ~LFS_SEG_USAGE_DIRTY;
	cp->numDirty--;
    }
    s->activeBytes = activeBytes;
    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, TRUE);

}

/*
 *----------------------------------------------------------------------
 *
 * LfsMarkSegClean --
 *
 *	Mark the specified segment as clean.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Seg usage array may be modified.
 *
 *----------------------------------------------------------------------
 */

void
LfsMarkSegClean(lfsPtr, segNumber)
    Lfs		*lfsPtr;	/* File system of interest. */
    int		segNumber; 	/* Segment number to mark clean. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);
    register LfsSegUsageEntry  *s;
    ReturnStatus      status;
    LfsStableMemEntry smemEntry;

    if ((segNumber < 0) || (segNumber >= usagePtr->params.numberSegments)) {
	panic("LfsMarkSegClean bad segment number %d\n", segNumber);
	return;
    }

    status = LfsStableMemFetch(&(usagePtr->stableMem), segNumber, 
			LFS_STABLE_MEM_MAY_DIRTY, 
			&smemEntry);
    if (status != SUCCESS) {
	panic("LfsMarkSegClean can't fetch usage array.");
	return;
    }
     s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);

    /*
     * Segment is being marked clean. Remove from dirty list if necessary.
     */
    if (s->flags & LFS_SEG_USAGE_DIRTY) { 
	s->flags &= ~LFS_SEG_USAGE_DIRTY;
	cp->numDirty--;
    }
    if (!(s->flags & LFS_SEG_USAGE_CLEAN)) { 
	cp->numClean++;
	s->flags = LFS_SEG_USAGE_CLEAN;
	cp->freeBlocks += LfsBytesToBlocks(lfsPtr, s->activeBytes);
	s->activeBytes = cp->cleanSegList;
	cp->cleanSegList = segNumber;
    }
    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, TRUE);
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * LfsSetDirtyLevel --
 *
 *	Set the usage level below which a segment is considered dirty.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Seg usage array may be modified.
 *
 *----------------------------------------------------------------------
 */

void
LfsSetDirtyLevel(lfsPtr, dirtyActiveBytes)
    Lfs	*lfsPtr;
    int	 dirtyActiveBytes; /* New level. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);
    register LfsSegUsageEntry  *s;
    register int	segNum;
    ReturnStatus      status;
    LfsStableMemEntry smemEntry;


    cp->dirtyActiveBytes = dirtyActiveBytes;
    for (segNum = 0; segNum < usagePtr->params.numberSegments; segNum++) {
	status = LfsStableMemFetch(&(usagePtr->stableMem), segNum,
			LFS_STABLE_MEM_MAY_DIRTY| 
			  ((segNum > 0) ? LFS_STABLE_MEM_REL_ENTRY : 0), 
			  &smemEntry);

	if (status != SUCCESS) {
	    panic("LfsSetDirtyLevel can't fetch usage array block.\n");
	}
	 s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);
	if (s->activeBytes <= dirtyActiveBytes) {
	    if (s->flags & (LFS_SEG_USAGE_DIRTY|LFS_SEG_USAGE_CLEAN)) { 
		/*
		 * All ready on dirty or clean list then do nothing.
		 */
	    } else { 
		s->flags |= LFS_SEG_USAGE_DIRTY;
		cp->numDirty++;
		LfsStableMemMarkDirty(&smemEntry);
	    }
	} 
    }
    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, FALSE);

}


/*
 *----------------------------------------------------------------------
 *
 * Lfs_DomainInfo --
 *
 *	Return info about the given domain lfsDomain.
 *
 * Results:
 *	Error  if can't get to the domain.
 *
 * Side effects:
 *	The domain info struct is filled in.
 *
 *----------------------------------------------------------------------
 */
ReturnStatus
Lfs_DomainInfo(domainPtr, domainInfoPtr)
    Fsdm_Domain	*domainPtr;
    Fs_DomainInfo	*domainInfoPtr;
{
    Lfs		*lfsPtr;

    lfsPtr = LfsFromDomainPtr(domainPtr);

    domainInfoPtr->maxKbytes = (LfsSegSize(lfsPtr)/1024) *
				lfsPtr->usageArray.params.numberSegments;

    domainInfoPtr->freeKbytes = 
	LfsBlocksToBytes(lfsPtr, lfsPtr->usageArray.checkPoint.freeBlocks)/1024;
    domainInfoPtr->maxFileDesc = lfsPtr->descMap.params.maxDesc;
    domainInfoPtr->freeFileDesc = lfsPtr->descMap.params.maxDesc -
				  lfsPtr->descMap.checkPoint.numAllocDesc;
    domainInfoPtr->blockSize = FS_BLOCK_SIZE;
    domainInfoPtr->optSize = FS_BLOCK_SIZE;


    return(SUCCESS);
}



/*
 *----------------------------------------------------------------------
 *
 * LfsGetLogTail --
 *
 *	Get the next available clean blocks to write the log to.
 *
 * Results:
 *	SUCCESS if log space was retrieved. FS_NO_DISK_SPACE if log
 *	space is not available. FS_WOULD_BLOCK if operation of blocked.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
LfsGetLogTail(lfsPtr, cantWait, logRangePtr, startBlockPtr)
    Lfs	*lfsPtr;	/* File system of interest. */
    Boolean	cantWait;	  /* TRUE if we can't wait for a clean seg. */
    LfsSegLogRange *logRangePtr;  /* Segments numbers returned. */
    int		   *startBlockPtr; /* OUT: Starting offset into segment. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);
    LfsSegUsageEntry *s;
    int		segNumber;
    ReturnStatus      status;
    LfsStableMemEntry smemEntry;


    if (!cantWait && (cp->numClean <= lfsPtr->usageArray.params.minNumClean)) {
	LfsSegCleanStart(lfsPtr);
	return FS_WOULD_BLOCK;
    }
    if (cp->currentBlockOffset != -1) {
	/*
	 * There is still room in the existing segment. Use it.
	 */
	logRangePtr->prevSeg = cp->previousSegment;
	logRangePtr->current = cp->currentSegment;
	logRangePtr->nextSeg =  cp->cleanSegList;
	(*startBlockPtr) = cp->currentBlockOffset;
	return SUCCESS;
    }
    /*
     * Need to location a new segment.
     */
    if (cp->numClean == 0) {
	return FS_NO_DISK_SPACE;
    }
    /*
     * Update the active bytes of the current segment the usage array.
     */
    status = LfsStableMemFetch(&(usagePtr->stableMem), cp->currentSegment, 
			LFS_STABLE_MEM_MAY_DIRTY, &smemEntry);
    if (status == SUCCESS) {
	s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);
	s->activeBytes = cp->curSegActiveBytes;
	if (s->activeBytes <= cp->dirtyActiveBytes) {
	    s->flags |= LFS_SEG_USAGE_DIRTY;
	    cp->numDirty++;
	}
	s->timeOfLastWrite = Fsutil_TimeInSeconds();
	LfsStableMemMarkDirty(&smemEntry);
    } else {
	panic("LfsGetCleanSeg can't fetch usage array.");
    }

    segNumber = cp->cleanSegList;
    status = LfsStableMemFetch(&(usagePtr->stableMem), segNumber, 
			(LFS_STABLE_MEM_MAY_DIRTY| 
			 LFS_STABLE_MEM_REL_ENTRY), &smemEntry);
    if (status != SUCCESS) {
	panic("LfsGetCleanSeg can't fetch usage array.");
	return status;
    }

    s = (LfsSegUsageEntry *) LfsStableMemEntryAddr(&smemEntry);
    cp->cleanSegList = s->activeBytes;

    logRangePtr->prevSeg = cp->previousSegment = cp->currentSegment;
    logRangePtr->current = cp->currentSegment = segNumber;
    logRangePtr->nextSeg =  cp->cleanSegList;

    cp->numClean--;
    s->activeBytes = cp->curSegActiveBytes = 0;
    if (cp->numClean <= lfsPtr->usageArray.params.minNumClean) {
	LfsSegCleanStart(lfsPtr);
    }
    s->flags  &= ~LFS_SEG_USAGE_CLEAN;
    LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, TRUE);
    (*startBlockPtr) = 0;
    return SUCCESS;
}

/*
 *----------------------------------------------------------------------
 *
 * LfsSetLogTail --
 *
 *	Set the next available clean blocks to write the log to.
 *
 * Results:
 *	None
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

void
LfsSetLogTail(lfsPtr, logRangePtr, startBlock, activeBytes)
    Lfs	*lfsPtr;	/* File system of interest. */
    LfsSegLogRange *logRangePtr;  /* Segments numbers returned. */
    int	startBlock; /* Starting offset into segment. */
    int	activeBytes;	/* Number of bytes written. */
{
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = &(usagePtr->checkPoint);

    cp->currentBlockOffset = startBlock;
    if (activeBytes > 0) {
	LfsSetSegUsage(lfsPtr, logRangePtr->current, activeBytes);
   }
}

/*
 *----------------------------------------------------------------------
 *
 * LfsGetSegsToClean --
 *
 *	Return a set of segments to clean.
 *
 * Results:
 *	Number of segments returned.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

int
LfsGetSegsToClean(lfsPtr, maxBlocks, maxSegArrayLen, segArrayPtr)
    Lfs	  *lfsPtr;	/* File system of interest. */
    int	  maxBlocks;	/* Maximum number of file system blocks to clean. */
    int	  maxSegArrayLen; 	/* The maximum number of segment to clean to 
				 * return. */
    LfsSegList	 *segArrayPtr;	/* Array of length maxSegArrayLen to return
				 * segments to clean. */
{
    int	numberSegs, segNum, numBlocks, blockSize;
    Boolean fullClean;
    int i, j;
    LfsSegUsageEntry *s;
    LfsSegUsage *usagePtr = &(lfsPtr->usageArray);
    ReturnStatus      status;
    LfsStableMemEntry smemEntry;

    numberSegs = 0;
    blockSize = LfsBlockSize(lfsPtr);
    /*
     * For each  segment.
     */
   for (segNum = 0; segNum < usagePtr->params.numberSegments; segNum++) {
	status = LfsStableMemFetch(&(usagePtr->stableMem), segNum, 
			     LFS_STABLE_MEM_MAY_DIRTY| 
			  ((segNum > 0) ? LFS_STABLE_MEM_REL_ENTRY : 0), 
			    &smemEntry);
	if (status != SUCCESS) {
	    panic("LfsSetDirtyLevel can't fetch usage array block.\n");
	    return status;
	}
	/*
	 * Execpt the one currently being written.
	 */
	if (usagePtr->checkPoint.currentSegment != segNum) { 
	    /*
	     * Find the proper position in the list for this segment.
	     */
	    s = (LfsSegUsageEntry *)LfsStableMemEntryAddr(&smemEntry);

	    if (s->flags & LFS_SEG_USAGE_DIRTY) {
		for (i = numberSegs-1; i >= 0; i--) {
		    if (segArrayPtr[i].activeBytes < s->activeBytes) {
			break;
		    } 
		}
		/*
		 * Insert it at that position by moving all others down. 
		 * Extend the array if it is not full already.
		 */
		if (numberSegs < maxSegArrayLen) {
		    numberSegs++;
		}
		for (j = numberSegs-2; j > i; j--) {
		    segArrayPtr[j+1] = segArrayPtr[j];
		}
		segArrayPtr[i+1].segNumber = segNum;
		segArrayPtr[i+1].activeBytes = s->activeBytes;
	    } 
	}
   }
   LfsStableMemRelease(&(usagePtr->stableMem), &smemEntry, FALSE);
   numBlocks = 0;
   fullClean = ((lfsPtr->controlFlags & LFS_CONTROL_CLEANALL) != 0);
   for (i = 0; i < numberSegs; i++) {
       /*
        * If we aren't doing a full clean and the segment is not empty, 
	* we stop cleaning early if an enough segments are clean.
	*/
	if (!fullClean && (segArrayPtr[i].activeBytes > 0) &&
	    (usagePtr->checkPoint.numClean+i >
		   usagePtr->params.minNumClean + 
		       usagePtr->params.numSegsToClean)) {
	    return i;
	}
	numBlocks +=  LfsBytesToBlocks(lfsPtr, segArrayPtr[i].activeBytes);
	if (numBlocks > maxBlocks) {
	    return i;
	}
   }
   return numberSegs;
}


/*
 *----------------------------------------------------------------------
 *
 * LfsSegUsageCheckpointUpdate --
 *
 *	This routine is used to update fields of the seg usage 
 *	checkpoint that change when the checkpoint itself is 
 *	written to the log.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
LfsSegUsageCheckpointUpdate(lfsPtr, checkPointPtr, size)
    Lfs	 *lfsPtr;	 /* File system being checkpointed. */
    char *checkPointPtr; /* Checkpoint region for SegUsage. */
    int	 size;		 /* Size of checkpoint region. */
{
    LfsSegUsage	      *usagePtr = &(lfsPtr->usageArray);

    if (size < sizeof(LfsSegUsageCheckPoint)) {
	panic("LfsSegUsageCheckpointUpdate bad checkpoint size.\n");
    }
    (*(LfsSegUsageCheckPoint *) checkPointPtr) = usagePtr->checkPoint;
    return;
}


extern ReturnStatus LfsSegUsageAttach _ARGS_((Lfs *lfsPtr, 
			int checkPointSize, char *checkPointPtr));
extern Boolean LfsSegUsageCheckpoint _ARGS_((LfsSeg *segPtr, int flags, 
			char *checkPointPtr, int *checkPointSizePtr, 
			ClientData *clientDataPtr));
extern void LfsSegUsageWriteDone _ARGS_((LfsSeg *segPtr, int flags, 
			ClientData *clientDataPtr));
extern Boolean LfsSegUsageClean _ARGS_((LfsSeg *segPtr, int *sizePtr,
			int *numCacheBlocksPtr, ClientData *clientDataPtr));
extern Boolean LfsSegUsageLayout _ARGS_((LfsSeg *segPtr, int flags, 
			ClientData *clientDataPtr));


static LfsSegIoInterface segUsageIoInterface = 
	{ LfsSegUsageAttach, LfsSegUsageLayout, LfsSegUsageClean,
	  LfsSegUsageCheckpoint, LfsSegUsageWriteDone,  0};


/*
 *----------------------------------------------------------------------
 *
 * LfsSegUsageInit --
 *
 *	Initialize the segment usage array  data structures.  
 *
 * Results:
 *	None
 *	
 * Side effects:
 *
 *----------------------------------------------------------------------
 */

void
LfsSegUsageInit()
{
    LfsSegIoRegister(LFS_SEG_USAGE_MOD,&segUsageIoInterface);
}


/*
 *----------------------------------------------------------------------
 *
 * SegUsageAttach --
 *
 *	Attach routine for the descriptor map. Creates and initializes the
 *	map for this file system.
 *
 * Results:
 *	SUCCESS if attaching is going ok.
 *
 * Side effects:
 *	Many
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
LfsSegUsageAttach(lfsPtr, checkPointSize, checkPointPtr)
    Lfs   *lfsPtr;	     /* File system for attach. */
    int   checkPointSize;    /* Size of checkpoint data. */
    char  *checkPointPtr;     /* Data from last checkpoint before shutdown. */
{
    LfsSegUsage	      *usagePtr = &(lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = (LfsSegUsageCheckPoint *) checkPointPtr;
    ReturnStatus status;

    /*
     * Allocate and fill in memory data structure for descriptor map.
     */
    usagePtr->params = lfsPtr->superBlock.usageArray;
    usagePtr->checkPoint = *cp;
    /*
     * Load the stableMem and buffer using the LfsStableMem routines.
     */
    status = LfsStableMemLoad(lfsPtr, &(usagePtr->params.stableMem), 
			checkPointSize - sizeof(LfsSegUsageCheckPoint), 
			checkPointPtr + sizeof(LfsSegUsageCheckPoint), 
			&(usagePtr->stableMem));
    if (status != SUCCESS) {
	LfsError(lfsPtr, status,"Can't loading descriptor map stableMem\n");
	return status;
    }
    printf("LfsSegUsageAttach - logEnd <%d,%d>, numClean %d numDirty %d numFull %d\n",
		cp->currentSegment, cp->currentBlockOffset,
		cp->numClean, cp->numDirty,
		usagePtr->params.numberSegments - cp->numClean - cp->numDirty);
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * SegUsageCheckpoint --
 *
 *	Routine to handle checkpointing of the descriptor map data.
 *
 * Results:
 *	TRUE if more data needs to be written, FALSE if this module is
 *	checkpointed.
 *
 * Side effects:
 *	Many
 *
 *----------------------------------------------------------------------
 */

Boolean
LfsSegUsageCheckpoint(segPtr, flags,checkPointPtr, checkPointSizePtr, 
			clientDataPtr)
    LfsSeg *segPtr;		/* Segment containing data for checkpoint. */
    char   *checkPointPtr;      /* Buffer to write checkpoint data. */
    int	   flags;		/* Flags. */
    int	   *checkPointSizePtr;  /* Bytes added to the checkpoint area.*/
    ClientData *clientDataPtr;
{
    LfsSegUsage	      *usagePtr = &(segPtr->lfsPtr->usageArray);
    LfsSegUsageCheckPoint *cp = (LfsSegUsageCheckPoint *) checkPointPtr;
    int		size;
    Boolean	full;

    *cp = usagePtr->checkPoint;
    size = sizeof(LfsSegUsageCheckPoint);

    full = LfsStableMemCheckpoint(segPtr, checkPointPtr + size, flags,
		checkPointSizePtr, clientDataPtr,
		&(usagePtr->stableMem));
    if (!full) { 
	*checkPointSizePtr = (*checkPointSizePtr) + size;
    }
    return full;

}

/*
 *----------------------------------------------------------------------
 *
 * SegUsageWriteDone --
 *
 *	Routine to handle finishing of a checkpoint.
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Many
 *
 *----------------------------------------------------------------------
 */

void
LfsSegUsageWriteDone(segPtr, flags, clientDataPtr)
    LfsSeg *segPtr;		/* Segment containing data for checkpoint. */
    int	   flags;		/* Flags for checkpoint */
    ClientData *clientDataPtr;
{
    LfsSegUsage	      *usagePtr = &(segPtr->lfsPtr->usageArray);

    LFS_STATS_ADD(segPtr->lfsPtr->stats.segusage.blocksWritten, 
		(LfsSegSummaryBytesLeft(segPtr) / sizeof(int)));
    LfsStableMemWriteDone(segPtr, flags, clientDataPtr, 
			  &(usagePtr->stableMem));
    return;

}


/*
 *----------------------------------------------------------------------
 *
 * LfsSegUsageClean --
 *
 *	Routine to handle cleaning of descriptor map data.
 *
 * Results:
 *	TRUE if more data needs to be written, FALSE if this module is
 *	happy for the time being.
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */

Boolean
LfsSegUsageClean(segPtr, sizePtr, numCacheBlocksPtr, clientDataPtr)
    LfsSeg *segPtr;	/* Segment containing data to clean. */
    int *sizePtr;		/* Size of cleaning. */
    int *numCacheBlocksPtr;
    ClientData *clientDataPtr;
{
    LfsSegUsage	      *usagePtr = &(segPtr->lfsPtr->usageArray);
    Boolean	full;

    full =  LfsStableMemClean(segPtr, sizePtr, numCacheBlocksPtr, clientDataPtr,
			&(usagePtr->stableMem));

    LFS_STATS_ADD(segPtr->lfsPtr->stats.segusage.blocksCleaned, 
		*sizePtr/usagePtr->stableMem.params.blockSize);
    return full;

}



/*
 *----------------------------------------------------------------------
 *
 * LfsSegUsageLayout --
 *
*	Routine to handle layingout of segUsage data.
 *
 * Results:
 *	TRUE if more data needs to be written, FALSE if this module is
 *	happy for the time being.
 *
 * Side effects:
 *	
 *
 *----------------------------------------------------------------------
 */

Boolean
LfsSegUsageLayout(segPtr, flags, clientDataPtr)
    LfsSeg *segPtr;	/* Segment containing data to clean. */
    int flags; /* Layout flags. */
    ClientData *clientDataPtr;
{
    LfsSegUsage	      *usagePtr = &(segPtr->lfsPtr->usageArray);

    if ((flags & LFS_CLEANING_LAYOUT) != 0) {
	return FALSE;
    }
    return  LfsStableMemLayout(segPtr, flags, 
				clientDataPtr, &(usagePtr->stableMem));
}

