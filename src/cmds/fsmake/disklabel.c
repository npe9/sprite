/* 
 * disklabel.c --
 *
 *	Contains routines for modifying a disk's label..
 *
 * Copyright 1990 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header: /sprite/src/admin/fsmake/RCS/disklabel.c,v 1.2 90/10/10 16:00:33 rab Exp Locker: shirriff $ SPRITE (Berkeley)";
#endif /* not lint */

#include "fsmake.h"

/*
 * The 8 partitions, a through h.
 */
#define	A_PART	0
#define B_PART	1
#define C_PART	2
#define D_PART	3
#define E_PART	4
#define F_PART	5
#define G_PART	6
#define H_PART	7

static ReturnStatus ScanDisktab();


/*
 *----------------------------------------------------------------------
 *
 * Reconfig --
 *
 *	Changes the configuration information of the disk. In
 *	particular, the number of heads, sectors per track, and
 *	cylinders may be changed.  If the "disktab" parameter is
 *	true then the information is read from the disktab file.
 *	Otherwise the disk is probed for its size and a
 *	configuration is created that minimizes the amount of
 *	wasted disk space.
 *
 * Results:
 *	SUCCESS if the label was changed properly, FAILURE otherwise
 *
 * Side effects:
 *	The disk configuration info in the label may be changed.
 *	If it is changed then the partition map is zeroed.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Reconfig(fid, disktab, disktabName, diskType, labelType, scsiDisk, labelPtrPtr)
    int			fid;		/* Handle for first partition of disk*/
    Boolean		disktab;	/* TRUE => use disktab file. */
    char		*disktabName;	/* Name of disktab file. */
    char		*diskType;	/* Type of disk. */
    Disk_NativeLabelType labelType;	/* Type of label to write. */
    int			scsiDisk;	/* TRUE => it's a scsi disk. */
    Disk_Label		**labelPtrPtr;	/* Place to return label ptr. */
{
    Disk_Label		*labelPtr;
    ReturnStatus	status = SUCCESS;
    labelPtr = *labelPtrPtr;
    if (labelPtr == NULL) {
	labelPtr = Disk_ReadLabel(fid);
	if (labelPtr != NULL) {
	    if ((labelType != DISK_NO_LABEL) && 
	        (labelType != labelPtr->labelType) && (!printOnly)) {
		Disk_Label	*newLabelPtr;
		status = Disk_EraseLabel(fid, labelPtr->labelType);
		if (status != SUCCESS) {
		    printf("Couldn't erase old label.\n");
		    return status;
		}
		newLabelPtr = Disk_NewLabel(labelType);
		newLabelPtr->numHeads = labelPtr->numHeads;
		newLabelPtr->numSectors = labelPtr->numSectors;
		newLabelPtr->numCylinders = labelPtr->numCylinders;
		newLabelPtr->numAltCylinders = labelPtr->numAltCylinders;
		bcopy(labelPtr->asciiLabel, newLabelPtr->asciiLabel, 
		    labelPtr->asciiLabelLen);
		bcopy((char *) labelPtr->partitions, 
		      (char *) newLabelPtr->partitions,
		      sizeof(labelPtr->partitions));
		labelPtr = newLabelPtr;
	    }
	} else {
	    if (labelType == DISK_NO_LABEL) {
		printf("Disk does not have a label. Use -labeltype\n");
		return FAILURE;
	    }
	    labelPtr = Disk_NewLabel(labelType);
	    if (labelPtr == NULL) {
		return FAILURE;
	    }
	}
	*labelPtrPtr = labelPtr;
    }
    if (disktab) {
	status = ScanDisktab(disktabName, TRUE, diskType, labelPtr);
    } else if (!scsiDisk) {
	printf("You must specify the -configdisktab for a non-scsi disk.\n");
	return FAILURE;
    } else {
#ifdef sprite
	status = InventConfig(fid, labelPtr);
#else
	printf("You must use the -configdisktab option on unix.\n");
	return FAILURE;
#endif
    }
    if (status != SUCCESS) {
	fprintf(stderr, "Unable to reconfigure disk\n");
    } else {
	int i;
	/*
	 * Clear the partition map since we've changed the disk
	 * geometry.
	 */
	for (i = 0; i < DISK_MAX_PARTS; i++) {
	    labelPtr->partitions[i].firstCylinder = 0;
	    labelPtr->partitions[i].numCylinders = 0;
	}
    }
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Repartition --
 *
 *	Repartitions the disk. If the disktab variable is TRUE then
 *	all partitions are set from the disktab file, otherwise
 *	a single partition is changed.
 *
 * Results:
 *	SUCCESS if the partition map was changed properly, 
 *	FAILURE otherwise
 *
 * Side effects:
 *	The label on the disk is changed.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
Repartition(fid, disktab, disktabName, diskType, labelType, partition,
    sizes, labelPtrPtr)
    int			fid;		/* Handle for first partition of disk*/
    Boolean		disktab;	/* TRUE => use disktab file. */
    char		*disktabName;	/* Name of disktab file. */
    char		*diskType;	/* Type of disk. */
    Disk_NativeLabelType labelType;	/* Type of label to write. */
    int			partition;	/* Which parition to change. */
    int			*sizes;		/* Sizes of partitions as a pct.*/
    Disk_Label		**labelPtrPtr;	/* Place to return label ptr. */

{
   Disk_Label		*labelPtr = *labelPtrPtr;
   ReturnStatus		status = SUCCESS;
   int			adjust;

   if (labelPtr == NULL) {
	labelPtr = Disk_ReadLabel(fid);
	if (labelPtr != NULL) {
	    if ((labelType != DISK_NO_LABEL) && 
	        (labelType != labelPtr->labelType) && (!printOnly)) {
		status = Disk_EraseLabel(fid, labelPtr->labelType);
		if (status != SUCCESS) {
		    fprintf(stderr, "Can't erase disk label.\n");
		    return status;
		}
		labelPtr->labelType = labelType;
	    }
	} else {
	    if (labelType == DISK_NO_LABEL) {
		printf("Disk does not have a label. Use -labeltype\n");
		return FAILURE;
	    }
	    labelPtr = Disk_NewLabel(labelType);
	    if (labelPtr == NULL) {
		fprintf(stderr, "The disk does not have a label.\n");
		return FAILURE;
	    }
	}
	*labelPtrPtr = labelPtr;
    }
    if (disktab) {
	status = ScanDisktab(disktabName, FALSE, diskType, labelPtr);
    } else {
	int		firstCylinder;
	int		numCylinders;
	int		i;
	Disk_Partition	*partitions = labelPtr->partitions;
	for (i = 0; i < 7; i++) {
	    if (sizes[i] == 0) {
		continue;
	    }
	    adjust = labelPtr->numCylinders * sizes[i] / 100;
	    /*
	     * Adjust the start and size of the current partition, and the
	     * start of all partitions that follow.
	     */
	    switch(i) {
		case A_PART:
		    firstCylinder = 0;
		    partitions[B_PART].firstCylinder += adjust;
		    partitions[D_PART].firstCylinder += adjust;
		    partitions[E_PART].firstCylinder += adjust;
		    partitions[F_PART].firstCylinder += adjust;
		    partitions[G_PART].firstCylinder += adjust;
		    break;
		case B_PART:
		    firstCylinder = partitions[A_PART].firstCylinder +
				    partitions[A_PART].numCylinders;
		    partitions[D_PART].firstCylinder += adjust;
		    partitions[E_PART].firstCylinder += adjust;
		    partitions[F_PART].firstCylinder += adjust;
		    partitions[G_PART].firstCylinder += adjust;
		    break;
		case C_PART:
		    firstCylinder = 0;
		    break;
		case D_PART:
		    partitions[E_PART].firstCylinder += adjust;
		    partitions[F_PART].firstCylinder += adjust;
		case G_PART:
		    firstCylinder = partitions[A_PART].firstCylinder +
				    partitions[A_PART].numCylinders;
		    break;
		case E_PART:
		    firstCylinder = partitions[D_PART].firstCylinder +
				    partitions[D_PART].numCylinders;
		    partitions[F_PART].firstCylinder += adjust;
		    break;
		case F_PART:
		    firstCylinder = partitions[E_PART].firstCylinder +
				    partitions[E_PART].numCylinders;
		    break;
		default:
		    fprintf(stderr, "Invalid partition %d\n", i);
		    return FAILURE;
	    }
	    partitions[i].firstCylinder = firstCylinder;
	    numCylinders = labelPtr->numCylinders * sizes[i] / 100;
	    partitions[i].numCylinders = numCylinders;
	}
    }
    return status;
}


#define	BUF_SIZE	256

/*
 *----------------------------------------------------------------------
 *
 * ScanDisktab --
 *
 *	Initialize the disk label by reading the information from
 * 	the disktab file.
 *
 * Results:
 *	SUCCESS if the label was initialized properly, FAILURE otherwise
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static ReturnStatus
ScanDisktab(fileName, reconfig, diskType, labelPtr)
    char	*fileName;	/* Name of the disktab file. */
    Boolean	reconfig;	/* Reconfigure disk */
    char	*diskType;	/* Type of disk. */
    Disk_Label	*labelPtr;	/* The disk label. */
{
    char		buf[BUF_SIZE];
    char		fullBuf[BUF_SIZE];
    char		*bufPtr;
    int			fullBufLen;
    int			sectorsPerTrack;
    int			tracksPerCylinder;
    int			sectorsPerCylinder;
    int			numCylinders;
    FILE		*fp;
    Disk_Partition  	*partitions;
    Disk_Partition  	dummy[DISK_MAX_PARTS];
    int			len;
    int			i;

    if (diskType == NULL) {
	printf(
	"You must use the -disktype option when using the disktab file.\n");
	return FAILURE;
    }
    fp = fopen(fileName, "r");
    if (fp == NULL) {
	fprintf(stderr,"Can't open %s", fileName);
	exit(1);
    }
    len = strlen(diskType);
    /*
     * Scan until we reach a line that contains the disk type in it.
     */
    while (fgets(buf, BUF_SIZE, fp) != NULL) {
	if (strncmp(diskType, buf, len) == 0 &&
	    buf[len] == '|') {
	    /*
	     * We found the disk type.
	     */
	    break;
	}
    }
    if (strncmp(diskType, buf, len) != 0) {
	fprintf(stderr, "`%s' not in disktab\n", diskType);
	fprintf(stderr, "Note: type must be first entry on line\n");
	return FAILURE;
    }

    fullBufLen = 0;
    /*
     * Now cram all of the lines that end in "\" together.
     */
    while (1) {
	for (bufPtr = buf; 
	     *bufPtr != '\n' && *bufPtr != '\\' && (bufPtr - buf < sizeof(buf));
	     bufPtr++) {
	    if (*bufPtr != ' ' && *bufPtr != '\t') {
		if (fullBufLen == sizeof(fullBuf)) {
		    break;
		}
		fullBuf[fullBufLen] = *bufPtr;
		fullBufLen++;
	    }
	}
	if (bufPtr - buf == sizeof(buf) || fullBufLen == sizeof(fullBuf)) {
	    printf("Buffer overflow.\n");
	    return FAILURE;
	}
	if (*bufPtr == '\n') {
	    fullBuf[fullBufLen] = 0;
	    break;
	}
	if (fgets(buf, BUF_SIZE, fp) == NULL) {
	    fprintf(stderr, "Premature EOF\n");
	    exit(1);
	}
    }
    if (reconfig) {
	partitions = dummy;
    } else {
	partitions = labelPtr->partitions;
    }
    /*
     * Now build up a partition table.
     */
    for (i = 0; i < DISK_MAX_PARTS; i++) {
	partitions[i].firstCylinder = 0;
	partitions[i].numCylinders = 0;
    }
    for (bufPtr = fullBuf; *bufPtr != 0; bufPtr++) {
	int	partition;

	if (strncmp(bufPtr, ":ns#", 4) == 0) {
	    bufPtr += 4;
	    sscanf(bufPtr, "%d", &sectorsPerTrack);
	} else if (strncmp(bufPtr, ":nt#", 4) == 0) {
	    bufPtr += 4;
	    sscanf(bufPtr, "%d", &tracksPerCylinder);
	} else if (strncmp(bufPtr, ":nc#", 4) == 0) {
	    bufPtr += 4;
	    sscanf(bufPtr, "%d", &numCylinders);
	} else if (strncmp(bufPtr, ":p", 2) == 0) {
	    /*
	     * Skip past the ":p".
	     */
	    bufPtr += 2;
	    partition = *bufPtr - 'a';
	    /*
	     * Skip past the partition character and the #.
	     */
	    bufPtr += 2;
	    sscanf(bufPtr, "%d", &partitions[partition].numCylinders);
	}
    }
    if (reconfig) {
	strcpy(labelPtr->asciiLabel, diskType);
	labelPtr->numHeads = tracksPerCylinder;
	labelPtr->numSectors = sectorsPerTrack;
	labelPtr->numCylinders = numCylinders;
	return SUCCESS;
    } else {
	tracksPerCylinder = labelPtr->numHeads;
	sectorsPerTrack = labelPtr->numSectors;
	numCylinders = labelPtr->numCylinders;
    }
    /*
     * The partition sizes in the disktab file are in terms of sectors.
     * Convert this to cylinders.
     */
    sectorsPerCylinder = sectorsPerTrack * tracksPerCylinder;
    for (i = 0; i < FSDM_NUM_DISK_PARTS; i++) {
	partitions[i].numCylinders /= sectorsPerCylinder;
    }
    /*
     * Now that we've built up the number of cylinders build up the
     * cylinder offsets.
     */
    partitions[A_PART].firstCylinder = 0;
    partitions[B_PART].firstCylinder = partitions[A_PART].numCylinders;
    partitions[C_PART].firstCylinder = 0;
    partitions[D_PART].firstCylinder = partitions[B_PART].firstCylinder + 
			partitions[B_PART].numCylinders;
    partitions[E_PART].firstCylinder = partitions[D_PART].firstCylinder + 
			partitions[D_PART].numCylinders;
    partitions[F_PART].firstCylinder = partitions[E_PART].firstCylinder + 
			partitions[E_PART].numCylinders;
    partitions[F_PART].numCylinders = 
			numCylinders - (partitions[E_PART].firstCylinder +
					partitions[E_PART].numCylinders);
    partitions[G_PART].firstCylinder = partitions[B_PART].firstCylinder + 
			partitions[B_PART].numCylinders;
    partitions[G_PART].numCylinders = 
			numCylinders - (partitions[B_PART].firstCylinder +
					partitions[B_PART].numCylinders);

    return SUCCESS;
}

/*
 *----------------------------------------------------------------------
 *
 * InventConfig --
 *
 *	Invents a configuration for the disk that maximizes the
 *	amount of available space.  This only works if the disk
 *	is a scsi disk.
 *
 * Results:
 *	Pointer to the label.
 *
 * Side effects:
 *	A scsi command is sent to the disk.
 *
 *----------------------------------------------------------------------
 */

#ifdef sprite 
ReturnStatus
InventConfig(fid, labelPtr)
    int		fid;		/* Handle on the raw disk. */
    Disk_Label	*labelPtr;	/* The disk label. */
{
    ScsiCmd		cmd;		
    CmdStatus		cmdStatus;
    struct stat 	statbuf;
    int			numSectors;
    int			size;
    ReturnStatus	status = SUCCESS;
    int			sectorsPerBlock;
    int			lowerLimit;
    int			upperLimit;
    int			best;
    int			leastWasted;
    int			i;

    /*
     * Send a Read Capacity SCSI command to the disk to find out how many
     * sectors there are and how many bytes per sector.
     */
    bzero((char *) &cmd, sizeof(cmd));;
    cmd.hdr.bufferLen = sizeof(ReadCapacityCommand);
    cmd.hdr.commandLen = sizeof(ReadCapacityCommand);
    cmd.hdr.dataOffset = sizeof(Dev_ScsiCommand)+sizeof(ReadCapacityCommand);
    cmd.cmd.command = 0x25;
    fstat(fid, &statbuf);
    cmd.cmd.lun = (statbuf.st_rdev >> 7) & 0x7;
    cmd.cmd.pmi = 0;
    status = Fs_IOControl(fid, IOC_SCSI_COMMAND, cmd.hdr.dataOffset,
			(char *) &cmd, sizeof(CmdStatus), (char *) &cmdStatus);
    if (status != 0) {
	fprintf(stderr,"Fs_IoControl returned status 0x%x : %s\n",status,
			Stat_GetMsg(status));
	exit(1);
    }
    numSectors = ((unsigned int)cmdStatus.info.result.addr3 << 24 | 
		(unsigned int)cmdStatus.info.result.addr2 << 16 |
		(unsigned int)cmdStatus.info.result.addr1 << 8 |
		(unsigned int)cmdStatus.info.result.addr0) + 1;
    size = (unsigned int)cmdStatus.info.result.size3 << 24 | 
		(unsigned int)cmdStatus.info.result.size2 << 16 |
		(unsigned int)cmdStatus.info.result.size1 << 8 |
		(unsigned int)cmdStatus.info.result.size0;
    if (size != 512) {
	fprintf(stderr, "Whoa!  There are %d bytes in a sector!!!\n", size);
	return FAILURE;
    }
    /*
     * Now compute a "good" number of sectors per cylinder so that
     * we waste as few as possible.  Remember that file system blocks
     * cannot cross cylinder boundaries, and that the file system uses
     * cylinders to localize the allocation of files. Therefore we
     * don't want cylinders to be too big or too small. 
     * I've arbitrarily
     * chosen a lower limit of 16 blocks (64 K) and 
     * picked an upper limit of 256 blocks (1 Mb).
     * We also don't want too many cylinders since the filesystem
     * does stuff on a cylinder basis.  The upper limit on the
     * number of cylinders is 3000.
     *
     * The computation is brute force.  I don't know of a closed form
     * solution.
     */
    sectorsPerBlock = FS_BLOCK_SIZE / size;
    lowerLimit = sectorsPerBlock * 16;
    upperLimit = sectorsPerBlock * 256;
    if (numSectors / lowerLimit > 3000) {
	lowerLimit = numSectors / 3000;
	if (lowerLimit % sectorsPerBlock != 0) {
	    lowerLimit += sectorsPerBlock - (lowerLimit % sectorsPerBlock);
	}
    }
    best = 0;
    leastWasted = upperLimit;
    /*
     * Iterate over all sizes of cylinders such that a whole number
     * of blocks fit in a cylinder.
     */
    for (i = lowerLimit; i <= upperLimit; i+=sectorsPerBlock) {
	int left;

	/*
	 * Compute number of leftover sectors because the disk size
	 * is not a multiple of the cylinder size, and due to the
	 * one cylinder that we'll throw away at the start of the disk
	 * for the boot sectors and stuff.
	 */
	left = numSectors % i + i;
	if (left < leastWasted) {
	    leastWasted = left;
	    best = i;
	}
    }
    labelPtr->numHeads = 1;
    labelPtr->numSectors = best;
    labelPtr->numCylinders = numSectors / best;
    return status;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * ConfirmDiskSize --
 *
 *	Checks that the the last sector of the disk as computed from
 *	the information in the label actually exists.
 *
 * Results:
 *	SUCCESS if the sector can be read, FAILURE otherwise
 *
 * Side effects:
 *	A sector is read from the disk.
 *
 *----------------------------------------------------------------------
 */

ReturnStatus
ConfirmDiskSize(fid, labelPtr, sizes)
    int		fid;		/* Handle on the raw disk. */
    Disk_Label	*labelPtr;	/* The disk label. */
    int		*sizes;		/* Size of partitions. */
{
    int 		lastSector;
    int			status;
    static char		buffer[DEV_BYTES_PER_SECTOR];
    ReturnStatus	returnStatus = SUCCESS;
    int			i;

    lastSector = labelPtr->numCylinders * labelPtr->numHeads *
		    labelPtr->numSectors - 1;
    status = Disk_SectorRead(fid, lastSector, 1, buffer);
    if (status != 0) {
	fprintf(stderr, "Can't read last sector (%d) of disk.\n", lastSector);
	fprintf(stderr, "Disk reconfiguration/repartition failed.\n");
	return FAILURE;
    }
    for (i = 0; i < 7; i++) {
	/*
	 * Ignore partitions we haven't modified.
	 */
	if (sizes[i] == 0) {
	    continue;
	}
	lastSector = (labelPtr->partitions[i].firstCylinder  +
		     labelPtr->partitions[i].numCylinders) *
		     labelPtr->numHeads * labelPtr->numSectors - 1;
	if (lastSector > 0) {
	    status = Disk_SectorRead(fid, lastSector, 1, buffer);
	    if (status != 0) {
		fprintf(stderr, 
		    "Can't read last sector (%d) of the '%c' partition\n", 
		    lastSector, ((char) i) + 'a');
		returnStatus = FAILURE;
	    }
	}
    }
    return returnStatus;
}

