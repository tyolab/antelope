/*
	COMPRESS_CARRYOVER12.C
	----------------------
	Functions for Vo Ngoc Anh and Alistair Moffat's Carryover-12 compression scheme
	This is a port of Anh and Moffat's code to ANT.

	Originally (http://www.cs.mu.oz.au/~alistair/carry/)
		Copyright (C) 2003  Authors: Vo Ngoc Anh & Alistair Moffat

		This program is free software; you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation; either version 2 of the License, or
		(at your option) any later version.

		This program is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		GNU General Public License for more details.

	These Changes
		Copyright (C) 2006, 2009 Authors: Andrew Trotman
		22 January 2010, Vo Ngoc Anh and Alistair Moffat gave permission to BSD this code and derivitaves of it:

		> From: "Vo Ngoc ANH" <vo@csse.unimelb.edu.au>
		> To: "Andrew Trotman" <andrew@cs.otago.ac.nz>
		> Cc: "Alistair Moffat" <alistair@csse.unimelb.edu.au>; "andrew Trotman" <andrew.trotman@otago.ac.nz>
		> Sent: Friday, January 22, 2010 1:11 PM
		> Subject: Re: Carryover 12
		>
		>
		>
		> Hi Andrew,
		> Thank you for your interest. To tell the truth, I don't have enough 
		> knowledge on the license matter. From my side, the answer is Yes. Please 
		> let me know if I need to do anything for that.
		>
		> Cheers,
		> Anh.
		>
		> On Fri, 22 Jan 2010, Andrew Trotman wrote:
		>
		>> Hi,
		>>
		>> At ADCS I spoke biefly to Alistair about licenses for your source code to 
		>> the Carryover 12 compression scheme.  I have a copy I downloaded shortly 
		>> after you released it, and I've hacked it quite a bit.  The problem I 
		>> have is that I want to release my hacked version using a BSD license and 
		>> your code is GPL. The two licenses are not compatible.  I want to use the 
		>> BSD license because my code includes other stuff that is already BSD 
		>> (such as a hashing algorithm). As far as I know the only GPL code I'm 
		>> using is yours.
		>>
		>> Could I please have a "special" license to release my derivitave of your 
		>> code (and derivitaves of my derivitaves) under the BSD license.
		>>
		>> Thanks
		>> Andrew.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pragma.h"

#pragma ANT_PRAGMA_CONST_CONDITIONAL

#include "compress_carryover12.h"
#include "compress_carryover12_internals.h"


#define MAX_ELEM_PER_WORD		64


/* ========================================================
 Coding variables:
   trans_B1_30_big[], trans_B1_32_big are left and right transition
     tables (see the paper) for the case when the largest elements 
     occupies more than 16 bits.
   trans_B1_30_small[], trans_B1_32_small are for the otherwise case

   __pc30, __pc32 is points to the left, right tables currently used
   __pcbase points to either __pc30 or __pc32 and represents the
     current transition table used for coding
   ========================================================
*/ 

unsigned char *__pc30, *__pc32;	/* point to transition table, 30 and 32 data bits */
unsigned char *__pcbase;    /* point to current transition table */
/*
	big is transition table for the cases when number of bits
	needed to code the maximal value exceeds 16.
	_small are used otherwise.
*/
unsigned char trans_B1_30_big[]={
	0,0,0,0, 1,2,3,28, 1,2,3,28, 2,3,4,28, 3,4,5,28, 4,5,6,28,
	5,6,7,28, 6,7,8,28, 6,7,10,28, 8,10,15,28, 9,10,14,28,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 10,15,16,28, 10,14,15,28,
	7,10,15,28, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	6,10,16,28, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 4,9,15,28};

unsigned char trans_B1_32_big[]={
	0,0,0,0, 1,2,3,28, 1,2,3,28, 2,3,4,28, 3,4,5,28, 4,5,6,28,
	5,6,7,28, 6,7,8,28, 7,9,10,28, 7,10,15,28, 8,10,15,28,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 7,10,15,28, 10,15,16,28,
	10,14,15,28, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	6,10,16,28, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 4,10,16,28};

