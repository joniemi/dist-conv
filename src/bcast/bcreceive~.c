/* ------------------------ bcast~ -------------------------------------------- */
/*                                                                              */
/* Sends uncompressed audio data over IP, from bcast~ to bcreceive~.            */
/*                                                                              */
/* Copyright (C) 2016 Jussi Nieminen.                                           */
/*                                                                              */
/* Based on netsend~ copyright (C) 2008 Remu and 2004-2005 Olaf Matthes,        */
/* originally based on streamout~ copyright (C) 1999 Guenter Geiger.            */
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


#ifdef PD
#include "m_pd.h"
#else
#include "ext.h"
#include "z_dsp.h"
#endif

#include "bcast~.h"

#ifdef USE_FAAC
#include "faad/faad.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#ifdef UNIX
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#define SOCKET_ERROR -1
#else
#include <winsock.h>
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifdef NT
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#define DEFAULT_AUDIO_BUFFER_FRAMES 16	/* a small circ. buffer for 16 frames */
#define DEFAULT_AVERAGE_NUMBER 10		/* number of values we store for average history */
#define DEFAULT_NETWORK_POLLTIME 1		/* interval in ms for polling for input data (Max/MSP only) */
#define DEFAULT_QUEUE_LENGTH 3			/* min. number of buffers that can be used reliably on your hardware */

#ifdef UNIX
#define CLOSESOCKET(fd) close(fd)
#endif
#ifdef _WINDOWS
#define CLOSESOCKET(fd) closesocket(fd)
#endif
#ifdef __APPLE__
#import <mach/mach_time.h>
#endif

#define TRUE 1
#define FALSE 0
typedef int BOOL;
typedef int SOCKET;

#ifdef PD
/* these would require to include some headers that are different
   between pd 0.36 and later, so it's easier to do it like this! */
EXTERN void sys_rmpollfn(int fd);
EXTERN void sys_addpollfn(int fd, void* fn, void *ptr);
#endif

#define NB_CHAR_LINE 8

static int bcreceive_tilde_sockerror(char *s)
{
#ifdef NT
    int err = WSAGetLastError();
    if (err == 10054) return 1;
    else if (err == 10040)
	{
		post("bcreceive~: %s: message too long (%d)", s, err);
		return 1;		//Recoverable error
	}
    else if (err == 10053) post("bcreceive~: %s: software caused connection abort (%d)", s, err);
    else if (err == 10055) post("bcreceive~: %s: no buffer space available (%d)", s, err);
    else if (err == 10060) post("bcreceive~: %s: connection timed out (%d)", s, err);
    else if (err == 10061) post("bcreceive~: %s: connection refused (%d)", s, err);
    else post("bcreceive~: %s: %s (%d)", s, strerror(err), err);
#else
    int err = errno;
    post("bcreceive~: %s: %s (%d)", s, strerror(err), err);
#endif
#ifdef NT
	if (err == WSAEWOULDBLOCK)
#endif
#ifdef UNIX
	if (err == EAGAIN)
#endif
	{
		return 1;		//Recoverable error
	}
	return 0;			//Indicate non-recoverable error
}


static int bcreceive_tilde_setsocketoptions(int sockfd)
{ 
	int sockopt = 1;
	if (setsockopt(sockfd, SOL_IP, TCP_NODELAY, (const char*)&sockopt, sizeof(int)) < 0)
		post("bcreceive~: setsockopt NODELAY failed");

	sockopt = 1;    
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&sockopt, sizeof(int)) < 0)
		post("bcreceive~: setsockopt REUSEADDR failed");

	sockopt = 1;    
	if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (const char*)&sockopt, sizeof(int)) < 0)
		post("bcreceive~: setsockopt BROADCAST failed");
	

	return 0;
}



/* ------------------------ bcreceive~ ----------------------------- */


static t_class *bcreceive_tilde_class;
static t_symbol *ps_format, *ps_channels, *ps_framesize, *ps_overflow, *ps_underflow,
                *ps_queuesize, *ps_average, *ps_sf_float, *ps_sf_16bit, *ps_sf_8bit, 
                *ps_sf_mp3, *ps_sf_aac, *ps_sf_unknown, *ps_bitrate, *ps_hostname,
				*ps_parameter, *ps_nothing;


typedef struct _bcreceive_tilde
{
#ifdef PD
	t_object x_obj;
	t_outlet *x_outlet1;			/* Outlet for TCP/IP state */
	t_outlet *x_outlet2;			/* Outlet for print list */
	t_outlet *x_outlet_parameters;	/* New: outlet for the parameter list */
#else
	t_pxobject x_obj;
	void *x_outlet1;
	void *x_outlet2;
	void *x_outlet_parameters;
	void *x_connectpoll;
	void *x_datapoll;
#endif
	int x_socket;
	int x_connectsocket;
	int x_nconnections;
	int x_ndrops;
	int x_tcp;
	t_symbol *x_hostname;

	/* buffering */
	int x_framein;
	int x_frameout;
	t_frame x_frames[DEFAULT_AUDIO_BUFFER_FRAMES];
	int x_maxframes;
	int32_t x_framecount; /* int32_t so that the size matches with the sender. */
	int x_blocksize;
	int x_blocksperrecv;
	int x_blockssincerecv;

	int32_t x_nbytes;
	int x_counter;
	int x_average[DEFAULT_AVERAGE_NUMBER];
	int x_averagecur;
	int x_underflow;
	int x_overflow;

#ifdef USE_FAAC
    faacDecHandle x_faac_decoder;
    faacDecFrameInfo x_faac_frameInfo;
    faacDecConfigurationPtr x_faac_config;
	int x_faac_init;
	unsigned char x_faac_buf[FAAD_MIN_STREAMSIZE * DEFAULT_AUDIO_CHANNELS];
	unsigned long x_faac_bytes;
#endif

	long x_samplerate;
	int x_noutlets;
	int x_vecsize;
	int device;					// To distinguish Raspberry Pi devices
	t_int **x_myvec;            // vector we pass on to the DSP routine

    BOOL                x_record;
    FILE*			    x_file;
    char				x_data[NB_CHAR_LINE];
	
#ifdef _WINDOWS
    LARGE_INTEGER		x_liStartTime;
#elif __APPLE__
	uint64_t			x_t0;
#endif

} t_bcreceive_tilde;

