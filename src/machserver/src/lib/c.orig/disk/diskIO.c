/* 
 * diskIO.c --
 *
 *	Routines to do I/O to a raw disk.
 *
 * Copyright (C) 1987 Regents of the University of California
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
static char rcsid[] = "$Header: /sprite/src/lib/c/disk/RCS/diskIO.c,v 1.8 90/03/16 17:41:15 jhh Exp Locker: mendel $ SPRITE (Berkeley)";
#endif not lint

#include "disk.h"
#include <stdio.h>
#include <errno.h>
#include <sys/file.h>


/*
 *----------------------------------------------------------------------
 *
 * Disk_SectorRead --
 *	Read sectors from the disk file at a specified offset.  This combines
 *	an Ioc_Reposition with the read.
 *
 * Results:
 *	0 if could read the sector, -1 if could not.  If couldn't read
 *	the disk then the error is stored in errno.
 *
 * Side effects:
 *	Reposition the disk file's stream pointer and *buffer filled
 *	with data from the disk.
 *
 *----------------------------------------------------------------------
 */
int
Disk_SectorRead(openFileID, firstSector, numSectors, buffer)
    int		openFileID;	/* Handle on raw disk */
    int		firstSector;	/* First sector to read */
    int		numSectors;	/* The number of sectors to read */
    Address	buffer;		/* The buffer to read into */
{
    int amountRead;

    if (lseek(openFileID, (long) (firstSector * DEV_BYTES_PER_SECTOR), L_SET) 
	< (long) 0) {
	perror("Disk_SectorRead: lseek failed");
	return(-1);
    }
    amountRead = read(openFileID, buffer, DEV_BYTES_PER_SECTOR * numSectors);
    if (amountRead < 0) {
	perror("Disk_SectorRead: read failed");
	return(-1);
    } else if (amountRead != DEV_BYTES_PER_SECTOR * numSectors) {
	fprintf(stderr, "Disk_SectorRead: short read, %d sectors, not %d\n",
		       amountRead / DEV_BYTES_PER_SECTOR, numSectors);
	errno = 0;
	return(-1);
    }
    return(0);
}

/*
 *----------------------------------------------------------------------
 *
 * Disk_SectorWrite --
 *	Write sectors to the disk file at a specified offset.  This combines
 *	an Ioc_Reposition with the write.
 *
 * Results:
 *	0 if could write the disk, -1 if could not.  If couldn't read the
 *	disk then the error number is stored in errno.
 *
 * Side effects:
 *	The write.
 *
 *----------------------------------------------------------------------
 */
int
Disk_SectorWrite(openFileID, firstSector, numSectors, buffer)
    int		openFileID;	/* Handle on raw disk */
    int		firstSector;	/* First sector to read */
    int		numSectors;	/* The number of sectors to read */
    Address	buffer;		/* The buffer to read into */
{
    int amountWritten;

    if (lseek(openFileID, (long) (firstSector * DEV_BYTES_PER_SECTOR), L_SET) 
	== (long) -1) {
	perror("Disk_SectorWrite: lseek failed");
	fprintf(stderr, "fd = %d, offset = %ld, whence= %d\n",
	    openFileID, firstSector * DEV_BYTES_PER_SECTOR, L_SET);
	return(-1);
    }
    amountWritten = write(openFileID, buffer, 
			  DEV_BYTES_PER_SECTOR * numSectors);
    if (amountWritten < 0) {
	perror("Disk_SectorWrite: write failed");
	fprintf(stderr, "fd = %d, buffer= 0x%08x, cnt = %d firstSector = %d\n",
	    openFileID, buffer, DEV_BYTES_PER_SECTOR * numSectors,
	    firstSector);

	return(-1);
    } else if (amountWritten != DEV_BYTES_PER_SECTOR * numSectors) {
	fprintf(stderr, "Disk_SectorWrite: short write, %d sectors, not %d\n",
		        amountWritten / DEV_BYTES_PER_SECTOR, numSectors);
	errno = 0;
	return(-1);
    }
    return(0);
}