unsigned char trans_B1_30_small[]={
	0,0,0,0, 1,2,3,16, 1,2,3,16, 2,3,4,16, 3,4,5,16, 4,5,6,16,
	5,6,7,16, 6,7,8,16, 6,7,10,16, 7,8,10,16, 9,10,14,16, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 8,10,15,16, 10,14,15,16,  7,10,15,16, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 3,7,10,16};

unsigned char trans_B1_32_small[] = {
	0,0,0,0, 1,2,3,16, 1,2,3,16, 2,3,4,16, 3,4,5,16, 4,5,6,16,
	5,6,7,16, 6,7,8,16, 7,9,10,16, 7,10,15,16, 8,10,15,16, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 7,10,15,16, 8,10,15,16, 10,14,15,16, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
	0,0,0,0, 3,7,10,16};
 
static unsigned char CLOG2TAB[] = {
	0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 
   5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 
   6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 
   6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 
   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
   7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 
   8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };

#define GET_TRANS_TABLE(avail) avail < 2? (avail = 30, __pc30) : (avail -= 2, __pc32)

/*
	__MASK
	------
	__mask[i] is 2^i-1
*/
unsigned __mask[33]= {
  0x00U, 0x01U, 0x03U, 0x07U, 0x0FU,
  0x1FU, 0x3FU, 0x7FU, 0xFFU,
  0x01FFU, 0x03FFU, 0x07FFU, 0x0FFFU,
  0x1FFFU, 0x3FFFU, 0x7FFFU, 0xFFFFU,
  0x01FFFFU, 0x03FFFFU, 0x07FFFFU, 0x0FFFFFU,
  0x1FFFFFU, 0x3FFFFFU, 0x7FFFFFU, 0xFFFFFFU,
  0x01FFFFFFU, 0x03FFFFFFU, 0x07FFFFFFU, 0x0FFFFFFFU,
  0x1FFFFFFFU, 0x3FFFFFFFU, 0x7FFFFFFFU, 0xFFFFFFFFU }; 

/* 
	MACROS FOR WORD ENCODING
	========================
*/
#define WORD_ENCODE_WRITE													\
	do																		\
		{																	\
		uint32_t word;														\
		word = __values[--__pvalue];										\
		for (--__pvalue; __pvalue >= 0; __pvalue--)							\
			{																\
			word <<= __bits[__pvalue];										\
			word |= __values[__pvalue];										\
			}																\
		*((uint32_t *)destination) = word;									\
		destination += sizeof(word);										\
		if (destination >= destination_end)									\
			return 0;	 /* overflow */										\
		__wremaining = 32;													\
		__pvalue = 0;														\
		}																	\
	while(0)

#define WORD_ENCODE(x,b)													\
	do 																		\
		{																	\
		if (__wremaining < (b))												\
			WORD_ENCODE_WRITE;												\
		__values[__pvalue] = ((uint32_t)x) - 1;								\
		__bits[__pvalue++] = (b);											\
		__wremaining -= (b);												\
		} 																	\
	while (0)

#define CARRY_BLOCK_ENCODE_START(n, max_bits)								\
	do 																		\
		{																	\
		__pc30 = max_bits <= 16 ? trans_B1_30_small : trans_B1_30_big;		\
		__pc32 = max_bits <= 16 ? trans_B1_32_small : trans_B1_32_big;		\
		__pcbase = __pc30;													\
		WORD_ENCODE((max_bits <= 16 ? 1 : 2), 1);							\
		} 																	\
	while(0)

/*
	QCEILLOG_2()
	------------
*/
static inline int32_t qceillog_2(ANT_compressable_integer x)
{
ANT_compressable_integer _B_x  = x - 1;

return _B_x >> 16 ? 
	(_B_x >> 24 ? 24 + CLOG2TAB[_B_x >> 24] : 16 | CLOG2TAB[_B_x >> 16]) : 
	(_B_x >> 8 ? 8 + CLOG2TAB[_B_x >> 8] : CLOG2TAB[_B_x]);
}