int recv_w(t_bcreceive_tilde *x, char *tag, SOCKET s, char *buf, int len, int flags)
{
	int                 i = 0;
	int                 j = 0;
	int                 nbChar = 0;
	int                 deltaChar = 0;
	int                 returnValue;

#ifdef _WINDOWS
	LARGE_INTEGER   liFrequency;
	LARGE_INTEGER   liCurrentTime;
#elif __APPLE__
	uint64_t		t1;
	uint64_t		delta;
	double			milliSeconds;
#endif

	returnValue = recv(s, buf, len, flags);

	if (x->x_record && x->x_file)
	{

#ifdef _WINDOWS
		QueryPerformanceFrequency ( &liFrequency );

		if (liFrequency.QuadPart == 0)
			fprintf(x->x_file, "bcreceive~ : Your computer does not support High Resolution Performance counter\n");

		QueryPerformanceCounter ( &liCurrentTime );
#elif __APPLE__
		t1 = mach_absolute_time();
		delta = t1 - x->x_t0;
		
		mach_timebase_info_data_t info;
		mach_timebase_info(&info);

		double nano = 1e-9 * ( (double) info.numer) / ((double) info.denom);
		double seconds = ((double) delta) * nano;

		milliSeconds = 1000.0f * seconds;
#endif

		fprintf (x->x_file, "[");fprintf (x->x_file, tag);
		
#ifdef _WINDOWS
		fprintf (x->x_file, "|t:%8.6fms", ((double)( (liCurrentTime.QuadPart - x->x_liStartTime.QuadPart) * (double)1000.0 / (double)liFrequency.QuadPart )) );
#elif __APPLE__
		fprintf (x->x_file, "|t:%8.6fms", milliSeconds);
#endif

		fprintf (x->x_file, "|length:%i", len);
		fprintf (x->x_file, "]");fprintf (x->x_file, "\n");
		
		j = 1;
		for (i = 1; i <= len; ++i)
		{
			/* Store data from buf to x_data in interavals of 8 (NB_CHAR_LINE). */
			x->x_data[j - 1] = buf[i - 1];

			/* Start a new line if modulus is 0 or i equals len. */			
			if (!(i % NB_CHAR_LINE) || (i == len))
			{
				if (i == len)
					nbChar = i % NB_CHAR_LINE;
				else
					nbChar = NB_CHAR_LINE;
				deltaChar = NB_CHAR_LINE - nbChar;

				fprintf (x->x_file, " | ");
				for (j = 0; j < nbChar; ++j)
					fprintf (x->x_file, "%3x ", (unsigned char)x->x_data[j]);
				for (j = 0; j < deltaChar; ++j)
					fprintf (x->x_file, "    ");
				fprintf (x->x_file, " | ");

				fprintf (x->x_file, "\n");
				
				j = 0;
			}
			++j;
		}
		fprintf (x->x_file, "\n======\n");
	}

	return returnValue;
}

/* prototypes (as needed) */
static void bcreceive_tilde_kick(t_bcreceive_tilde *x);

#ifdef USE_FAAC
/* open encoder and set default values */
static void bcreceive_tilde_faac_open(t_bcreceive_tilde* x)
{
	x->x_faac_decoder = faacDecOpen();
	x->x_faac_config = faacDecGetCurrentConfiguration(x->x_faac_decoder);
	x->x_faac_config->defSampleRate = x->x_samplerate;
	x->x_faac_config->defObjectType = MAIN; // LC;
	x->x_faac_config->outputFormat = FAAD_FMT_FLOAT;
	faacDecSetConfiguration(x->x_faac_decoder, x->x_faac_config);
	x->x_faac_init = 0;
	x->x_faac_bytes = 0;
}

static void bcreceive_tilde_faac_close(t_bcreceive_tilde* x)
{
	if (x->x_faac_decoder != NULL)
		faacDecClose(x->x_faac_decoder);
	x->x_faac_decoder = NULL;
	x->x_faac_init = 0;
}

/* init decoder when we get a new stream */
static int bcreceive_tilde_faac_init(t_bcreceive_tilde* x, int frame)
{
	unsigned long samplerate;
    unsigned char channels;
	long bytes_consumed = 0;

	if ((bytes_consumed = faacDecInit(x->x_faac_decoder, x->x_faac_buf, x->x_faac_bytes, &samplerate, &channels)) < 0)
	{
		faacDecConfigurationPtr config;
		error("bcreceive~: faac: initializing decoder library failed");
		bcreceive_tilde_faac_close(x);
		return -1;
	}
	else if (samplerate != (unsigned long)x->x_samplerate)
	{
		error("bcreceive~: incoming stream has wrong samplerate");
		bcreceive_tilde_faac_close(x);
		return -1;
	}

	/* adjust accumulating AAC buffer */
	memmove(x->x_faac_buf, x->x_faac_buf + bytes_consumed, x->x_faac_bytes - bytes_consumed);
	x->x_faac_bytes -= bytes_consumed;

	x->x_faac_init = 1;	/* indicate that decoder is ready */
	return 0;
}

/* decode AAC using FAAD2 library */
static int bcreceive_tilde_faac_decode(t_bcreceive_tilde* x, int frame)
{
	unsigned int i, ret;
	float *sample_buffer;

	/* open decoder, if not yet done */
	if (x->x_faac_decoder == NULL)
	{
		bcreceive_tilde_faac_open(x);
	}

	/* add new AAC data into buffer */
	memcpy(x->x_faac_buf + x->x_faac_bytes, x->x_frames[frame].data, x->x_frames[frame].tag.framesize);
	x->x_faac_bytes += x->x_frames[frame].tag.framesize;

	/* in case we have more than FAAD_MIN_STREAMSIZE bytes per channel try decoding */
	if (x->x_faac_bytes >= (unsigned long)(FAAD_MIN_STREAMSIZE * x->x_frames[frame].tag.channels))
	{
		/* init decoder, if not yet done */
		if (!x->x_faac_init)
		{
			ret = bcreceive_tilde_faac_init(x, frame);
			if (ret == -1)
			{
				return -1;
			}
		}

		/* decode data */
		memset(&x->x_faac_frameInfo, 0, sizeof(faacDecFrameInfo));
		sample_buffer = (float *)faacDecDecode(x->x_faac_decoder, &x->x_faac_frameInfo, x->x_faac_buf, x->x_faac_bytes);
		if (x->x_faac_frameInfo.error != 0)
		{
			error("bcreceive~: faac: %s", faacDecGetErrorMessage(x->x_faac_frameInfo.error));
			bcreceive_tilde_faac_close(x);
			return -1;
		}

		/* adjust accumulating AAC buffer */
		memmove(x->x_faac_buf, x->x_faac_buf + x->x_faac_frameInfo.bytesconsumed, x->x_faac_bytes - x->x_faac_frameInfo.bytesconsumed);
		x->x_faac_bytes -= x->x_faac_frameInfo.bytesconsumed;

		/* copy decoded PCM samples back to frame */
		memcpy(x->x_frames[frame].data, sample_buffer, x->x_faac_frameInfo.samples * sizeof(float));

		/* return number of decoded PCM samples */
		return x->x_faac_frameInfo.samples * SF_SIZEOF(SF_FLOAT);
	}
	else
	{
		return 0;	/* indicate we didn't get any new audio data */
	}
}
#endif	/* USE_FAAC */




/* remove all pollfunctions and close socket */
static void bcreceive_tilde_closesocket(t_bcreceive_tilde* x)
{
#ifdef PD
	sys_rmpollfn(x->x_socket);
	outlet_float(x->x_outlet1, 0);
#else
	clock_unset(x->x_datapoll);
	outlet_int(x->x_outlet1, 0);
#endif
	CLOSESOCKET(x->x_socket);
	x->x_socket = -1;
}



