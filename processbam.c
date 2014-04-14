#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

/* htslib headers */
#include <htslib/sam.h>
#include <htslib/hts.h>

/* tardis headers */
#include "processbam.h"

/* Sample this many concordants to calculate avg/median/std */
#define SAMPLEFRAG 1000000 

void loadBAM( bamInfo* inBam, char* path)
{
	/* Variables */
	htsFile* bam_file;
	bam_hdr_t* bam_header;
	bam1_core_t bam_alignment_core;
	bam1_t*	bam_alignment;
	int return_value;
	int i;

	/* Open the BAM file for reading. htslib automatically detects the format
		of the file, so appending "b" after "r" in mode is redundant. */
	bam_file = hts_open( path, "r");
	if( !bam_file)
	{
		fprintf( stderr, "Error opening BAM file\n");
		exit( 1);
	}

	/* Read in BAM header information */
	bam_header = bam_hdr_read( ( bam_file->fp).bgzf);

	/* Store the number of reference sequences */
	inBam->numChrom = bam_header->n_targets;

	/* For all reference sequences, store their names and lengths */
	for( i = 0; i < bam_header->n_targets; i++)
	{
		inBam->chromNames[i] = ( bam_header->target_name)[i];
		inBam->chromLengths[i] = ( bam_header->target_len)[i];
	}

	/* Get BAM header text */
	char* header_text = bam_header->text;

	/* Extract the Sample Name from the header text */
	get_sample_name( inBam, header_text);

	/* For all alignments, store the core information */
	bam_alignment = bam_init1();
	return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);
	i = 0;
	while( return_value != -1)
	{
		bam_alignment_core = bam_alignment->core;

		inBam->pos[i] = bam_alignment_core.pos + 1;
		inBam->bin[i] = bam_alignment_core.bin;
		inBam->qual[i] = bam_alignment_core.qual;
		inBam->length_read_name[i] = bam_alignment_core.l_qname;
		inBam->flag[i] =  bam_alignment_core.flag;
		inBam->pos_mate_read[i] = bam_alignment_core.mpos + 1;
		inBam->is_reverse[i] = bam_is_rev( bam_alignment);
		inBam->is_mate_reverse[i] = bam_is_mrev( bam_alignment);
		inBam->read_name[i] = bam_get_qname( bam_alignment);

		/* CIGAR string is stored by htslib as 4 bits representing the type
		 of operation and 28 bits representing the length of the operation */
		uint32_t* cigar_array = bam_get_cigar( bam_alignment);
		int num_cigar_ops = bam_alignment_core.n_cigar;
		int j;
		for( j = 0; j < num_cigar_ops; j++)
		{
			inBam->cigar_ops[i][j] = bam_cigar_opchr( cigar_array[j]);
			inBam->length_cigar_ops[i][j] = cigar_array[j] >> 4;
		}

		/* Extract the read and fragment size related information */
		inBam->length_read[i] = bam_alignment_core.l_qseq;
		inBam->insertion_size[i] = bam_alignment_core.isize;
		inBam->read[i] = bam_get_seq( bam_alignment);

		/* Read next alignment */
		return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);
		i = i + 1;
	}
	
	/* Close the BAM file */
	return_value = hts_close( bam_file);
	if( return_value != 0)
	{
		fprintf( stderr, "Error closing BAM file\n");
		exit( 1);
	} 
}

/* Decode 4-bit encoded bases to their corresponding characters */
char base_as_char( int base_as_int)
{
	if( base_as_int == 1)
	{
		return 'A';
	}
	else if( base_as_int == 2)
	{
		return 'C';
	}
	else if( base_as_int == 4)
	{
		return 'G';
	}
	else if( base_as_int == 8)
	{
		return 'T';
	}
	else if( base_as_int == 15)
	{
		return 'N';
	}
}

void get_sample_name( bamInfo* inBam, char* header_text)
{
	/* Delimit the BAM header text with tabs and newlines */
	char* p = strtok( header_text, "\t\n");
	char sample_name[1024];

	while( p != NULL)
	{
		/* If the current token has "SM" as the first two characters,
			we have found our Sample Name */
		if( p[0] == 'S' && p[1] == 'M')
		{
			/* Get the Sample Name */
			strncpy( sample_name, p + 3, strlen( p) - 3);

			/* Add the NULL terminator */
			sample_name[strlen( p) - 3] = '\0';

			/* Exit loop */
			break;
		}
		p = strtok( NULL, "\t\n");
	}

	inBam->sampleName = sample_name;
}

/* This function prints the read sequence at the given index 
 as a human readable string */ 
void print_read( bamInfo* inBam, int index)
{
	int i;	
	printf( "Read: ");
	for( i = 0; i < inBam->length_read[index]; i++)
	{
		int base = bam_seqi( inBam->read[index], i);
		printf( "%c", base_as_char( base));
	}
	printf( "\n");
}

/* Calculate the median, average, and standard deviation for the fragment size
 samples which satisfy the given criteria */
void calculate_statistics( bamInfo* inBam)
{
	int i = 0;
	int j = 0;
	int second_test_pass = 0;
	int fragment_size_total = 0;
	int fragment_size[SAMPLEFRAG];
	float variance = 0;

	while( j < SAMPLEFRAG)
	{
		if( inBam->length_read[i] > 0 && ( inBam->flag[i] & 0x10 == 0) && ( inBam->flag[i] & 0x20 != 0))
		{
			fragment_size[j] = inBam->length_read[i];
			j = j + 1;
		}
		
		i = i + 1;
	}

	qsort( fragment_size, SAMPLEFRAG, sizeof( int), compare_size);
	inBam->fragMed = fragment_size[SAMPLEFRAG / 2];

	for( i = 0; i < SAMPLEFRAG; i++)
	{
		if( fragment_size[i] <= 2 * inBam->fragMed)
		{
			fragment_size_total = fragment_size_total + fragment_size[i];
			second_test_pass = second_test_pass + 1;
		}
	}

	inBam->fragAvg = ( float) fragment_size_total / ( float) second_test_pass;

	for( i = 0; i < SAMPLEFRAG; i++)
	{
		variance = variance + ( fragment_size[i] - inBam->fragAvg) * ( fragment_size[i] - inBam->fragAvg);
	}

	variance = ( float) variance / ( float) SAMPLEFRAG;
	inBam->fragStd = sqrt( variance);
}	

int compare_size( const void* p, const void* q)
{
	int i = *( const int*) p;
    int j = *( const int*) q;

	if( i < j)
	{
		return -1;
	}
	else if( i == j)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}