/*
 *----------------------------------------------------------------------
 *
 * Disk_BlockRead --
 *	Read blocks to the disk file at a specified block offset.
 *	This has to use the disk geometry information to figure out
 *	what disk sectors correspond to the block.
 *
 * Results:
 *	0 if could read the block, -1 if could not.  If couldn't read the block
 *	the the error is stored in errno.
 *
 * Side effects:
 *	*buffer is filled with the data from the disk.
 *
 *----------------------------------------------------------------------
 */
int
Disk_BlockRead(openFileID, headerPtr, firstBlock, numBlocks, buffer)
    int			openFileID;	/* Handle on raw disk */
    Ofs_DomainHeader	*headerPtr;	/* Domain header with geometry
					 * information */
    int			firstBlock;	/* First block to read */
    int			numBlocks;	/* The number of blocks to read */
    Address		buffer;		/* The buffer to read into */
{
    register Ofs_Geometry *geoPtr;
    register int blockIndex;
    register int cylinder;
    register int rotationalSet;
    register int blockNumber;
    int firstSector;

    geoPtr = &headerPtr->geometry;
    for (blockIndex = 0 ; blockIndex < numBlocks ; blockIndex++) {
	blockNumber	= firstBlock + blockIndex;
	cylinder	= blockNumber / geoPtr->blocksPerCylinder;
	if (geoPtr->rotSetsPerCyl > 0) {
	    /*
	     * Original mapping scheme using rotational sets.
	     */
	    blockNumber		-= cylinder * geoPtr->blocksPerCylinder;
	    rotationalSet	= blockNumber / geoPtr->blocksPerRotSet;
	    blockNumber		-= rotationalSet * geoPtr->blocksPerRotSet;
	
	    firstSector = geoPtr->sectorsPerTrack * geoPtr->numHeads * 
			 cylinder +
			 geoPtr->sectorsPerTrack * geoPtr->tracksPerRotSet *
			 rotationalSet + geoPtr->blockOffset[blockNumber];
	} else if (geoPtr->rotSetsPerCyl == OFS_SCSI_MAPPING){
	    /*
	     * New mapping for scsi devices.
	     */
	    firstSector = geoPtr->sectorsPerTrack * geoPtr->numHeads * 
			cylinder +
			blockNumber * DISK_SECTORS_PER_BLOCK - 
			cylinder * 
			geoPtr->blocksPerCylinder * DISK_SECTORS_PER_BLOCK;
	} else {
	    return -1;
	}
	if (Disk_SectorRead(openFileID, firstSector,
			     DISK_SECTORS_PER_BLOCK, buffer) < 0) {
	    return(-1);
	}
	buffer += FS_BLOCK_SIZE;
    }
    return(0);
}

/*
 *----------------------------------------------------------------------
 *
 * Disk_BadBlockRead --
 *	Read 1 block a sector at a time, returning a bitmap corresponding
 *	to the blocks that were read successfully.
 *	This has to use the disk geometry information to figure out
 *	what disk sectors correspond to the block.
 *
 * Results:
 *	The bitmask of valid sectors is returned.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
Disk_BadBlockRead(openFileID, headerPtr, blockNumber, buffer)
    int openFileID;	/* Handle on raw disk */
    Ofs_DomainHeader *headerPtr;/* Domain header with geometry information */
    int blockNumber;	/* Block to read */
    Address buffer;	/* The buffer to read into */
{
    ReturnStatus status;
    register Ofs_Geometry *geoPtr;
    register int sectorIndex;
    register int cylinder;
    register int rotationalSet;
    int firstSector;
    int valid = 0;		/* Assumes <= 32 sectors/block */

    geoPtr = &headerPtr->geometry;
    cylinder	= blockNumber / geoPtr->blocksPerCylinder;
    blockNumber	-= cylinder * geoPtr->blocksPerCylinder;
    rotationalSet	= blockNumber / geoPtr->blocksPerRotSet;
    blockNumber	-= rotationalSet * geoPtr->blocksPerRotSet;

    firstSector = geoPtr->sectorsPerTrack * geoPtr->numHeads * cylinder + 
	    geoPtr->sectorsPerTrack * geoPtr->tracksPerRotSet * rotationalSet +
	    geoPtr->blockOffset[blockNumber];
    for (sectorIndex = 0; sectorIndex < DISK_SECTORS_PER_BLOCK; sectorIndex++) {
	if (Disk_SectorRead(openFileID, firstSector + sectorIndex, 
			    1, buffer) >= 0) {
	    valid |= (1 << sectorIndex);
	}
        buffer += DEV_BYTES_PER_SECTOR;
    }
    return(valid);
}