#ifdef PD
static void bcreceive_tilde_reset(t_bcreceive_tilde* x, t_floatarg buffer)
#else
static void bcreceive_tilde_reset(t_bcreceive_tilde* x, double buffer)
#endif
{
	int i;
	x->x_counter = 0;
	x->x_nbytes = 0;
	x->x_framein = 0;
	x->x_frameout = 0;
	x->x_blockssincerecv = 0;
	x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
#ifdef USE_FAAC
	x->x_faac_bytes = 0;
#endif

	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++)
		x->x_average[i] = x->x_maxframes;
	x->x_averagecur = 0;

	if (buffer == 0.0)	// set default
		x->x_maxframes = DEFAULT_QUEUE_LENGTH;
	else
	{
		buffer = (float)CLIP((float)buffer, 0., 1.);
		x->x_maxframes = (int)(DEFAULT_AUDIO_BUFFER_FRAMES * buffer);
		x->x_maxframes = CLIP(x->x_maxframes, 1, DEFAULT_AUDIO_BUFFER_FRAMES - 1);
		post("bcreceive~: set buffer to %g (%d frames)", buffer, x->x_maxframes);
	}
	x->x_underflow = 0;
	x->x_overflow = 0;
}

#ifdef PD
static void bcreceive_tilde_blocksize(t_bcreceive_tilde *x, t_floatarg blocksize)
#else
static void bcreceive_tilde_blocksize(t_bcreceive_tilde* x, long blocksize)
#endif
{
	x->x_blocksize = (int)blocksize;
	x->x_blockssincerecv = 0;
	x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
	post("bcreceive~ : blocksize set to %i", x->x_blocksize);
}

#ifdef PD
static void bcreceive_tilde_device(t_bcreceive_tilde *x, t_floatarg dev)
#else
static void bcreceive_tilde_device(t_bcreceive_tilde* x, long dev)
#endif
{
	x->device = (int)dev;
	post("bcreceive~ : device set to %i", x->device);
}

static void bcreceive_tilde_open(t_bcreceive_tilde *x, t_symbol* path)
{
    x->x_file = fopen(path->s_name, "w+");
    
    if (x->x_file)          post("bcreceive~ : file %s opened", path->s_name);
    else                    post("bcreceive~ : a problem occured when trying to open the file %s", path->s_name);
}

#ifdef PD
static void bcreceive_tilde_record(t_bcreceive_tilde *x, t_floatarg record)
#else
static void bcreceive_tilde_record(t_bcreceive_tilde *x, long record)
#endif
{
    if (!x->x_file)
    {
        post("bcreceive~ : you need to open a file first");
        return;
    }
    
    //REC 1
    if (record)
    {
        post("bcreceive~ : recording...");
        
        //already recording
        if (x->x_record)    post("bcreceive~ : already recording");
        
        //starts record
        else
        {
            x->x_record = TRUE;
#ifdef _WINDOWS
            QueryPerformanceCounter ( &x->x_liStartTime );  //Timestamp to 0
#elif __APPLE__
			x->x_t0 =  mach_absolute_time();
#endif
        }
    }
    
    //REC 0
    else
    {
        post("bcreceive~ : end of recording...");
        
        //already off
        if (!x->x_record)    post("bcreceive~ : recording is already set to 0");
        
        //ends record
        else
        {
            fclose(x->x_file);
            x->x_record = FALSE;
        }
    }
}


