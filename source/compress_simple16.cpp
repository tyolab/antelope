/*
	COMPRESS_SIMPLE16.C
	------------------
	Simple-16, an adaptation of Simple-9.

	This code is a substantial rewrite of compress_simple9.c, merged with the ideas developed in the following papers:
	Zhang J, Long X, Suel T. (April 2008) Performance of compressed inverted list caching in search engines. Proceeedings of 17th Conference on the World Wide Web, pp 387-396 
	see http://www2008.org/papers/pdf/p387-zhangA.pdf
	also see http://www2009.org/proceedings/pdf/p401.pdf

	Author: Blake Burgess
	License: BSD (see compress_simple9.c and compress_sigma.c)
*/
#include <stdio.h>
#include "compress_simple16.h"
#include "maths.h"

#define FIND_FIRST_SET 1+ANT_floor_log2
#define FIND_LAST_SET ANT_ceiling_log2

/*
	ANT_compress_simple16::simple16_shift_table[]
	---------------------------------------------
	Number of bits to shift across when packing -- is sum of prior packed ints (see above)
*/
long ANT_compress_simple16::simple16_shift_table[] =
{
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
0, 2, 4, 6, 8, 10, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 28, 28, 28, 28, 28, 28,
0, 1, 2, 3, 4, 5, 6, 7, 9, 11, 13, 15, 17, 19, 21, 22, 23, 24, 25, 26, 27, 28, 28, 28, 28, 28, 28, 28,
0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 26, 28, 28, 28, 28, 28, 28, 28,
0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 4, 7, 10, 13, 16, 19, 22, 25, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 3, 7, 11, 15, 19, 22, 25, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 4, 8, 12, 16, 20, 24, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 5, 10, 15, 20, 24, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 4, 8, 13, 18, 23, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 6, 12, 18, 23, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 5, 10, 16, 22, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 7, 14, 21, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 10, 19, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 14, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
0, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28
};

/*
	ANT_compress_simple16::ints_packed_table[]
	--------------------------------------------
	Number of integers packed into a word, given its mask type
*/
long ANT_compress_simple16::ints_packed_table[] = {28, 21, 21, 21, 14, 9, 8, 7, 6, 6, 5, 5, 4, 3, 2, 1};


