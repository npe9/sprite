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
# Tue Sep 24 17:17:15 PDT 1991
#
# For more information, refer to the mkmf manual page.
#
# $Header: /sprite/lib/mkmf/RCS/Makefile.md,v 1.6 90/03/12 23:28:42 jhh Exp $
#
# Allow mkmf

SRCS		= List_Init.c List_Insert.c List_ListInsert.c List_Move.c List_Remove.c
HDRS		= 
MDPUBHDRS	= 
OBJS		= sun3.md/List_Init.o sun3.md/List_Insert.o sun3.md/List_ListInsert.o sun3.md/List_Move.o sun3.md/List_Remove.o
CLEANOBJS	= sun3.md/List_Init.o sun3.md/List_Insert.o sun3.md/List_ListInsert.o sun3.md/List_Move.o sun3.md/List_Remove.o
INSTFILES	= Makefile
SACREDOBJS	= 