static void bcreceive_tilde_datapoll(t_bcreceive_tilde *x)
{
#ifndef PD
	int ret;
    struct timeval timout;
    fd_set readset;
    timout.tv_sec = 0;
    timout.tv_usec = 0;
    FD_ZERO(&readset);
    FD_SET(x->x_socket, &readset);

	ret = select(x->x_socket + 1, &readset, NULL, NULL, &timout);
    if (ret < 0)
    {
    	bcreceive_tilde_sockerror("select");
		return;
    }

	if (FD_ISSET(x->x_socket, &readset))	/* data available */
#endif
	{
		int ret;
		int n, p;

		if (x->x_tcp)
		{
			n = x->x_nbytes;

			if (x->x_nbytes == 0)	/* we ate all the samples and need a new header tag */
			{
				/* get the new tag */
				ret = recv_w(x, "header", x->x_socket, (char*)&x->x_frames[x->x_framein].tag, sizeof(t_tag), MSG_PEEK);
				
				if (ret == 0)	/* disconnect */
				{
					post("bcreceive~: EOF on socket %d", x->x_socket);
					bcreceive_tilde_closesocket(x);
					x->x_socket = -1;
					x->x_counter = 0;
					return;
				}
				if (ret < 0)	/* error */
				{
					if (bcreceive_tilde_sockerror("recv tag"))
						goto bail;
					bcreceive_tilde_closesocket(x);
					x->x_socket = -1;
					x->x_counter = 0;
					return;
				}
				else if (ret != sizeof(t_tag))
				{
					/* incomplete header tag: return and try again later */
					/* in the hope that more data will be available */
					return;
				}

				/* receive header tag */

				ret = recv_w(x, "header", x->x_socket, (char*)&x->x_frames[x->x_framein].tag, sizeof(t_tag), 0);

				/* adjust byte order if neccessarry */
				if (x->x_frames[x->x_framein].tag.version != SF_BYTE_NATIVE)
				{
					x->x_frames[x->x_framein].tag.count = bcast_long(x->x_frames[x->x_framein].tag.count);
					x->x_frames[x->x_framein].tag.framesize = bcast_long(x->x_frames[x->x_framein].tag.framesize);
					for ( p = 0; p > DEFAULT_AUDIO_CHANNELS; p++)
					{
						x->x_frames[x->x_framein].tag.parameters[p].ir_length =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].ir_length);
						x->x_frames[x->x_framein].tag.parameters[p].ir_changed =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].ir_changed);
						x->x_frames[x->x_framein].tag.parameters[p].device =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].device);
						//x->x_frames[x->x_framein].tag.parameters[p].channel =
						//	bcast_long(x->x_frames[x->x_framein].tag.parameters[p].channel);
						x->x_frames[x->x_framein].tag.parameters[p].bypass =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].bypass);
					}
				}

				/* get info from header tag */
				/*if (x->x_frames[x->x_framein].tag.channels > x->x_noutlets)
				{
					error("bcreceive~: incoming stream has too many channels (%d), kicking client", x->x_frames[x->x_framein].tag.channels);
					bcreceive_tilde_kick(x);
					x->x_socket = -1;
					x->x_counter = 0;
					return;
				}*/
				x->x_nbytes = n = x->x_frames[x->x_framein].tag.framesize;

				/* check whether the data packet has the correct count */
				if ((x->x_framecount != x->x_frames[x->x_framein].tag.count)
					&& (x->x_frames[x->x_framein].tag.count > 2))
				{
					error("bcreceive~: we lost %d frames", (int)(x->x_frames[x->x_framein].tag.count - x->x_framecount));
					post("bcreceive~: current package is %d, expected %d", x->x_frames[x->x_framein].tag.count, x->x_framecount);
				}
				x->x_framecount = x->x_frames[x->x_framein].tag.count + 1;
			}
			else	/* we already have the header tag or some data and need more */
			{

				ret = recv_w(x, "data", x->x_socket, (char*)x->x_frames[x->x_framein].data + x->x_frames[x->x_framein].tag.framesize - n, n, 0);

				if (ret > 0)
				{
					n -= ret;
				}
				else if (ret < 0)	/* error */
				{
					if (bcreceive_tilde_sockerror("recv data"))
						goto bail;
					bcreceive_tilde_closesocket(x);
					x->x_socket = -1;
					x->x_counter = 0;
					return;
				}

				x->x_nbytes = n;
				if (n == 0)			/* a complete packet is received */
				{
#ifdef USE_FAAC     /* decode aac data if format is SF_AAC */
					if (x->x_frames[x->x_framein].tag.format == SF_AAC)
					{
						ret = bcreceive_tilde_faac_decode(x, x->x_framein);
						if (ret == -1)
						{
							bcreceive_tilde_kick(x);
							x->x_socket = -1;
							x->x_counter = 0;
							return;
						}
						else
						{
							/* update framesize */
							x->x_frames[x->x_framein].tag.framesize = ret;
						}
					}
#else
					if (x->x_frames[x->x_framein].tag.format == SF_AAC)
					{
						error("bcreceive~: don't know how to decode AAC format");
						bcreceive_tilde_kick(x);
						x->x_socket = -1;
						x->x_counter = 0;
						return;
					}
#endif

					x->x_counter++;
					x->x_framein++;
					x->x_framein %= DEFAULT_AUDIO_BUFFER_FRAMES;

					/* check for buffer overflow */
					if (x->x_framein == x->x_frameout)
					{
						x->x_overflow++;
					}
				}
			}
		}
		else /* UDP */
		{
			n = x->x_nbytes;

			if (x->x_nbytes == 0)	/* we ate all the samples and need a new header tag */
			{
				/* receive header tag */
				
				memset( (char*)&x->x_frames[x->x_framein].tag, 0, sizeof(t_tag) );

				ret = recv_w(x ,"header", x->x_socket, (char*)&x->x_frames[x->x_framein].tag, sizeof(t_tag), 0);
				
				if (ret <= 0)	/* error */
				{
					if (bcreceive_tilde_sockerror("recv tag"))
						goto bail;

					bcreceive_tilde_reset(x, 0);
					x->x_counter = 0;
					return;
				}
				else if (ret != sizeof(t_tag))
				{
					/* incomplete header tag: return and try again later */
					/* in the hope that more data will be available */
					post("Ret: %d t_tag: %d", ret, sizeof(t_tag));
					error("bcreceive~: got incomplete header tag");
					return;
				}
				/* adjust byte order if necessary */
				if (x->x_frames[x->x_framein].tag.version != SF_BYTE_NATIVE)
				{
					x->x_frames[x->x_framein].tag.count = bcast_long(x->x_frames[x->x_framein].tag.count);
					x->x_frames[x->x_framein].tag.framesize = bcast_long(x->x_frames[x->x_framein].tag.framesize);
					for ( p = 0; p > DEFAULT_AUDIO_CHANNELS; p++)
					{
						x->x_frames[x->x_framein].tag.parameters[p].ir_length =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].ir_length);
						x->x_frames[x->x_framein].tag.parameters[p].ir_changed =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].ir_changed);
						x->x_frames[x->x_framein].tag.parameters[p].device =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].device);
						//x->x_frames[x->x_framein].tag.parameters[p].channel =
							//bcast_long(x->x_frames[x->x_framein].tag.parameters[p].channel);
						x->x_frames[x->x_framein].tag.parameters[p].bypass =
							bcast_long(x->x_frames[x->x_framein].tag.parameters[p].bypass);
					}
				}
				/* get info from header tag */
				/*if (x->x_frames[x->x_framein].tag.channels > x->x_noutlets)
				{
					error("bcreceive~: incoming stream has too many channels (%d)", x->x_frames[x->x_framein].tag.channels);
					x->x_counter = 0;
					return;
				}*/
				x->x_nbytes = n = x->x_frames[x->x_framein].tag.framesize;

			}
			else	/* we already have header tag or some data and need more */
			{
				ret = recv_w(x, "data", x->x_socket, (char*)x->x_frames[x->x_framein].data + x->x_frames[x->x_framein].tag.framesize - n, n, 0);

				if (ret > 0)
				{
					n -= ret;
				}
				else if (ret < 0)	/* error */
				{
					if (bcreceive_tilde_sockerror("recv data"))
					{
						bcreceive_tilde_reset(x, 0);
						goto bail;
					}
					bcreceive_tilde_reset(x, 0);
					x->x_counter = 0;
					return;
				}

				x->x_nbytes = n;
				if (n == 0)			/* a complete packet is received */
				{
#ifdef USE_FAAC     /* decode aac data if format is SF_AAC and update framesize */
					if (x->x_frames[x->x_framein].tag.format == SF_AAC)
					{
						ret = bcreceive_tilde_faac_decode(x, x->x_framein);
						if (ret == -1)
						{
							return;
						}
						else
						{
							/* update framesize */
							x->x_frames[x->x_framein].tag.framesize = ret;
						}
					}
#else
					if (x->x_frames[x->x_framein].tag.format == SF_AAC)
					{
						error("bcreceive~: don't know how to decode AAC format");
						return;
					}
#endif
					x->x_counter++;
					x->x_framein++;
					x->x_framein %= DEFAULT_AUDIO_BUFFER_FRAMES;

					/* check for buffer overflow */
					if (x->x_framein == x->x_frameout)
					{
						x->x_overflow++;
					}
				}
			}
		}
	}
bail:
	;
#ifndef PD
	clock_delay(x->x_datapoll, DEFAULT_NETWORK_POLLTIME);
#endif
}


static void bcreceive_tilde_connectpoll(t_bcreceive_tilde *x)
{
#ifndef PD
	int ret;
    struct timeval timout;
    fd_set readset;
    timout.tv_sec = 0;
    timout.tv_usec = 0;
    FD_ZERO(&readset);
    FD_SET(x->x_connectsocket, &readset);

	ret = select(x->x_connectsocket + 1, &readset, NULL, NULL, &timout);
    if (ret < 0)
    {
    	bcreceive_tilde_sockerror("select");
		return;
    }

	if (FD_ISSET(x->x_connectsocket, &readset))	/* pending connection */
#endif
	{
		int sockaddrl = (int)sizeof(struct sockaddr);
		struct sockaddr_in incomer_address;
		int fd = accept(x->x_connectsocket, (struct sockaddr*)&incomer_address, &sockaddrl);
		if (fd < 0) 
		{
			post("bcreceive~: accept failed");
			return;
		}
#ifdef O_NONBLOCK
		fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
		if (x->x_socket != -1)
		{
			post("bcreceive~: new connection");
			bcreceive_tilde_closesocket(x);
		}

		bcreceive_tilde_reset(x, 0);
		x->x_socket = fd;
		x->x_nbytes = 0;
		x->x_hostname = gensym(inet_ntoa(incomer_address.sin_addr));
#ifdef PD
		sys_addpollfn(fd, bcreceive_tilde_datapoll, x);
		outlet_float(x->x_outlet1, 1);
#else
		clock_delay(x->x_datapoll, 0);
		outlet_int(x->x_outlet1, 1);
#endif
	}
#ifndef PD
	clock_delay(x->x_connectpoll, DEFAULT_NETWORK_POLLTIME);
#endif
}