/*
	ANT_COMPRESS_SIMPLE16::CAN_PACK_TABLE[]
	---------------------------------------------
	Bitmask map for valid masks at an offset (column) for some num_bits_needed (row).
*/
long ANT_compress_simple16::can_pack_table[] =
{
0xffff, 0x7fff, 0x3fff, 0x1fff, 0x0fff, 0x03ff, 0x00ff, 0x007f, 0x003f, 0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x000f, 0x000f, 0x000f, 0x000f, 0x000f, 0x000f, 0x000f, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
0xfff2, 0x7ff2, 0x3ff2, 0x1ff2, 0x0ff2, 0x03f2, 0x00f2, 0x0074, 0x0034, 0x0014, 0x0014, 0x0014, 0x0014, 0x0014, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0008, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xffe0, 0x7fe0, 0x3fe0, 0x1fe0, 0x0fe0, 0x03e0, 0x00e0, 0x0060, 0x0020, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xffa0, 0x7fc0, 0x3fc0, 0x1fc0, 0x0fc0, 0x0380, 0x0080, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xfd00, 0x7d00, 0x3f00, 0x1f00, 0x0e00, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xf400, 0x7400, 0x3c00, 0x1800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xf000, 0x7000, 0x3000, 0x1000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xe000, 0x6000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xe000, 0x4000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0xc000, 0x4000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
0x8000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

/*
	ANT_COMPRESS_SIMPLE16::INVALID_MASKS_FOR_OFFSET[]
	-----------------------------------
	We AND out masks for offsets where we don't know if we can fully pack for that offset
*/
long ANT_compress_simple16::invalid_masks_for_offset[] =
{
0x0000, 0x8000, 0xc000, 0xe000, 0xf000, 0xfc00, 0xff00, 0xff80, 0xffc0, 0xffe0, 0xffe0, 0xffe0, 0xffe0, 0xffe0, 0xfff0, 0xfff0, 0xfff0, 0xfff0, 0xfff0, 0xfff0, 0xfff0, 0xfffe, 0xfffe, 0xfffe, 0xfffe, 0xfffe, 0xfffe, 0xfffe, 0xffff
};

/*
	ANT_COMPRESS_SIMPLE16::ROW_FOR_BITS_NEEDED[]
	-----------------------------------
	Translates the 'bits_needed' to the appropriate 'row' offset for use with can_pack table.
*/
long ANT_compress_simple16::row_for_bits_needed[] =
{
0, 0, 28, 56, 84, 112, 140, 168, 196, 196, 224, 252, 252, 252, 252, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/*
	ANT_COMPRESS_SIMPLE16::COMPRESS()
	--------------------------------
*/
long long ANT_compress_simple16::compress(unsigned char *destination, long long destination_length, ANT_compressable_integer *source, long long source_integers)
{
long long words_in_compressed_string, pos;
uint32_t mask_type, num_to_pack;
uint32_t *into, *end;
uint32_t remaining;
uint32_t offset, mask_type_offset;
uint16_t last_bitmask, bitmask;
into = (uint32_t *)destination;
end = (uint32_t *)(destination + destination_length);
pos = 0;
for (words_in_compressed_string = 0; pos < source_integers; words_in_compressed_string++)
	{
	remaining = (pos + 28 < source_integers) ? 28 : source_integers - pos;
	last_bitmask = 0x0000;
	bitmask = 0xFFFF;
	/* constrain last_bitmask to contain only bits for masks we can pack with */
	for (offset = 0; offset < remaining && bitmask; offset++)
		{
		bitmask &= can_pack_table[row_for_bits_needed[FIND_LAST_SET(source[pos + offset])] + offset];
		last_bitmask |= (bitmask & invalid_masks_for_offset[offset + 1]);
		}
	/* 'ffs' function returns 0 => no bits were set => no valid masks => invalid input */
	if ((mask_type = FIND_FIRST_SET(last_bitmask)) == 0)
		return 0;
	/* turn bit position into actual mask to use */
	mask_type--;
	num_to_pack = ints_packed_table[mask_type];
	/* pack the word */
	*into = 0;
	mask_type_offset = 28 * mask_type;
	for (offset = 0; offset < num_to_pack; offset++)
		*into |= (source[pos + offset] << simple16_shift_table[mask_type_offset + offset]);
	*into = (*into << 4) | mask_type;
	pos += num_to_pack;
	into++;
	if (into > end)
		return 0;
	}
return words_in_compressed_string * sizeof(*into); //stores the length of n[]
}

/*
	ANT_COMPRESS_SIMPLE16::DECOMPRESS()
	----------------------------------
*/
void ANT_compress_simple16::decompress(ANT_compressable_integer *destination, unsigned char *source, long long destination_integers)
{
uint32_t *compressed_sequence = (uint32_t *)source;
uint32_t value, mask_type;
ANT_compressable_integer *end = destination + destination_integers;

while (destination < end)
	{
	value = *compressed_sequence++;
	mask_type = value & 0xF;
	value >>= 4;

	// Unrolled loop to enable pipelining
	switch (mask_type)
		{
		case 0x0:
			*destination++ = value & 0x1;
			*destination++ = (value >> 0x1) & 0x1;
			*destination++ = (value >> 0x2) & 0x1;
			*destination++ = (value >> 0x3) & 0x1;
			*destination++ = (value >> 0x4) & 0x1;
			*destination++ = (value >> 0x5) & 0x1;
			*destination++ = (value >> 0x6) & 0x1;
			*destination++ = (value >> 0x7) & 0x1;
			*destination++ = (value >> 0x8) & 0x1;
			*destination++ = (value >> 0x9) & 0x1;
			*destination++ = (value >> 0xA) & 0x1;
			*destination++ = (value >> 0xB) & 0x1;
			*destination++ = (value >> 0xC) & 0x1;
			*destination++ = (value >> 0xD) & 0x1;
			*destination++ = (value >> 0xE) & 0x1;
			*destination++ = (value >> 0xF) & 0x1;
			*destination++ = (value >> 0x10) & 0x1;
			*destination++ = (value >> 0x11) & 0x1;
			*destination++ = (value >> 0x12) & 0x1;
			*destination++ = (value >> 0x13) & 0x1;
			*destination++ = (value >> 0x14) & 0x1;
			*destination++ = (value >> 0x15) & 0x1;
			*destination++ = (value >> 0x16) & 0x1;
			*destination++ = (value >> 0x17) & 0x1;
			*destination++ = (value >> 0x18) & 0x1;
			*destination++ = (value >> 0x19) & 0x1;
			*destination++ = (value >> 0x1A) & 0x1;
			*destination++ = (value >> 0x1B) & 0x1;
			break;
		case 0x1:
			*destination++ = value & 0x3;
			*destination++ = (value >> 0x2) & 0x3;
			*destination++ = (value >> 0x4) & 0x3;
			*destination++ = (value >> 0x6) & 0x3;
			*destination++ = (value >> 0x8) & 0x3;
			*destination++ = (value >> 0xA) & 0x3;
			*destination++ = (value >> 0xC) & 0x3;
			*destination++ = (value >> 0xE) & 0x1;
			*destination++ = (value >> 0xF) & 0x1;
			*destination++ = (value >> 0x10) & 0x1;
			*destination++ = (value >> 0x11) & 0x1;
			*destination++ = (value >> 0x12) & 0x1;
			*destination++ = (value >> 0x13) & 0x1;
			*destination++ = (value >> 0x14) & 0x1;
			*destination++ = (value >> 0x15) & 0x1;
			*destination++ = (value >> 0x16) & 0x1;
			*destination++ = (value >> 0x17) & 0x1;
			*destination++ = (value >> 0x18) & 0x1;
			*destination++ = (value >> 0x19) & 0x1;
			*destination++ = (value >> 0x1A) & 0x1;
			*destination++ = (value >> 0x1B) & 0x1;
			break;
		case 0x2:
			*destination++ = value & 0x1;
			*destination++ = (value >> 0x1) & 0x1;
			*destination++ = (value >> 0x2) & 0x1;
			*destination++ = (value >> 0x3) & 0x1;
			*destination++ = (value >> 0x4) & 0x1;
			*destination++ = (value >> 0x5) & 0x1;
			*destination++ = (value >> 0x6) & 0x1;
			*destination++ = (value >> 0x7) & 0x3;
			*destination++ = (value >> 0x9) & 0x3;
			*destination++ = (value >> 0xB) & 0x3;
			*destination++ = (value >> 0xD) & 0x3;
			*destination++ = (value >> 0xF) & 0x3;
			*destination++ = (value >> 0x11) & 0x3;
			*destination++ = (value >> 0x13) & 0x3;
			*destination++ = (value >> 0x15) & 0x1;
			*destination++ = (value >> 0x16) & 0x1;
			*destination++ = (value >> 0x17) & 0x1;
			*destination++ = (value >> 0x18) & 0x1;
			*destination++ = (value >> 0x19) & 0x1;
			*destination++ = (value >> 0x1A) & 0x1;
			*destination++ = (value >> 0x1B) & 0x1;
			break;
		case 0x3:
			*destination++ = value & 0x1;
			*destination++ = (value >> 0x1) & 0x1;
			*destination++ = (value >> 0x2) & 0x1;
			*destination++ = (value >> 0x3) & 0x1;
			*destination++ = (value >> 0x4) & 0x1;
			*destination++ = (value >> 0x5) & 0x1;
			*destination++ = (value >> 0x6) & 0x1;
			*destination++ = (value >> 0x7) & 0x1;
			*destination++ = (value >> 0x8) & 0x1;
			*destination++ = (value >> 0x9) & 0x1;
			*destination++ = (value >> 0xA) & 0x1;
			*destination++ = (value >> 0xB) & 0x1;
			*destination++ = (value >> 0xC) & 0x1;
			*destination++ = (value >> 0xD) & 0x1;
			*destination++ = (value >> 0xE) & 0x3;
			*destination++ = (value >> 0x10) & 0x3;
			*destination++ = (value >> 0x12) & 0x3;
			*destination++ = (value >> 0x14) & 0x3;
			*destination++ = (value >> 0x16) & 0x3;
			*destination++ = (value >> 0x18) & 0x3;
			*destination++ = (value >> 0x1A) & 0x3;
			break;
		case 0x4:
			*destination++ = value & 0x3;
			*destination++ = (value >> 0x2) & 0x3;
			*destination++ = (value >> 0x4) & 0x3;
			*destination++ = (value >> 0x6) & 0x3;
			*destination++ = (value >> 0x8) & 0x3;
			*destination++ = (value >> 0xA) & 0x3;
			*destination++ = (value >> 0xC) & 0x3;
			*destination++ = (value >> 0xE) & 0x3;
			*destination++ = (value >> 0x10) & 0x3;
			*destination++ = (value >> 0x12) & 0x3;
			*destination++ = (value >> 0x14) & 0x3;
			*destination++ = (value >> 0x16) & 0x3;
			*destination++ = (value >> 0x18) & 0x3;
			*destination++ = (value >> 0x1A) & 0x3;
			break;
		case 0x5:
			*destination++ = value & 0xF;
			*destination++ = (value >> 0x4) & 0x7;
			*destination++ = (value >> 0x7) & 0x7;
			*destination++ = (value >> 0xA) & 0x7;
			*destination++ = (value >> 0xD) & 0x7;
			*destination++ = (value >> 0x10) & 0x7;
			*destination++ = (value >> 0x13) & 0x7;
			*destination++ = (value >> 0x16) & 0x7;
			*destination++ = (value >> 0x19) & 0x7;
			break;
		case 0x6:
			*destination++ = value & 0x7;
			*destination++ = (value >> 0x3) & 0xF;
			*destination++ = (value >> 0x7) & 0xF;
			*destination++ = (value >> 0xB) & 0xF;
			*destination++ = (value >> 0xF) & 0xF;
			*destination++ = (value >> 0x13) & 0x7;
			*destination++ = (value >> 0x16) & 0x7;
			*destination++ = (value >> 0x19) & 0x7;
			break;
		case 0x7:
			*destination++ = value & 0xF;
			*destination++ = (value >> 0x4) & 0xF;
			*destination++ = (value >> 0x8) & 0xF;
			*destination++ = (value >> 0xC) & 0xF;
			*destination++ = (value >> 0x10) & 0xF;
			*destination++ = (value >> 0x14) & 0xF;
			*destination++ = (value >> 0x18) & 0xF;
			break;
		case 0x8:
			*destination++ = value & 0x1F;
			*destination++ = (value >> 0x5) & 0x1F;
			*destination++ = (value >> 0xA) & 0x1F;
			*destination++ = (value >> 0xF) & 0x1F;
			*destination++ = (value >> 0x14) & 0xF;
			*destination++ = (value >> 0x18) & 0xF;
			break;
		case 0x9:
			*destination++ = value & 0xF;
			*destination++ = (value >> 0x4) & 0xF;
			*destination++ = (value >> 0x8) & 0x1F;
			*destination++ = (value >> 0xD) & 0x1F;
			*destination++ = (value >> 0x12) & 0x1F;
			*destination++ = (value >> 0x17) & 0x1F;
			break;
		case 0xA:
			*destination++ = value & 0x3F;
			*destination++ = (value >> 0x6) & 0x3F;
			*destination++ = (value >> 0xC) & 0x3F;
			*destination++ = (value >> 0x12) & 0x1F;
			*destination++ = (value >> 0x17) & 0x1F;
			break;
		case 0xB:
			*destination++ = value & 0x1F;
			*destination++ = (value >> 0x5) & 0x1F;
			*destination++ = (value >> 0xA) & 0x3F;
			*destination++ = (value >> 0x10) & 0x3F;
			*destination++ = (value >> 0x16) & 0x3F;
			break;
		case 0xC:
			*destination++ = value & 0x7F;
			*destination++ = (value >> 0x7) & 0x7F;
			*destination++ = (value >> 0xE) & 0x7F;
			*destination++ = (value >> 0x15) & 0x7F;
			break;
		case 0xD:
			*destination++ = value & 0x3FF;
			*destination++ = (value >> 0xA) & 0x1FF;
			*destination++ = (value >> 0x13) & 0x1FF;
			break;
		case 0xE:
			*destination++ = value & 0x3FFF;
			*destination++ = (value >> 0xE) & 0x3FFF;
			break;
		case 0xF:
			*destination++ = value & 0xFFFFFFF;
			break;
		}
	}
}
