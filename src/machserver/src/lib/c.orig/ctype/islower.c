/* 
 * islower.c --
 *
 *	Contains the C library procedure "islower".
 *
 * Copyright 1988 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 */

#ifndef lint
static char rcsid[] = "$Header: islower.c,v 1.1 88/04/27 18:03:31 ouster Exp $ SPRITE (Berkeley)";
#endif not lint

#include "ctype.h"
#undef islower

/*
 *----------------------------------------------------------------------
 *
 * islower --
 *
 *	Tell whether a character is a lower-case one or not.
 *
 * Results:
 *	Returns non-zero if c is a lower-case letter, zero otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
islower(c)
    int c;			/* Character value to test.  Must be an
				 * ASCII value or EOF. */
{
    return ((_ctype_bits+1)[c] & CTYPE_LOWER);
}