static int bcreceive_tilde_createsocket(t_bcreceive_tilde* x, int portno)
{
    struct sockaddr_in server;
    int sockfd;
    int tcp = x->x_tcp;

     /* create a socket */
    if (!tcp)
      sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    else
      sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        bcreceive_tilde_sockerror("socket");
        return 0;
    }
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;

    /* assign server port number */

    server.sin_port = htons((u_short)portno);
    post("listening to port number %d", portno);

    bcreceive_tilde_setsocketoptions(sockfd);

    /* name the socket */
    if (bind(sockfd, (struct sockaddr *)&server, sizeof(server)) < 0)
	{
         bcreceive_tilde_sockerror("bind");
         CLOSESOCKET(sockfd);
         return 0;
    }


    if (!tcp)
	{
         x->x_socket = sockfd;
         x->x_nbytes = 0;
#ifdef PD
         sys_addpollfn(sockfd, bcreceive_tilde_datapoll, x);
#else
		 clock_delay(x->x_datapoll, 0);
#endif
    }
    else
	{
         if (listen(sockfd, 5) < 0)
		 {
              bcreceive_tilde_sockerror("listen");
              CLOSESOCKET(sockfd);
			  return 0;
         }
         else
		 {
              x->x_connectsocket = sockfd;
			  /* start polling for connection requests */
#ifdef PD
              sys_addpollfn(sockfd, bcreceive_tilde_connectpoll, x);
#else
			  clock_delay(x->x_connectpoll, 0);
#endif
         }
    }
    return 1;
}



/* kick connected client */
static void bcreceive_tilde_kick(t_bcreceive_tilde *x)
{
	if (x->x_tcp)
	{
		if (x->x_socket != -1)
		{
			shutdown(x->x_socket, 1);
			bcreceive_tilde_closesocket(x);
			post("bcreceive~: kicked client!");
		}
		else error("bcreceive~: no client to kick");
	}
	else error("bcreceive~: kicking clients in UDP mode not possible");
}


#define QUEUESIZE (int)((x->x_framein + DEFAULT_AUDIO_BUFFER_FRAMES - x->x_frameout) % DEFAULT_AUDIO_BUFFER_FRAMES)
#define BLOCKOFFSET (x->x_blockssincerecv * x->x_vecsize * x->x_frames[x->x_frameout].tag.channels)

static t_int *bcreceive_tilde_perform(t_int *w)
{
	/* The pointer to the object struct */ 	
	t_bcreceive_tilde *x = (t_bcreceive_tilde*) (w[1]);
	/* The length of the signal vec */
	int n = (int)(w[2]);
	/* The out channels vec */	
	t_float *out[DEFAULT_AUDIO_CHANNELS];

	const int offset = 3;
	const int channels = x->x_frames[x->x_frameout].tag.channels;
	/* input + ir pairs. Floors the result (values are integers). */
	const int sources = channels / 2;
	/* Three parameters per source. 1) IR length 2) IR changed 3) bypass flag */
	const int number_of_parameters = 3 * sources;

	/* The parameter list */
	t_atom param_list[number_of_parameters];

	int i;
	int src_count = 0;
	int param_idx = 0;
	int src_offset = sources;
	
	for (i = 0; i < x->x_noutlets; i++)
	{
		out[i] = (t_float *)(w[offset + i]);
	}

	/* Update the data vector size */
	if (n != x->x_vecsize)
	{
		x->x_vecsize = n;
		x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
		x->x_blockssincerecv = 0;
	}

	/* check whether there is enough data in buffer */
	if (x->x_counter < x->x_maxframes)
	{
		goto bail;
	}

	/* check for buffer underflow */
	if (x->x_framein == x->x_frameout)
	{
		x->x_underflow++;
		goto bail;
	}

	/* queue balancing */
	x->x_average[x->x_averagecur] = QUEUESIZE;
	if (++x->x_averagecur >= DEFAULT_AVERAGE_NUMBER)
		x->x_averagecur = 0;

	/*** The process block ***/
	switch (x->x_frames[x->x_frameout].tag.format)
	{
		case SF_FLOAT:
		{
			t_float* buf = (t_float *)x->x_frames[x->x_frameout].data + BLOCKOFFSET;

			for ( i = 0; i < sources; i++ )
			{
				if ( x->x_frames[x->x_frameout].tag.parameters[i].device == x->device )
				{
					/* Check byte order before inserting samples. */
					if (x->x_frames[x->x_frameout].tag.version == SF_BYTE_NATIVE)
					{
						while (n--)
						{
							*out[src_count]++ = *buf;
							buf += sources;
							*out[src_offset]++ = *buf;
							buf += sources;
						}
					}
					else
					{
						while (n--)
						{
							*out[src_count]++ = bcast_float(*buf);
							buf += sources;
							*out[src_offset]++ = bcast_float(*buf);
							buf += sources;
						}
					}

					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_length );
					SETFLOAT( &param_list[param_idx+1], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_changed );
					SETFLOAT( &param_list[param_idx+2], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].bypass );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
				/* Break if too many sources than outlets. */
				if ( src_count > sources )
					break;

				buf++;
			}
			/* 0s to the rest of the channels, if less sources than outlets. */
			if ( src_count < sources )
			{
				while ( src_count < sources )
				{
					while (n--)
					{
						*out[src_count]++ = 0.;
						*out[src_offset]++ = 0.;
					}

					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], 0. );
					SETFLOAT( &param_list[param_idx+1], 0. );
					SETFLOAT( &param_list[param_idx+2], 0. );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
			}
			break;
		}
		case SF_16BIT:     
		{
			short* buf = (short *)x->x_frames[x->x_frameout].data + BLOCKOFFSET;

			for ( i = 0; i < sources; i++ )
			{
				if ( x->x_frames[x->x_frameout].tag.parameters[i].device == x->device )
				{
					if (x->x_frames[x->x_frameout].tag.version == SF_BYTE_NATIVE)
					{
						while (n--)
						{
							*out[src_count]++ = (t_float)(*buf * 3.051850e-05);
							buf += sources;
							*out[src_offset]++ = (t_float)(*buf * 3.051850e-05);
							buf += sources;
						}
					}
					else
					{
						while (n--)
						{
							*out[src_count]++ = (t_float)(bcast_float(*buf) * 3.051850e-05);
							buf += sources;
							*out[src_offset]++ = (t_float)(bcast_float(*buf) * 3.051850e-05);
							buf += sources;
						}
					}

					SETFLOAT( &param_list[param_idx], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_length );
					SETFLOAT( &param_list[param_idx+1], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_changed );
					SETFLOAT( &param_list[param_idx+2], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].bypass );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
				if ( src_count > sources )
					break;

				buf++;
			}
			if ( src_count < sources )
			{
				while ( src_count < sources )
				{
					while (n--)
					{
						*out[src_count]++ = 0.;
						*out[src_offset]++ = 0.;
					}
					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], 0. );
					SETFLOAT( &param_list[param_idx+1], 0. );
					SETFLOAT( &param_list[param_idx+2], 0. );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
			}
			break;

		}
		case SF_8BIT:     
		{
			unsigned char* buf = (unsigned char *)x->x_frames[x->x_frameout].data + BLOCKOFFSET;

			for ( i = 0; i < sources; i++ )
			{
				if ( x->x_frames[x->x_frameout].tag.parameters[i].device == x->device )
				{
					while (n--)
					{
						*out[src_count]++ = (t_float)((0.0078125 * (*buf)) - 1.0);
						buf += sources;
						*out[src_offset]++ = (t_float)((0.0078125 * (*buf)) - 1.0);
						buf += sources;
					}

					SETFLOAT( &param_list[param_idx], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_length );
					SETFLOAT( &param_list[param_idx+1], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_changed );
					SETFLOAT( &param_list[param_idx+2], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].bypass );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
				if ( src_count > sources )
					break;

				buf++;
			}
			if ( src_count < sources )
			{
				while ( src_count < sources )
				{
					while (n--)
					{
						*out[src_count]++ = 0.;
						*out[src_offset]++ = 0.;
					}
					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], 0. );
					SETFLOAT( &param_list[param_idx+1], 0. );
					SETFLOAT( &param_list[param_idx+2], 0. );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
			}
			break;
		}
		case SF_MP3:     
		{
			post("bcreceive~: mp3 format not supported");
			if (x->x_tcp)
				bcreceive_tilde_kick(x);
			break;
		}
		case SF_AAC:     
		{
#ifdef USE_FAAC
			t_float* buf = (t_float *)x->x_frames[x->x_frameout].data + BLOCKOFFSET;

			for ( i = 0; i < sources; i++ )
			{
				if ( x->x_frames[x->x_frameout].tag.parameters[i].device == x->device )
				{
					while (n--)
					{
						*out[src_count]++ = *buf;
						buf += sources;
						*out[src_offset]++ = *buf;
						buf += sources;
					}

					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_length );
					SETFLOAT( &param_list[param_idx+1], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].ir_changed );
					SETFLOAT( &param_list[param_idx+2], ( t_float )x->x_frames[x->x_frameout].tag.parameters[i].bypass );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
				/* Break if too many sources than outlets. */
				if ( src_count > sources )
					break;

				buf++;
			}
			/* 0s to the rest of the channels, if less sources than outlets. */
			if ( src_count < sources )
			{
				while ( src_count < sources )
				{
					while (n--)
					{
						*out[src_count]++ = 0.;
						*out[src_offset]++ = 0.;
					}

					/* Set values to parameter vector. */
					SETFLOAT( &param_list[param_idx], 0. );
					SETFLOAT( &param_list[param_idx+1], 0. );
					SETFLOAT( &param_list[param_idx+2], 0. );

					param_idx += 3;
					src_count++;
					src_offset++;
					n = x->x_vecsize;
				}
			}
			break;
