/* Floor function.  PowerPC32 version.
   Copyright (C) 2004-2016 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#define _LANGUAGE_ASSEMBLY
#include "asm.h"

.section .rodata.cst4,"aM",@progbits,4
	.align 2
.LC0:	/* 2**52 */
	.long 0x59800000

.section .text
	.globl floor
floor:
	lis		r9, .LC0@ha
	lfs		fr13, .LC0@l (r9)
	fabs	fr0, fr1
	fsub	fr12, fr13, fr13	/* generate 0.0  */
	fcmpu	cr7, fr0, fr13		/* if (fabs(x) > TWO52)  */
	mffs	fr11				/* Save current FPU rounding mode and
								   "inexact" state.  */
	fcmpu	cr6, fr1, fr12		/* if (x > 0.0)  */
	bnllr-	cr7
	mtfsfi	7, 3				/* Set rounding mode toward -inf.  */
	ble-	cr6, .L4
	fadd	fr1, fr1, fr13		/* x+= TWO52;  */
	fsub	fr1, fr1, fr13		/* x-= TWO52;  */
	fabs	fr1, fr1			/* if (x == 0.0)  */
								/* x = 0.0; */
	mtfsf	0xff, fr11			/* Restore previous rounding mode and
								   "inexact" state.  */
	blr
.L4:
	bge-	cr6, .L9			/* if (x < 0.0)  */
	fsub	fr1, fr1, fr13		/* x-= TWO52;  */
	fadd	fr1, fr1, fr13		/* x+= TWO52;  */
	fnabs	fr1, fr1			/* if (x == 0.0)  */
								/* x = -0.0; */
.L9:
	mtfsf	0xff, fr11			/* Restore previous rounding mode and
								   "inexact" state.  */
	blr