/*
	CALC_MIN_BITS()
	---------------
	Calculate the number of bits needed to store the largest integer in the list
*/
static inline int32_t calc_min_bits(ANT_compressable_integer *gaps, int64_t n)
{
int64_t i;
int32_t bits, max = 0;

for (i = 0; i < n; i++)

if (max < (bits = qceillog_2(gaps[i] + 1)))
	max = bits;

return max > 28 ? -1 : max;
}

/*
	ELEMS_CODED()
	-------------
	given codeleng of "len" bits, and "avail" bits available for coding,
	Return number_of_elems_coded (possible) if "avail" bits can be used to
	code the number of elems  with the remaining < "len"
	Returns 0 (impossible) otherwise
*/
int64_t elems_coded(int32_t avail, int32_t len, ANT_compressable_integer *gaps, int64_t start, int64_t end)
{
int64_t i, real_end, max;

if (len)
	{
	max = avail/len;
	real_end = start + max - 1 <= end ? start + max: end + 1; 
	for (i = start; i < real_end && qceillog_2(gaps[i] + 1) <= len; i++);			// empty loop
	if (i < real_end)
		return 0;
	return real_end - start;
	}
else
	{
	for (i = start; i < start + MAX_ELEM_PER_WORD && i <= end && qceillog_2(gaps[i] + 1) <= len; i++);			// empty loop

	if (i - start < 2)
		return 0;
	return i - start;
	}  
}

/*
	ANT_COMPRESS_CARRYOVER12::COMPRESS()
	------------------------------------
	Parameters
	a - (source)
	n - length of a (in integers)
	destination - where the compressed sequence is put
	destination_length - length of destination (in bytes)

	returns length of destination (length in bytes)
*/
long long ANT_compress_carryover12::compress(unsigned char *destination, long long destination_length, ANT_compressable_integer *a, long long n)
{
uint32_t max_bits;
uint32_t __values[32];			// can't compress integers larger than 2^28 so they will all fit in a uint32_t
uint32_t __bits[32];
int64_t i;
int32_t j, __wremaining = 32, __pvalue = 0, size, avail;
int64_t elems = 0;
unsigned char *table, *base, *destination_end, *original_destination = destination;

destination_end = destination + destination_length;

size = TRANS_TABLE_STARTER;

if ((max_bits = calc_min_bits(a, n)) < 0)
	return 0;

CARRY_BLOCK_ENCODE_START(n, max_bits);

for (i = 0; i < n; )
	{
	avail = __wremaining;
	table = GET_TRANS_TABLE(avail);
	base = table + (size << 2);       /* row in trans table */

	/* 1. Modeling: Find j= the first-fit column in base */	
	for (j = 0; j < 4; j++)
		{
		size = base[j];
		if (size > avail) 		/* must use next word for data  */
			{
			avail = 32;
			j = -1;
			continue;
			}
		if ((elems = elems_coded(avail, size, a, i, n - 1)) != 0)
			break;
		}

	/* 2. Coding: Code elements using row "base" & column "j" */
	WORD_ENCODE(j + 1, 2);             /* encoding column */
	for ( ; elems ; elems--, i++)   /* encoding d-gaps */
		WORD_ENCODE(a[i] + 1, size);
	}

if (__pvalue)
	WORD_ENCODE_WRITE;

return destination - original_destination;
}

/*
	MACROS FOR WORD DECODING
	========================
*/

/*
	ANT_COMPRESS_CARRYOVER12::DECOMPRESS()
	--------------------------------------
	__wpos is the compressed string
	destination is the destination
	n is the number of integers in __wpos
*/
void ANT_compress_carryover12::decompress(ANT_compressable_integer *destination, unsigned char *compressed, long long n)
{
int64_t i;
int32_t __wbits = TRANS_TABLE_STARTER;
int32_t __wremaining = -1;
uint32_t *__wpos = (uint32_t *)compressed, __wval = 0;

CARRY_BLOCK_DECODE_START;
for (i = 0; i < n; i++)
	CARRY_DECODE(*destination++);
}