#else
			post("bcreceive~: aac format not supported");
			if (x->x_tcp)
				bcreceive_tilde_kick(x);
#endif
			break;
		}
		default:
			post("bcreceive~: unknown format (%d)",x->x_frames[x->x_frameout].tag.format);
			if (x->x_tcp)
				bcreceive_tilde_kick(x);
			break;
	}

	/* Send the parameter vector. */
	outlet_list(x->x_outlet_parameters, NULL, number_of_parameters, param_list);

	if (!(x->x_blockssincerecv < x->x_blocksperrecv - 1))
	{
		x->x_blockssincerecv = 0;
		x->x_frameout++;
		x->x_frameout %= DEFAULT_AUDIO_BUFFER_FRAMES;
	}
	else
	{
		x->x_blockssincerecv++;
	}

	return (w + offset + x->x_noutlets);

bail:
	/* set output to zero */
	while (n--)
	{
		for (i = 0; i < x->x_noutlets; i++)
		{
			*(out[i]++) = 0.;
		}
	}
	return (w + offset + x->x_noutlets);
}



static void bcreceive_tilde_dsp(t_bcreceive_tilde *x, t_signal **sp)
{
	int i;

	x->x_myvec[0] = (t_int*)x;
	x->x_myvec[1] = (t_int*)sp[0]->s_n; // The length of the signal vec

	x->x_samplerate = (long)sp[0]->s_sr;

	if (x->x_blocksize % sp[0]->s_n)
	{
		error("bcast~: signal vector size too large (needs to be even divisor of %d)", x->x_blocksize);
	}
	else
	{
#ifdef PD
		for (i = 0; i < x->x_noutlets; i++)
		{
			x->x_myvec[2 + i] = (t_int*)sp[i + 1]->s_vec;
		}
		dsp_addv(bcreceive_tilde_perform, x->x_noutlets + 2, (t_int*)x->x_myvec);
#else
		for (i = 0; i < x->x_noutlets; i++)
		{
			x->x_myvec[2 + i] = (t_int*)sp[i]->s_vec;
		}
		dsp_addv(bcreceive_tilde_perform, x->x_noutlets + 2, (void **)x->x_myvec);
#endif	/* PD */
	}
}


/* send stream info when banged */
static void bcreceive_tilde_bang(t_bcreceive_tilde *x)
{
	t_atom list[2];
	t_atom param_list[MAX_PARAMETERS];
	t_symbol *sf_format;
	t_float bitrate;
	int i, avg = 0;
	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++)
		avg += x->x_average[i];

	bitrate = (t_float)((SF_SIZEOF(x->x_frames[x->x_frameout].tag.format) * x->x_samplerate * 8 * x->x_frames[x->x_frameout].tag.channels) / 1000.);

	switch (x->x_frames[x->x_frameout].tag.format)
	{
		case SF_FLOAT:
		{
			sf_format = ps_sf_float;
			break;
		}
		case SF_16BIT:
		{
			sf_format = ps_sf_16bit;
			break;
		}
		case SF_8BIT:
		{
			sf_format = ps_sf_8bit;
			break;
		}
		case SF_MP3:
		{
			sf_format = ps_sf_mp3;
			break;
		}
		case SF_AAC:
		{
			sf_format = ps_sf_aac;
			break;
		}
		default:
		{
			sf_format = ps_sf_unknown;
			break;
		}
	}

#ifdef PD
	/* --- stream information (t_tag) --- */
	/* audio format */
	SETSYMBOL(list, (t_symbol *)sf_format);
	outlet_anything(x->x_outlet2, ps_format, 1, list);

	/* channels */
	SETFLOAT(list, (t_float)x->x_frames[x->x_frameout].tag.channels);
	outlet_anything(x->x_outlet2, ps_channels, 1, list);

	/* framesize */
	SETFLOAT(list, (t_float)x->x_frames[x->x_frameout].tag.framesize);
	outlet_anything(x->x_outlet2, ps_framesize, 1, list);

	/* bitrate */
	SETFLOAT(list, (t_float)bitrate);
	outlet_anything(x->x_outlet2, ps_bitrate, 1, list);

	/* --- internal info (buffer and network) --- */
	/* overflow */
	SETFLOAT(list, (t_float)x->x_overflow);
	outlet_anything(x->x_outlet2, ps_overflow, 1, list);

	/* underflow */
	SETFLOAT(list, (t_float)x->x_underflow);
	outlet_anything(x->x_outlet2, ps_underflow, 1, list);

	/* queuesize */
	SETFLOAT(list, (t_float)QUEUESIZE);
	outlet_anything(x->x_outlet2, ps_queuesize, 1, list);

	/* average queuesize */
	SETFLOAT(list, (t_float)((t_float)avg / (t_float)DEFAULT_AVERAGE_NUMBER));
	outlet_anything(x->x_outlet2, ps_average, 1, list);

	if (x->x_tcp)
	{
		/* IP address */
		SETSYMBOL(list, (t_symbol *)x->x_hostname);
		outlet_anything(x->x_outlet2, ps_hostname, 1, list);
	}

	/* parameter vector */
	int s = 0;
	for ( i = 0; i < MAX_PARAMETERS; i += 4 )
	{
		SETFLOAT( &param_list[i], ( t_float )x->x_frames[x->x_frameout].tag.parameters[s].ir_length );
		SETFLOAT( &param_list[i+1], ( t_float )x->x_frames[x->x_frameout].tag.parameters[s].ir_changed );
		SETFLOAT( &param_list[i+2], ( t_float )x->x_frames[x->x_frameout].tag.parameters[s].device );
		SETFLOAT( &param_list[i+3], ( t_float )x->x_frames[x->x_frameout].tag.parameters[s].bypass );
		//SETFLOAT( &param_list[i+4], ( t_float )x->x_frames[x->x_frameout].tag.parameters[s].channel );	
		s++;
	}
	outlet_anything(x->x_outlet2, ps_parameter, MAX_PARAMETERS, param_list);

