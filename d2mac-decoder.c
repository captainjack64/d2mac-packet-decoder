#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Polynomial for PRBS generator */
#define _PRBS_POLY 0x7FFF

/* PRBS store */
uint16_t prbs[625];

#define MAC_PACKETS 82
#define MAC_LINE_BITS 751
#define MAC_SAMPLE_RATE 20250000
#define D2MAC_SAMPLE_RATE 10125000
#define MAC_PACKET_BYTES 91

/* Bitmap blown up by 300% = 3x samples */
#define OVERSAMPLING 1

/* Samples per symbol with oversampling */
#define SPS MAC_SAMPLE_RATE / D2MAC_SAMPLE_RATE * OVERSAMPLING

/* Top MAC line in the frame */
#define TOP_LINE 1

/* Pseudo-random binary sequence (PRBS) generator for spectrum shaping */
static int _prbs(uint16_t *x)
{
	int b;
	
	b = (*x ^ (*x >> 14)) & 1;
	*x = (*x >> 1) | (b << 14);
	
	return(b);
}

/* Load PRBS values */
void prbs_init()
{
	int i, x;
	prbs[0] = _PRBS_POLY;

	for(i = 1; i < 625; i++)
	{
		prbs[i] = prbs[i - 1];
		
		for(x = 0; x < 648; x++)
		{
			_prbs(&prbs[i]);
		}
	}
}

/* Read frame bitmap */
uint32_t  *read_bmp(char *filename, int *width, int *height)
{
	int padding, i, k;
	unsigned char *bdata;
	unsigned char info[54];
	uint32_t *frame;

	fprintf(stderr,"Reading file %s...\n", filename);
	
	FILE* f = fopen(filename, "rb");	
	if(f != NULL) 
	{
		fread(info, sizeof(unsigned char), 54, f); 
		*width  = *(int*) &info[18];
		*height = *(int*) &info[22];	
				
		padding = (*width * 3 + 3) & (~3);
		bdata = malloc(padding * sizeof(uint8_t));
		frame = malloc(*width * *height * sizeof(uint32_t));
					
		for(i = k = 0; i < *height; i++)
		{
			fread(bdata, sizeof(uint8_t), padding, f);			
			for(int j = 0; j < *width * 3; j += 3, k++) frame[k] = bdata[j + 2] << 16 | bdata[j + 1] << 8 | bdata[j] << 0;
		}
		
		fclose(f);
		free(bdata);
	}
	else
	{
		fprintf(stderr,"File not found - exiting");
		exit(-1);
	}
	
	return frame;
}

char *get_packet_type(int address)
{
	char *fmt;
	
	switch(address)
	{
		case 224:
			fmt = "audio packet";
			break;
		case 1023:
			fmt = "dummy packet";
			break;
		case 0:
			fmt = "SI packet";
			break;
		default:
			fmt = "unknown packet";
			break;
	}
	
	return fmt;
}

/* Deinterleave packet */
static void deinterleave(uint8_t pkt[94])
{
	uint8_t tmp[94];
	int c, d, i;
	
	memcpy(tmp, pkt, 94);
	
	/* + 1 bit to ensure final byte is shifted correctly */
	for(d = i = 0; i < MAC_LINE_BITS + 1; i++)
	{
		c = i >> 3;
		
		pkt[c] = (pkt[c] >> 1) | (tmp[d] << 7);
		tmp[d] >>= 1;
		
		if(++d == 94) d = 0;
	}
}

/* Pack bits into buffer MSB first */
static size_t _rbits(uint8_t *data, size_t offset, uint64_t bits, size_t nbits)
{
	uint64_t m = (uint64_t) 1 << (nbits - 1);
	uint8_t b;
	
	for(; nbits; nbits--, offset++, bits <<= 1)
	{
		b = 1 << (offset & 7);
		if(bits & m) data[offset >> 3] |= b;
		else data[offset >> 3] &= ~b;
	}
	
	return(offset);
}

int get_polarity(int b)
{
	/* Limits for '0' bit */
	int grey_low = 0x55;
	int grey_high = 0xAA;
	
	if(b >= 0 && b <= grey_low) return -1;
	if(b > grey_low && b <= grey_high) return 0;
	if(b > grey_high && b <= 0xFF) return 1;
	
	return 0;
}

int get_bit(int p)
{
	return (!p ? p : 1);
}

uint64_t find_hsync(int line, int width, int height, uint32_t *frame)
{
	/* Loops */
	int i, j;
	
	/* Frame position */
	int f;
	
	/* Sample bit */
	int bit, pol;
	
	/* Byte (6 bits) */
	uint8_t byte;
	
	/* Position in frame */
	int pos = OVERSAMPLING;
	
	line = line * OVERSAMPLING - (OVERSAMPLING /2);
	
	for(j = 0; j < width - 12; j++)
	{
		
		for(i = pos = byte = 0; i < 6; i++, pos += SPS)
		{
			/* Get position of sample in the frame */
			f = j + pos + (width * (height - line));
			
			/* Get polarity */
			pol = get_polarity(frame[f] & 0xFF);
			
			/* Get bit */
			bit = get_bit(pol);
			
			byte |= (bit & 1) << i;
		}
		
		/* Break if line sync word found */
		if(byte == 0x34 || byte == 0x0B) 
		{
			fprintf(stderr,"line sync (0x%02X) found @ offset %03i: data:", byte, j - 1);
			break;
		}
	}
	
	return (j - 1);
}