/*
 *----------------------------------------------------------------------
 *
 * Disk_BlockWrite --
 *	Write blocks to the disk file at a specified block offset.
 *	This has to use the disk geometry information to figure out
 *	what disk sectors correspond to the block.
 *	Write blocks individually if a hard error occurs during the write
 *	of the entire block.
 *
 *	Note: ignores the error condition otherwise, so if two blocks
 *	are to be written, everything but unwritable sectors will be written
 *	and the error for the unwritable sector(s) would be returned.
 *
 * Results:
 *	0 if could write the block, -1 if could not.  If couldn't write the
 *	block then the error is stored in errno.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
Disk_BlockWrite(openFileID, headerPtr, firstBlock, numBlocks, buffer)
    int			openFileID;	/* Handle on raw disk */
    Ofs_DomainHeader	*headerPtr;	/* Domain header with geometry
					 * information */
    int			firstBlock;	/* First block to read */
    int			numBlocks;	/* The number of blocks to read */
    Address		buffer;		/* The buffer to read into */
{
    register Ofs_Geometry *geoPtr;
    register int blockIndex;	/* Loop counter */
    register int cylinder;	/* Cylinder within domain */
    register int rotationalSet;	/* Rotational Set within cylinder */
    register int blockNumber;	/* Block number within rotational set */
    int firstSector;

    geoPtr = &headerPtr->geometry;
    for (blockIndex = 0 ; blockIndex < numBlocks ; blockIndex++) {
	blockNumber	= firstBlock + blockIndex;
	cylinder	= blockNumber / geoPtr->blocksPerCylinder;
	if (geoPtr->rotSetsPerCyl > 0) {
	    /*
	     * Original mapping scheme using rotational sets.
	     */
	    blockNumber		-= cylinder * geoPtr->blocksPerCylinder;
	    rotationalSet	= blockNumber / geoPtr->blocksPerRotSet;
	    blockNumber		-= rotationalSet * geoPtr->blocksPerRotSet;
	
	    firstSector = geoPtr->sectorsPerTrack * geoPtr->numHeads * 
			 cylinder +
			 geoPtr->sectorsPerTrack * geoPtr->tracksPerRotSet *
			 rotationalSet + geoPtr->blockOffset[blockNumber];
	} else if (geoPtr->rotSetsPerCyl == OFS_SCSI_MAPPING){
	    /*
	     * New mapping for scsi devices.
	     */
	    firstSector = geoPtr->sectorsPerTrack * geoPtr->numHeads * 
			cylinder +
			blockNumber * DISK_SECTORS_PER_BLOCK - 
			cylinder * 
			geoPtr->blocksPerCylinder * DISK_SECTORS_PER_BLOCK;
	} else {
	    return -1;
	}
	if (Disk_SectorWrite(openFileID, firstSector,
			     DISK_SECTORS_PER_BLOCK, buffer) < 0) {
	    return(-1);
	}
	buffer += FS_BLOCK_SIZE;
    }
    return(0);
}

