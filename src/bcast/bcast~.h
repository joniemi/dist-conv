/* ------------------------ bcast~ -------------------------------------------- */
/*                                                                              */
/* Sends uncompressed audio data over IP, from bcast~ to bcreceive~.            */
/*                                                                              */
/* Copyright (C) 2016 Jussi Nieminen.                                           */
/*                                                                              */
/* Based on netsend~ copyright (C) 2004-2005 Olaf Matthes, itself based on        */
/* streamout~ copyright (C) 1999 Guenter Geiger.                                */
/*                                                                              */
/* This file is part of bcast~.                                                 */
/*                                                                              */
/* bcast~ is free software: you can redistribute it and/or modify               */
/* it under the terms of the GNU General Public License as published by         */
/* the Free Software Foundation, either version 3 of the License, or            */
/* (at your option) any later version.                                          */
/*                                                                              */
/* bcast~ is distributed in the hope that it will be useful,                    */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of               */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                */
/* GNU General Public License for more details.                                 */
/*                                                                              */
/* You should have received a copy of the GNU General Public License            */
/* along with bcast~. If not, see <http://www.gnu.org/licenses/>.               */
/*                                                                              */
/* Based on Pure Data by Miller Puckette and others.                            */
/*                                                                              */
/* ---------------------------------------------------------------------------- */

#define VERSION "SD 1.0b"

#define DEFAULT_AUDIO_CHANNELS 32	    /* max. number of audio channels we support */
#define DEFAULT_AUDIO_SOURCES 16
#define DEFAULT_AUDIO_BUFFER_SIZE 64	/* number of samples in one audio block */
#define DEFAULT_UDP_PACKT_SIZE 1472		/* number of bytes we send in one UDP datagram excluding UDP and IP header size (OS X only) */
#define DEFAULT_PORT 8000               /* default network port number */

#ifdef _WINDOWS
#ifndef HAVE_INT32_T
typedef int int32_t;
#define HAVE_INT32_T
#endif
#ifndef HAVE_INT16_T
typedef short int16_t;
#define HAVE_INT16_T
#endif
#ifndef HAVE_U_INT32_T
typedef unsigned int u_int32_t;
#define HAVE_U_INT32_T
#endif
#ifndef HAVE_U_INT16_T
typedef unsigned short u_int16_t;
#define HAVE_U_INT16_T
#endif
#endif

#ifndef CLIP
#define CLIP(a, lo, hi) ( (a)>(lo)?( (a)<(hi)?(a):(hi) ):(lo) )
#endif

/* For the __u32 datatype */
//#include <linux/types.h>

/* swap 32bit t_float. Is there a better way to do that???? */
#ifdef _WINDOWS
__inline static float bcast_float(float f)
#else
inline static float bcast_float(float f)
#endif
{
    union
    {
        float f;
        unsigned char b[4];
    } dat1, dat2;
    
    dat1.f = f;
    dat2.b[0] = dat1.b[3];
    dat2.b[1] = dat1.b[2];
    dat2.b[2] = dat1.b[1];
    dat2.b[3] = dat1.b[0];
    return dat2.f;
}

/* swap 32bit long int */
#ifdef _WINDOWS
__inline static long bcast_long(long n)
#else
inline static uint32_t bcast_long(uint32_t n)
#endif
{
    return (((n & 0xff) << 24) | ((n & 0xff00) << 8) |
    	((n & 0xff0000) >> 8) | ((n & 0xff000000) >> 24));
}

/* swap 16bit short int */
#ifdef _WINDOWS
__inline static short bcast_short(short n)
#else
inline static short bcast_short(short n)
#endif
{
    return (((n & 0xff) << 8) | ((n & 0xff00) >> 8));
}


/* format specific stuff */

#define SF_FLOAT  1
#define SF_DOUBLE 2		/* not implemented */
#define SF_8BIT   10
#define SF_16BIT  11
#define SF_32BIT  12	/* not implemented */
#define SF_ALAW   20	/* not implemented */
#define SF_MP3    30    /* not implemented */
#define SF_AAC    31    /* AAC encoding using FAAC */
#define SF_VORBIS 40	/* not implemented */
#define SF_FLAC   50	/* not implemented */

#define SF_SIZEOF(a) (a == SF_FLOAT ? sizeof(t_float) : \
                     a == SF_16BIT ? sizeof(short) : 1)


/* version / byte-endian specific stuff */

#define SF_BYTE_LE 1		/* little endian */
#define SF_BYTE_BE 2		/* big endian */

#ifdef __APPLE__
#	ifdef __i386__
#		define SF_BYTE_NATIVE SF_BYTE_LE
#	elif __ppc__
#		define SF_BYTE_NATIVE SF_BYTE_BE
#	elif __x86_64__
#		define SF_BYTE_NATIVE SF_BYTE_BE
#	elif __ppc_64__
#		define SF_BYTE_NATIVE SF_BYTE_BE
#	endif
#else //All others
#	define SF_BYTE_NATIVE SF_BYTE_LE
#endif

#define MAX_PARAMETERS ( 4 * DEFAULT_AUDIO_SOURCES )

typedef struct _parameter {		/* size (bytes) */
	uint32_t 	ir_length;		/*		4		*/
	char		ir_changed;		/*		1		*/
	char		device;			/*		1		*/
	char		channel;		/*		1		*/
	char		bypass;			/*		1		*/
} t_parameter;					/*		= 8		*/


typedef struct _tag {								/* size (bytes)	*/
	t_parameter parameters[DEFAULT_AUDIO_CHANNELS];	/*		32 x 8	*/
	uint32_t count;									/*		4		*/
	uint32_t framesize;   							/*		4		*/
	uint16_t channels;								/*		2		*/
	char version;									/*		1		*/
	char format;									/*		1		*/
} t_tag;											/*		= 268	*/

/* Structs are packed to 4 byte boundary. */

typedef struct _frame {
     t_tag  tag;
     char  *data;
} t_frame;