uint64_t get_line_bits(uint8_t *data, int line, int width, int height, int pos, uint32_t *frame, uint64_t offset)
{
	int i;
	
	/* Frame position */
	int f;
	
	/* Sample bit */
	int bit, pol;
	
	/* Get PRBS line */
	uint16_t poly = prbs[TOP_LINE + line - 2];
	
	line = line * OVERSAMPLING - (OVERSAMPLING /2);
	
	/* Scan for 99 bits */
	for(i = 0; i < 99; i++, pos += SPS)
	{
		/* Get position of sample in the frame */
		f = pos + (width * (height - line));
		
		/* Get polarity */
		pol = get_polarity(frame[f] & 0xFF);
		
		/* Get bit */
		bit = get_bit(pol);
		
		/* Pack into data with PRBS removed */
		offset = _rbits(data, offset, bit ^ _prbs(&poly), 1);
	}
	
	/* Print data */
	for(i = 0; i < 12; i++) fprintf(stderr," %02X ", data[(offset - 99)/8 + i]);
	fprintf(stderr,"\n");
	return offset;
}

void decode_frame(uint32_t *frame, int width, int height)
{
	
	uint8_t *data;
	
	/* Give memory - 12 bytes per line x number of lines */
	data = malloc(12 * height * sizeof(uint8_t));
	
	/* Packet store */
	uint8_t *pkt;
	
	/* Give memory - 94 bytes per packet */
	pkt = malloc(94 * sizeof(uint8_t));
	
	/* Overall data offset counter */
	int offset;
	
	/* Loop vars */
	int i, j, l;
	
	/* Zeroise missing data */
	for(offset = 0, l = 1; l < TOP_LINE; l++)
	{
		fprintf(stderr,"Skipping/packing line: %03i...\n", l);
		for(i = 0; i < 99; i++)
		{
			offset = _rbits(data, offset, 0, 1);
		}
	}

	/* Line sync offset */
	int hoffset;
	
	/* Scan lines for data */
	for(l = 1; l < height / OVERSAMPLING; l++)
	{
		fprintf(stderr,"Line %03i: ", TOP_LINE + l - 1);
		
		/* Find line sync and get starting position */
		hoffset = find_hsync(l, width, height, frame);
		
		// hoffset = 293;
		
		/* Get bits */
		offset = get_line_bits(data, l, width, height, hoffset + (6 * SPS) + (SPS/2), frame, offset);
	}
	
	/* Splice data and display packets */	
	if(1)
	{
		/* Address/continuty */
		int address;
		int continuity;
		
		/* Packet bits/bytes */
		int bit;
		char byte;
		
		/* Packet offset counter */
		int poffset;
		
		for(l = 0, offset = 0; l < MAC_PACKETS; l++)
		{
			/* Pack bits into packet */
			for(i = 0, poffset = 0; i < MAC_LINE_BITS; i++, offset++)
			{
				bit = data[offset >> 3] & 1;
				poffset = _rbits(pkt, poffset, bit , 1);
				data[offset >> 3] >>= 1;
			}
			
			/* Do what it says */
			deinterleave(pkt);
			
			/* Grab address and continuity values */
			address = (pkt[1] & 0x03) << 8| pkt[0];
			continuity = pkt[1] >> 2 & 0x03;
			
			/* Display packet payload */
			fprintf(stderr,"\nPacket number: %i\n", l + 1);
			fprintf(stderr,"Packet address: %d (%s)\n", address, get_packet_type(address));
			fprintf(stderr,"Packet continuity: %i\n", continuity);
			fprintf(stderr,"Packet data:\n\t");
			
			for(i = 2; i <  MAC_PACKET_BYTES - 2; i += 16)
			{
				for(j = 0; j < 16; j++)
				{
					byte = ((pkt[i + j] >> 7 & 1) | (pkt[i + 1 + j] << 1)) & 0xFF;
					fprintf(stderr,"%02X ", byte & 0xFF);
				}
				fprintf(stderr,"  ");
				for(j = 0; j < 16; j++)
				{
					byte = ((pkt[i + j] >> 7 & 1) | (pkt[i + 1 + j] << 1)) & 0xFF;
					fprintf(stderr,"%c", (byte & 0xFF) != 0x0A && (byte & 0xFF) < 0x80 ? byte & 0xFF : ' ');
				}
				fprintf(stderr,"\n\t");
			}
		}
	}
}

int main()
{
	int width, height;

	/* Initialise line PRBS */
	prbs_init();
	
	/* Get image data */
	uint32_t *frame = read_bmp("packets.bmp", &width, &height);

	/* Call decode frame routing and hope for the best */
	decode_frame(frame, width, height);
	fprintf(stderr,"\n");
	return 0;
}