#else
	/* --- stream information (t_tag) --- */
	/* audio format */
	SETSYM(list, ps_format);
	SETSYM(list + 1, (t_symbol *)sf_format);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* channels */
	SETSYM(list, ps_channels);
	SETLONG(list + 1, (int)x->x_frames[x->x_frameout].tag.channels);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* framesize */
	SETSYM(list, ps_framesize);
	SETLONG(list + 1, (int)x->x_frames[x->x_frameout].tag.framesize);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* bitrate */
	SETSYM(list, ps_bitrate);
	SETFLOAT(list + 1, (t_float)bitrate);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* --- internal info (buffer and network) --- */
	/* overflow */
	SETSYM(list, ps_overflow);
	SETLONG(list + 1, (int)x->x_overflow);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* underflow */
	SETSYM(list, ps_underflow);
	SETLONG(list + 1, (int)x->x_underflow);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* queuesize */
	SETSYM(list, ps_queuesize);
	SETLONG(list + 1, (int)QUEUESIZE);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* average queuesize */
	SETSYM(list, ps_average);
	SETFLOAT(list + 1, (t_float)((t_float)avg / (t_float)DEFAULT_AVERAGE_NUMBER));
	outlet_list(x->x_outlet2, NULL, 2, list);

	if (x->x_tcp)
	{
		/* IP address */
		SETSYM(list, (t_symbol *)ps_hostname);
		SETSYM(list + 1, x->x_hostname);
		outlet_list(x->x_outlet2, NULL, 2, list);
	}

	/* parameter vector */
	for ( i = 0; i < DEFAULT_AUDIO_CHANNELS; i++ )
	{
		SETSYM( list, (t_symbol *)ps_parameter );
		SETLONG( list, ( t_float ) x->x_frames[x->x_frameout].tag.parameters[i].ir_length );
		outlet_list( x->x_outlet2, NULL, 2, list);
		
		SETSYM( list, (t_symbol *)ps_parameter );
		SETLONG( list, ( t_float ) x->x_frames[x->x_frameout].tag.parameters[i].ir_changed );
		outlet_list( x->x_outlet2, NULL, 2, list);

		SETSYM( list, (t_symbol *)ps_parameter );
		SETLONG( list, ( t_float ) x->x_frames[x->x_frameout].tag.parameters[i].device );
		outlet_list( x->x_outlet2, NULL, 2, list);

		//SETSYM( list, (t_symbol *)ps_parameter );
		//SETLONG( list, ( t_float ) x->x_frames[x->x_frameout].tag.parameters[i].channel );
		//outlet_list( x->x_outlet2, NULL, 2, list);

		SETSYM( list, (t_symbol *)ps_parameter );
		SETLONG( list, ( t_float ) x->x_frames[x->x_frameout].tag.parameters[i].bypass );
		outlet_list( x->x_outlet2, NULL, 2, list);
	}

#endif
}



static void bcreceive_tilde_print(t_bcreceive_tilde* x)
{
	int i, avg = 0;
	for (i = 0; i < DEFAULT_AVERAGE_NUMBER; i++)
		avg += x->x_average[i];
	post("bcreceive~: last size = %d, avg size = %g, %d underflows, %d overflows", QUEUESIZE, (float)((float)avg / (float)DEFAULT_AVERAGE_NUMBER), x->x_underflow, x->x_overflow);
	post("bcreceive~: channels = %d, framesize = %d, packets = %d", x->x_frames[x->x_framein].tag.channels, x->x_frames[x->x_framein].tag.framesize, x->x_counter);
	for ( i = 0; i < DEFAULT_AUDIO_CHANNELS; i++ )
	{
		post("bcreceive~: device #%d: ir_length: %d ir_changed: %d bypass: %d",
			(int)x->x_frames[x->x_framein].tag.parameters[i].device,
			(int)x->x_frames[x->x_framein].tag.parameters[i].ir_length,
			(int)x->x_frames[x->x_framein].tag.parameters[i].ir_changed,
			(int)x->x_frames[x->x_framein].tag.parameters[i].bypass);
	}
}



