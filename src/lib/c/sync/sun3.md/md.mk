#
# Prototype Makefile for machine-dependent directories.
#
# A file of this form resides in each ".md" subdirectory of a
# command.  Its name is typically "md.mk".  During makes in the
# parent directory, this file (or a similar file in a sibling
# subdirectory) is included to define machine-specific things
# such as additional source and object files.
#
# This Makefile is automatically generated.
# DO NOT EDIT IT OR YOU MAY LOSE YOUR CHANGES.
#
# Generated from /sprite/lib/mkmf/Makefile.md
# Mon Jun  8 14:36:25 PDT 1992
#
# For more information, refer to the mkmf manual page.
#
# $Header: /sprite/lib/mkmf/RCS/Makefile.md,v 1.6 90/03/12 23:28:42 jhh Exp $
#
# Allow mkmf

SRCS		= sun3.md/Sync_Unlock.s sun3.md/Sync_GetLock.s Sync_SlowLock.c Sync_SlowWait.c
HDRS		= 
MDPUBHDRS	= 
OBJS		= sun3.md/Sync_Unlock.o sun3.md/Sync_GetLock.o sun3.md/Sync_SlowLock.o sun3.md/Sync_SlowWait.o
CLEANOBJS	= sun3.md/Sync_Unlock.o sun3.md/Sync_GetLock.o sun3.md/Sync_SlowLock.o sun3.md/Sync_SlowWait.o
INSTFILES	= sun3.md/md.mk sun3.md/dependencies.mk Makefile
SACREDOBJS	= 