#ifdef PD
static void *bcreceive_tilde_new(t_floatarg fportno, t_floatarg outlets, t_floatarg prot)
#else
static void *bcreceive_tilde_new(long fportno, long outlets, long prot)
#endif
{
	t_bcreceive_tilde *x;
	int i;

	if (fportno == 0) fportno = DEFAULT_PORT;

#ifdef PD
	x = (t_bcreceive_tilde *)pd_new(bcreceive_tilde_class);
    if (x)
    { 
        for (i = sizeof(t_object); i < (int)sizeof(t_bcreceive_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	x->x_noutlets = CLIP((int)outlets, 1, DEFAULT_AUDIO_CHANNELS);
	for (i = 0; i < x->x_noutlets; i++)
		outlet_new(&x->x_obj, &s_signal);
	if (!prot)
		x->x_outlet1 = outlet_new(&x->x_obj, &s_anything);	/* outlet for connection state (TCP/IP) */

	/* outlet for parameter list */
	x->x_outlet_parameters = outlet_new(&x->x_obj, &s_float);	

	/* outlet for printing */
	x->x_outlet2 = outlet_new(&x->x_obj, &s_anything);

#else
	x = (t_bcreceive_tilde *)newobject(bcreceive_tilde_class);
    if (x)
    { 
        for (i = sizeof(t_pxobject); i < (int)sizeof(t_bcreceive_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	/* no signal inlets */
	dsp_setup((t_pxobject *)x, 0);	
	x->x_noutlets = CLIP((int)outlets, 1, DEFAULT_AUDIO_CHANNELS);
	
	/* outlet for info list */
	x->x_outlet2 = listout(x);

	/* outlet for parameter list */	
	//x->x_outlet_irlength = outlet_new(x, "int");
	x->x_outlet_parameters = listout(x);

	/* outlet for connection state (TCP/IP) */
	if (!prot)
		x->x_outlet1 = listout(x);

	/* Signal outlets */	
	for (i = 0 ; i < x->x_noutlets; i++)
		outlet_new(x, "signal");

	x->x_connectpoll = clock_new(x, (method)bcreceive_tilde_connectpoll);
	x->x_datapoll = clock_new(x, (method)bcreceive_tilde_datapoll);
#endif

	x->x_myvec = (t_int **)t_getbytes(sizeof(t_int *) * (x->x_noutlets + 3));
	if (!x->x_myvec)
	{
		error("bcreceive~: out of memory");
		return NULL;
	}

#ifdef USE_FAAC
	x->x_faac_decoder = NULL;
	x->x_faac_init = 0;
#endif

	x->x_connectsocket = -1;
	x->x_socket = -1;
	x->x_tcp = 1;
	x->x_nconnections = 0;
	x->x_ndrops = 0;
	x->x_underflow = 0;
	x->x_overflow = 0;
	x->x_hostname = ps_nothing;
	for (i = 0; i < DEFAULT_AUDIO_BUFFER_FRAMES; i++)
	{
		x->x_frames[i].data = (char *)t_getbytes(DEFAULT_AUDIO_BUFFER_SIZE * x->x_noutlets * sizeof(t_float));
	}
	x->x_framein = 0;
	x->x_frameout = 0;
	x->x_maxframes = DEFAULT_QUEUE_LENGTH;
	x->x_vecsize = 64;	/* we'll update this later */
	x->x_blocksize = DEFAULT_AUDIO_BUFFER_SIZE;	/* LATER make this dynamic */
	x->x_blockssincerecv = 0;
	x->x_blocksperrecv = x->x_blocksize / x->x_vecsize;
    x->x_record = FALSE;
	x->x_file = NULL;

	if (prot)
		x->x_tcp = 0;

	if (!bcreceive_tilde_createsocket(x, (int)fportno))
	{
		error("bcreceive~: failed to create listening socket");
		return (NULL);
	}

	return (x);
}



static void bcreceive_tilde_free(t_bcreceive_tilde *x)
{
	int i;

	if (x->x_connectsocket != -1)
	{
#ifdef PD
		sys_rmpollfn(x->x_connectsocket);
#else
		clock_unset(x->x_connectpoll);
#endif
		CLOSESOCKET(x->x_connectsocket);
	}
	if (x->x_socket != -1)
	{
#ifdef PD
		sys_rmpollfn(x->x_socket);
#else
		clock_unset(x->x_datapoll);
#endif
		CLOSESOCKET(x->x_socket);
	}

#ifndef PD
	dsp_free((t_pxobject *)x);	/* free the object */
	clock_free((t_object *)x->x_connectpoll);
	clock_free((t_object *)x->x_datapoll);
#endif

#ifdef USE_FAAC
	bcreceive_tilde_faac_close(x);
#endif

	/* free memory */
	t_freebytes(x->x_myvec, sizeof(t_int *) * (x->x_noutlets + 3));
	for (i = 0; i < DEFAULT_AUDIO_BUFFER_FRAMES; i++)
	{
		 t_freebytes(x->x_frames[i].data, DEFAULT_AUDIO_BUFFER_SIZE * x->x_noutlets * sizeof(t_float));
	}
}



#ifdef PD
void bcreceive_tilde_setup(void)
{
	bcreceive_tilde_class = class_new(gensym("bcreceive~"), 
		(t_newmethod) bcreceive_tilde_new, (t_method) bcreceive_tilde_free,
		sizeof(t_bcreceive_tilde),  0, A_DEFFLOAT, A_DEFFLOAT, A_DEFFLOAT, A_NULL);

	class_addmethod(bcreceive_tilde_class, nullfn, gensym("signal"), (t_atomtype)0);
	class_addbang(bcreceive_tilde_class, (t_method)bcreceive_tilde_bang);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_dsp, gensym("dsp"), (t_atomtype)0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_print, gensym("print"), (t_atomtype)0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_kick, gensym("kick"), (t_atomtype)0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_reset, gensym("reset"), A_DEFFLOAT, 0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_reset, gensym("buffer"), A_DEFFLOAT, 0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_blocksize, gensym("blocksize"), A_FLOAT, 0);
    class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_open, gensym("open"), A_SYMBOL, 0);
    class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_record, gensym("record"), A_FLOAT, 0);
	class_addmethod(bcreceive_tilde_class, (t_method)bcreceive_tilde_device, gensym("device"), A_FLOAT, 0);
	class_sethelpsymbol(bcreceive_tilde_class, gensym("bcast~"));

	post("___ bcreceive~ %s ___", VERSION);
	post("Copyright (C) 2016 Jussi Nieminen");
	post("Based on code copyright (C) 2008 Remu and 2004-2005 Olaf Matthes");
	post("___");
	
	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_overflow = gensym("overflow");
	ps_underflow = gensym("underflow");
	ps_queuesize = gensym("queuesize");
	ps_average = gensym("average");
	ps_hostname = gensym("ipaddr");
	ps_parameter = gensym("parameter");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_aac = gensym("_aac_");
	ps_sf_unknown = gensym("_unknown_");
	ps_nothing = gensym("");
}

#else

void bcreceive_tilde_assist(t_bcreceive_tilde *x, void *b, long m, long a, char *s)
{
	switch (m)
	{
		case 1: /* inlet */
			sprintf(s, "(Anything) Control Messages");
			break;
		case 2: /* outlets */
			sprintf(s, "(Signal) Audio Channel %d", (int)(a + 1));
			break;
		break;
	}

}

void main()
{
#ifdef _WINDOWS
    short version = MAKEWORD(2, 0);
    WSADATA nobby;
#endif	/* _WINDOWS */

	setup((t_messlist **)&bcreceive_tilde_class, (method)bcreceive_tilde_new, (method)bcreceive_tilde_free, 
		  (short)sizeof(t_bcreceive_tilde), 0L, A_DEFLONG, A_DEFLONG, A_DEFLONG, 0);
	addmess((method)bcreceive_tilde_dsp, "dsp", A_CANT, 0);
	addmess((method)bcreceive_tilde_assist, "assist", A_CANT, 0);
	addmess((method)bcreceive_tilde_print, "print", 0);
	addmess((method)bcreceive_tilde_kick, "kick", 0);
	addmess((method)bcreceive_tilde_reset, "reset", A_DEFFLOAT, 0);
	addmess((method)bcreceive_tilde_reset, "buffer", A_DEFFLOAT, 0);
	addmess((method)bcreceive_tilde_blocksize, "blocksize", A_LONG, 0);
	addbang((method)bcreceive_tilde_bang);
    addmess((method)bcreceive_tilde_open, "open", A_SYM, 0);
    addmess((method)bcreceive_tilde_record, "record", A_LONG, 0);
	dsp_initclass();
	finder_addclass("System", "bcreceive~");

	post("___ bcreceive~ %s ___", VERSION);
	post("Copyright (C) 2008 Remu | Written by Olivier Guillerminet");
	post("Based on code copyright (C) 2004-2005 Olaf Matthes");
	post("___");
	
	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_overflow = gensym("overflow");
	ps_underflow = gensym("underflow");
	ps_queuesize = gensym("queuesize");
	ps_average = gensym("average");
	ps_hostname = gensym("ipaddr");
	ps_parameter = gensym("parameter");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_aac = gensym("_aac_");
	ps_sf_unknown = gensym("_unknown_");
	ps_nothing = gensym("");

#ifdef _WINDOWS
    if (WSAStartup(version, &nobby)) error("bcreceive~: WSAstartup failed");
#endif	/* _WINDOWS */
}
#endif	/* PD */
