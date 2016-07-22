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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bcast~.h"
#include "float_cast.h"	/* tools for fast conversion from float to int */

#ifdef USE_FAAC
#include "faac/faac.h"
#endif

#include <sys/types.h>
#ifdef UNIX
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#define SOCKET_ERROR -1
#endif

#ifdef _WINDOWS
#include <winsock.h>
#include <pthread.h>
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )
#endif

#ifdef __APPLE__
#import <mach/mach_time.h>
#endif

#define TRUE 1
#define FALSE 0
typedef int BOOL;
typedef int SOCKET;

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS /*MSG_DONTWAIT|*/MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif


#define NB_CHAR_LINE 8


static int bcast_tilde_sockerror(char *s)
{
#ifdef _WINDOWS
    int err = WSAGetLastError();
    if (err == 10054) return 1;
    else if (err == 10053) post("bcast~: %s: software caused connection abort (%d)", s, err);
    else if (err == 10055) post("bcast~: %s: no buffer space available (%d)", s, err);
    else if (err == 10060) post("bcast~: %s: connection timed out (%d)", s, err);
    else if (err == 10061) post("bcast~: %s: connection refused (%d)", s, err);
    else post("bcast~: %s: %s (%d)", s, strerror(err), err);
#else
    int err = errno;
    post("bcast~: %s: %s (%d)", s, strerror(err), err);
#endif
#ifdef _WINDOWS
	if (err == WSAEWOULDBLOCK)
#endif
#ifdef UNIX
	if (err == EAGAIN)
#endif
	{
		return 1;	/* recoverable error */
	}
	return 0;	/* indicate non-recoverable error */
}



static void bcast_tilde_closesocket(int fd)
{
#ifdef UNIX
	close(fd);
#endif
#ifdef NT
	closesocket(fd);
#endif
}


/* ------------------------ bcast~ ----------------------------- */


#ifdef PD
static t_class *bcast_tilde_class;
#else
static void *bcast_tilde_class;
#endif

static t_symbol *ps_nothing, *ps_localhost, *ps_parameter;
static t_symbol *ps_format, *ps_channels, *ps_framesize, *ps_overflow, *ps_underflow;
static t_symbol *ps_queuesize, *ps_average, *ps_sf_float, *ps_sf_16bit, *ps_sf_8bit;
static t_symbol *ps_sf_mp3, *ps_sf_aac, *ps_sf_unknown, *ps_bitrate, *ps_hostname;

typedef struct _bcast_tilde
{
#ifdef PD
	t_object x_obj;
	t_outlet *x_outlet;
	t_outlet *x_outlet2;
	t_clock *x_clock;
#else
	t_pxobject x_obj;
	void *x_outlet;
	void *x_outlet2;
	void *x_clock;
#endif
	int x_fd;
	int x_protocol;
	t_tag x_tag;
	t_symbol* x_hostname;
	int x_portno;
	int x_connectstate;
	char* x_cbuf;
	int x_cbufsize;
	int x_blocksize;
	int x_blockspersend;
	int x_blockssincesend;

	long x_samplerate;          /* samplerate we're running at */
	int x_vecsize;              /* current DSP signal vector size */
	int x_ninlets;              /* number of inlets */
	int x_channels;             /* number of channels we want to stream */
	int x_format;               /* format of streamed audio data */
	int x_bitrate;              /* specifies bitrate for compressed formats */
	int x_count;                /* total number of audio frames */
	t_int** x_myvec;            /* vector we pass on in the DSP routine */

#ifdef USE_FAAC
    unsigned char *x_faacbuf;	/* data returned by encoder */
	faacEncHandle x_faac;
#endif

    pthread_mutex_t     x_mutex;
    pthread_cond_t      x_requestcondition;
    pthread_cond_t      x_answercondition;
    pthread_t           x_childthread;
	BOOL                x_childthreadon;
    
    BOOL                x_record;
    FILE*			    x_file;
    char				x_data[NB_CHAR_LINE];
	
#ifdef _WINDOWS
    LARGE_INTEGER		x_liStartTime;
#elif __APPLE__
	uint64_t			x_t0;
#endif

} t_bcast_tilde;

int send_w(t_bcast_tilde *x, char* tag, SOCKET s, char* buf, int len, int flags)
{	
	int             i = 0;
	int             j = 0;
	int             nbChar = 0;
	int             deltaChar = 0;
	
#ifdef _WINDOWS
	LARGE_INTEGER   liFrequency;
	LARGE_INTEGER   liCurrentTime;
#elif __APPLE__
	uint64_t		t1;
	uint64_t		delta;
	double			milliSeconds;
#endif

	if (x->x_record && x->x_file)
	{
	
#ifdef _WINDOWS
		QueryPerformanceFrequency ( &liFrequency );

		if (liFrequency.QuadPart == 0)
			fprintf(x->x_file, "bcast~ : Your computer does not support High Resolution Performance counter\n");

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
			x->x_data[j - 1] = buf[i - 1];
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

	return send(s, buf, len, flags);
}


static void bcast_tilde_notify(t_bcast_tilde *x)
{
	pthread_mutex_lock(&x->x_mutex);
    x->x_childthreadon = FALSE;
	outlet_float(x->x_outlet, x->x_connectstate);
	pthread_mutex_unlock(&x->x_mutex);
}


static void bcast_tilde_disconnect(t_bcast_tilde *x)
{
	pthread_mutex_lock(&x->x_mutex);
	if (x->x_fd != -1)
	{
		bcast_tilde_closesocket(x->x_fd);
		x->x_fd = -1;
		x->x_connectstate = 0;
		outlet_float(x->x_outlet, 0);
	}
	pthread_mutex_unlock(&x->x_mutex);
}


static void *bcast_tilde_doconnect(void *zz)
{
	t_bcast_tilde *x = (t_bcast_tilde *)zz;
    struct sockaddr_in server;
    struct hostent *hp;
	int intarg = 1;
    int sockfd;
    int portno;
	t_symbol *hostname;
	
	pthread_mutex_lock(&x->x_mutex);
    hostname = x->x_hostname;
	portno = x->x_portno;
	pthread_mutex_unlock(&x->x_mutex);

    /* create a socket */
    sockfd = socket(AF_INET, x->x_protocol, 0);
    if (sockfd < 0)
	{
		post("bcast~: connection to %s on port %d failed", hostname->s_name, portno);
		bcast_tilde_sockerror("socket");
        x->x_childthreadon = FALSE;
		return (0);
    }

    /* connect socket using hostname provided in command line */
    server.sin_family = AF_INET;
    hp = gethostbyname(x->x_hostname->s_name);
    if (hp == 0)
    {
        post("bcast~: bad host?");
        x->x_childthreadon = FALSE;
        return (0);
    }

    /* for stream (TCP) sockets, specify "nodelay" */
    if (x->x_protocol == SOCK_STREAM)
    {
		intarg = 1;
		if (setsockopt(sockfd, SOL_IP, TCP_NODELAY, (const char *)&intarg, sizeof(intarg)) < 0)
    			error("bcast~: setsockopt(TCP_NODELAY) failed");
	}

#ifdef SO_PRIORITY
    /* set high priority, LINUX only */
	intarg = 6;	/* select a priority between 0 and 7 */
    if (setsockopt(sockfd, SOL_SOCKET, SO_PRIORITY, (const char*)&intarg, sizeof(int)) < 0)
    {
		error("bcast~: setsockopt(SO_PRIORITY) failed");
    }
#endif

	/* for datagram (UDP) sockets, enable sending to the broadcast address. */
    if (x->x_protocol == SOCK_DGRAM)
    {
		intarg = 1;    
		if ( setsockopt( sockfd, SOL_SOCKET, SO_BROADCAST, (const char*)&intarg, sizeof(int) ) < 0 )
			post("bcast~: setsockopt(BROADCAST) failed");
	}

    memcpy((char *)&server.sin_addr, (char *)hp->h_addr, hp->h_length);

    /* assign client port number */
    server.sin_port = htons((unsigned short)portno);

    /* try to connect */
    if (connect(sockfd, (struct sockaddr *) &server, sizeof (server)) < 0)
    {
        bcast_tilde_sockerror("connecting stream socket");
        bcast_tilde_closesocket(sockfd);
        x->x_childthreadon = FALSE;
        return (0);
    }

    post("bcast~: connected host %s on port %d", hostname->s_name, portno);

	pthread_mutex_lock(&x->x_mutex);
    x->x_fd = sockfd;
	x->x_connectstate = 1;
	clock_delay(x->x_clock, 0);
	pthread_mutex_unlock(&x->x_mutex);
	return (0);
}



#ifdef PD
static void bcast_tilde_connect(t_bcast_tilde *x, t_symbol *host, t_floatarg fportno)
#else
static void bcast_tilde_connect(t_bcast_tilde *x, t_symbol *host, long fportno)
#endif
{
	pthread_mutex_lock(&x->x_mutex);
    if (x->x_childthreadon != FALSE)
    {
		 pthread_mutex_unlock(&x->x_mutex);
         post("bcast~: already trying to connect");
         return;
    }
    if (x->x_fd != -1)
    {
		 pthread_mutex_unlock(&x->x_mutex);
         post("bcast~: already connected");
         return;
    }

	if (host != ps_nothing)
		x->x_hostname = host;
	else
		x->x_hostname = ps_localhost;

    if (!fportno)
		x->x_portno = DEFAULT_PORT;
    else
		x->x_portno = (int)fportno;
    x->x_count = 0;

	/* start child thread to connect */
    if (pthread_create(&x->x_childthread, 0, bcast_tilde_doconnect, x) != 0)
    {
        x->x_childthreadon = TRUE;
    }
	pthread_mutex_unlock(&x->x_mutex);
}


static void bcast_tilde_parameters(t_bcast_tilde *x, t_symbol *s, int argc, t_atom *argv)
{
	int n, k;

	/* Check if the amount of parameters exceeds the specification */
	if ( argc > MAX_PARAMETERS )
	{
		post("bcast~: Too many parameters (%d). Maximum is %d.", argc,
				MAX_PARAMETERS);
		return;
	}
	
	k = 0;

	for ( n = 0; n < argc; n += 4 )
	{
		/* type check */
		if ( argv[n].a_type == A_FLOAT )
		{
			x->x_tag.parameters[k].ir_length = ( uint32_t ) atom_getfloat( &argv[n] );
			x->x_tag.parameters[k].ir_changed = ( uint32_t ) atom_getfloat( &argv[n+1] );
			x->x_tag.parameters[k].device = ( char ) atom_getfloat( &argv[n+2] );
			//x->x_tag.parameters[k].channel = ( char ) atom_getfloat( &argv[n+3] );
			x->x_tag.parameters[k].bypass = ( char ) atom_getfloat( &argv[n+3] );
			
			post( "bcast~: device #%d: ir length: %d ir changed: %d bypass: %d",
				x->x_tag.parameters[k].device,
				x->x_tag.parameters[k].ir_length,
				x->x_tag.parameters[k].ir_changed,
				//x->x_tag.parameters[k].channel,
				x->x_tag.parameters[k].bypass );
		}
		else
			post( "bcast~: Parameter #%d in wrong format. Expected A_FLOAT.", n );

		k++;
	}
	
}


#ifdef USE_FAAC
static void bcast_tilde_faac_deinit(t_bcast_tilde *x)
{
	if (x->x_faac)
		faacEncClose(x->x_faac);
	x->x_faac = NULL;
}


static int bcast_tilde_faac_init(t_bcast_tilde *x)
{
	faacEncConfigurationPtr faac_format;
	char *faac_id_string;
	char *faac_copyright_string;
	unsigned long samplesInput, maxBytesOutput;
    unsigned int mpegVersion = MPEG2;
	unsigned int channels = x->x_channels;

    if (!faacEncGetVersion(&faac_id_string, &faac_copyright_string) == FAAC_CFG_VERSION)
		return -1;

    /* open the encoder library */
    x->x_faac = faacEncOpen(x->x_samplerate, channels, &samplesInput, &maxBytesOutput);

    /* put the options in the configuration struct */
    faac_format = faacEncGetCurrentConfiguration(x->x_faac);
    faac_format->aacObjectType = MAIN; // LOW; /* MAIN, LOW or LTP */
    faac_format->mpegVersion = MPEG2;
    faac_format->useTns = 1;
    if (channels >= 6)
	{
        faac_format->useLfe = 1;
		faac_format->allowMidside = 0;
	}
    else
	{
        faac_format->useLfe = 0;
		faac_format->allowMidside = 1;
	}
	if (x->x_bitrate)
		faac_format->bitRate = (x->x_bitrate * 1000) / channels;
    faac_format->bandWidth = 0;
    faac_format->outputFormat = 1;			/* 0 = Raw; 1 = ADTS */
    faac_format->inputFormat = FAAC_INPUT_FLOAT;	/* input is float but scaled by 32768 */
    if (!faacEncSetConfiguration(x->x_faac, faac_format))
	{
        error("bcast~: faac: unsupported format settings");
		faacEncClose(x->x_faac);
		x->x_faac = NULL;
        return -1;
    }
	return 0;
}


static int bcast_tilde_faac_encode(t_bcast_tilde *x)
{
	static const int encbufsize = 1.25 * x->x_audiobuffersize + 7200;
    unsigned int samples = x->x_blocksize * x->x_tag.channels;
	float *bp = (float *)x->x_cbuf;

    	/* encode AAC */
	int ret = faacEncEncode(x->x_faac, (int32_t *)bp, samples, x->x_faacbuf, encbufsize);

		/* check result */
    if (ret < 0)
    {
		error("bcast~: faac: encoding failed (%d)", ret);
        faacEncClose(x->x_faac);
        x->x_faac = NULL;
		return -1;
    }

	return ret;	/* return total number of AAC samples (bytes) we got */
}
#endif

static t_int *bcast_tilde_perform(t_int *w)
{
    t_bcast_tilde* x = (t_bcast_tilde*) (w[1]);
    int n = (int)(w[2]);
	t_float *in[DEFAULT_AUDIO_CHANNELS];
	const int offset = 3;
    char* bp = NULL;
    int i, length = x->x_blocksize * SF_SIZEOF(x->x_tag.format) * x->x_tag.channels;
    int sent = 0;

	pthread_mutex_lock(&x->x_mutex);

	for (i = 0; i < x->x_ninlets; i++)
		in[i] = (t_float *)(w[offset + i]);

	if (n != x->x_vecsize)	/* resize buffer */
	{
		x->x_vecsize = n;
		x->x_blockspersend = x->x_blocksize / x->x_vecsize;
		x->x_blockssincesend = 0;
		length = x->x_blocksize * SF_SIZEOF(x->x_tag.format) * x->x_tag.channels;
	}

    /* format the buffer */
    switch (x->x_tag.format)
	{
		case SF_FLOAT:
		{
			t_float* fbuf = (t_float *)x->x_cbuf + (x->x_blockssincesend * x->x_vecsize * x->x_tag.channels);
			while (n--) 
				for (i = 0; i < x->x_tag.channels; i++)
					*fbuf++ = *(in[i]++);
			break;
		}
		case SF_16BIT:
		{
			short* cibuf = (short *)x->x_cbuf + (x->x_blockssincesend * x->x_vecsize * x->x_tag.channels);
			while (n--) 
				for (i = 0; i < x->x_tag.channels; i++)
					*cibuf++ = (short)lrint(32767.0 * *(in[i]++));
			break;
		}
		case SF_8BIT:
		{
			unsigned char*  cbuf = (unsigned char*)x->x_cbuf + (x->x_blockssincesend * x->x_vecsize * x->x_tag.channels);
			while (n--) 
				for (i = 0; i < x->x_tag.channels; i++)
					*cbuf++ = (unsigned char)(128. * (1.0 + *(in[i]++)));
			break;
		}
#ifdef USE_FAAC
		case SF_AAC:
		{
			/* same as SF_FLOAT but * 32767 and no byteswapping required */
			t_float* fbuf = (t_float *)x->x_cbuf + (x->x_blockssincesend * x->x_vecsize * x->x_tag.channels);
			while (n--) 
				for (i = 0; i < x->x_tag.channels; i++)
					*fbuf++ = 32767.0 * *(in[i]++);
			break;
		}
#endif
		default:
			 break;
    }

	if (!(x->x_blockssincesend < x->x_blockspersend - 1))	/* time to send the buffer */
	{
		x->x_blockssincesend = 0;
		x->x_count++;	/* count data packet we're going to send */

		if (x->x_fd != -1)
		{
#ifdef USE_FAAC
			if (x->x_tag.format == SF_AAC)
			{
				/* encode PCM to AAC and return new framesize */
				length = bcast_tilde_faac_encode(x);
				if (length <= 0)
				{
					if (length == 0) error("bcast~: skipping empty aac data frame");
					else error("bcast~: error encoding AAC");
					x->x_count--;
					goto bail;
				}
				bp = (char *)x->x_faacbuf;
			}
			else
#endif
				bp = (char *)x->x_cbuf;

			/* fill in the header tag */
			x->x_tag.framesize = length;
			x->x_tag.count = x->x_count;

			 /* send the format tag */

			if (send_w(x, "header", x->x_fd, (char*)&x->x_tag, sizeof(t_tag), SEND_FLAGS) < 0)
			{
				bcast_tilde_sockerror("send tag 1");
				pthread_mutex_unlock(&x->x_mutex);
				bcast_tilde_disconnect(x);
				return (w + offset + x->x_ninlets);
			}
         
			if (x->x_protocol == SOCK_STREAM) /* TCP: send all the data at once */
			{
				/* send the buffer */
				for (sent = 0; sent < length;)
				{
					int ret = 0;
					ret = send_w(x, "data", x->x_fd, bp, length - sent, SEND_FLAGS);

					if (ret <= 0)
					{
						bcast_tilde_sockerror("send data");
						pthread_mutex_unlock(&x->x_mutex);
						bcast_tilde_disconnect(x);
						return (w + offset + x->x_ninlets);
					}
					else
					{
						sent += ret;
						bp += ret;
					}
				}
			}
			else	/* UDP: max. packet size is 64k (incl. headers) so we have to split */
			{
#ifdef __APPLE__
				/* WARING: due to a 'bug' (maybe Apple would call it a feature?) in OS X
				   send calls with data packets larger than 1.5k fail with error number 40!
				   Thus we have to split the data packets into several packets that are 
				   1.5k in size. The other side will reassemble them again. */

				int size = DEFAULT_UDP_PACKT_SIZE;
				if (length < size)	/* maybe data fits into one packet? */
					size = length;

				/* send the buffer */
				for (sent = 0; sent < length;)
				{
					int ret = 0;
					ret = send_w(x, "data", x->x_fd, bp, size, SEND_FLAGS);
					if (ret <= 0)
					{
						bcast_tilde_sockerror("send data");
						pthread_mutex_unlock(&x->x_mutex);
						bcast_tilde_disconnect(x);
						return (w + offset + x->x_ninlets);
					}
					else
					{
						bp += ret;
						sent += ret;
						if ((length - sent) < size)
							size = length - sent;
					}
				}
#else
				/* send the buffer, the OS might segment it into smaller packets */
				int ret = send_w(x, "data", x->x_fd, bp, length, SEND_FLAGS);

				if (ret <= 0)
				{
					bcast_tilde_sockerror("send data");
					pthread_mutex_unlock(&x->x_mutex);
					bcast_tilde_disconnect(x);
					return (w + offset + x->x_ninlets);
				}
#endif
			}
		}

#ifdef USE_FAAC
bail:
#endif
		/* check whether user has updated any parameters */
		if (x->x_tag.channels != x->x_channels)
		{
#ifdef USE_FAAC
			/* reinit FAAC if format is SF_AAC */
			if (x->x_tag.format == SF_AAC)
			{
				bcast_tilde_faac_deinit(x);
				bcast_tilde_faac_init(x);
			}
#endif
			x->x_tag.channels = x->x_channels;
		}
		if (x->x_tag.format != x->x_format)
		{
#ifdef USE_FAAC
			/* deinit FAAC if format was SF_AAC */
			if ((x->x_format != SF_AAC) && (x->x_tag.format == SF_AAC))
			{
				bcast_tilde_faac_deinit(x);
			}
#endif
			x->x_tag.format = x->x_format;
		}
	}
	else
	{
		x->x_blockssincesend++;
	}
	pthread_mutex_unlock(&x->x_mutex);
    return (w + offset + x->x_ninlets);
}



static void bcast_tilde_dsp(t_bcast_tilde *x, t_signal **sp)
{
	int i;
	// DEBUG
	//post("bcast~: irlength = %u", x->x_tag.irlength);

	pthread_mutex_lock(&x->x_mutex);

	x->x_myvec[0] = (t_int*)x;
	x->x_myvec[1] = (t_int*)sp[0]->s_n;

	x->x_samplerate = sp[0]->s_sr;

	for (i = 0; i < x->x_ninlets; i++)
	{
		x->x_myvec[2 + i] = (t_int*)sp[i]->s_vec;
	}

	pthread_mutex_unlock(&x->x_mutex);

	if (x->x_blocksize % sp[0]->s_n)
	{
		error("bcast~: signal vector size too large (needs to be even divisor of %d)", x->x_blocksize);
	}
	else
	{
#ifdef PD
		dsp_addv(bcast_tilde_perform, x->x_ninlets + 2, (t_int*)x->x_myvec);
#else
		dsp_addv(bcast_tilde_perform, x->x_ninlets + 2, (void**)x->x_myvec);
#endif
	}
}


#ifdef PD
static void bcast_tilde_channels(t_bcast_tilde *x, t_floatarg channels)
#else
static void bcast_tilde_channels(t_bcast_tilde *x, long channels)
#endif
{
	pthread_mutex_lock(&x->x_mutex);
	if (channels >= 0 && channels <= DEFAULT_AUDIO_CHANNELS)
	{
		x->x_channels = (int)channels;
		post("bcast~: channels set to %d", (int)channels);
	}
	pthread_mutex_unlock(&x->x_mutex);
}

#ifdef PD
static void bcast_tilde_blocksize(t_bcast_tilde *x, t_floatarg blocksize)
#else
static void bcast_tilde_blocksize(t_bcast_tilde *x, long blocksize)
#endif
{
    if (x->x_connectstate)
    {
        post("bcast~ : error - you can't change blocksize when bcast~ is already connected");
    }
    else
    {
        x->x_blocksize = (int)blocksize;
	    x->x_blockspersend = x->x_blocksize / x->x_vecsize;
	    x->x_blockssincesend = 0;
	    x->x_cbufsize = x->x_blocksize * sizeof(t_float) * x->x_ninlets;
	    x->x_cbuf = (char *)t_getbytes(x->x_cbufsize);
	    post("bcast~ : blocksize set to %i", x->x_blocksize);
	}
}

static void bcast_tilde_open(t_bcast_tilde *x, t_symbol* path)
{
    x->x_file = fopen(path->s_name, "w+");
    
    if (x->x_file)          post("bcast~ : file %s opened", path->s_name);
    else                    post("bcast~ : a problem occured when trying to open the file %s", path->s_name);
}

#ifdef PD
static void bcast_tilde_record(t_bcast_tilde *x, t_floatarg record)
#else
static void bcast_tilde_record(t_bcast_tilde *x, long record)
#endif
{
    if (!x->x_file)
    {
        post("bcast~ : you need to open a file first");
        return;
    }
    
    //REC 1
    if (record)
    {
        post("bcast~ : recording...");
        
        //already recording
        if (x->x_record)    post("bcast~ : already recording");
        
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
        post("bcast~ : end of recording...");
        
        //already off
        if (!x->x_record)    post("bcast~ : recording is already set to 0");
        
        //ends record
        else
        {
            fclose(x->x_file);
            x->x_record = FALSE;
        }
    }
}


#ifdef PD
static void bcast_tilde_format(t_bcast_tilde *x, t_symbol* form, t_floatarg bitrate)
#else
static void bcast_tilde_format(t_bcast_tilde *x, t_symbol* form, long bitrate)
#endif
{
	pthread_mutex_lock(&x->x_mutex);
	if (!strncmp(form->s_name,"float", 5) && x->x_tag.format != SF_FLOAT)
	{
		x->x_format = (int)SF_FLOAT;
	}
	else if (!strncmp(form->s_name,"16bit", 5) && x->x_tag.format != SF_16BIT)
	{
		x->x_format = (int)SF_16BIT;
	}
	else if (!strncmp(form->s_name,"8bit", 4) && x->x_tag.format != SF_8BIT)
	{
		x->x_format = (int)SF_8BIT;
	}
	else if (!strncmp(form->s_name,"mp3", 3) && x->x_tag.format != SF_MP3)
	{
		error("bcast~: not compiled with mp3 support");
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	else if (!strncmp(form->s_name,"aac", 3) && x->x_tag.format != SF_AAC)
	{
#ifdef USE_FAAC
		x->x_bitrate = (int)bitrate;
		if (bcast_tilde_faac_init(x) < 0)
		{
			pthread_mutex_unlock(&x->x_mutex);
			return;
		}
		x->x_format = (int)SF_AAC;
#else
		error("bcast~: not compiled with aac support");
		pthread_mutex_unlock(&x->x_mutex);
		return;
#endif
	}

	post("bcast~: format set to %s", form->s_name);
	pthread_mutex_unlock(&x->x_mutex);
}


/* set hostname to send to */
static void bcast_tilde_host(t_bcast_tilde *x, t_symbol* host)
{
	pthread_mutex_lock(&x->x_mutex);
	if (host != ps_nothing)
		x->x_hostname = host;
	else
		x->x_hostname = ps_localhost;

	if (x->x_fd != -1)
	{
		pthread_mutex_unlock(&x->x_mutex);
		bcast_tilde_connect(x,x->x_hostname, (float)x->x_portno);
		return;
	}
	pthread_mutex_unlock(&x->x_mutex);
}



#ifdef PD
static void bcast_tilde_float(t_bcast_tilde* x, t_floatarg arg)
#else
static void bcast_tilde_float(t_bcast_tilde* x, double arg)
#endif
{
	if (arg == 0.0)
		bcast_tilde_disconnect(x);
	else
		bcast_tilde_connect(x,x->x_hostname,(float) x->x_portno);
}


/* send stream info when banged */
static void bcast_tilde_bang(t_bcast_tilde *x)
{
	t_atom list[2];
	t_symbol *sf_format;
	t_float bitrate;
	int i;
	bitrate = (t_float)((SF_SIZEOF(x->x_tag.format) * x->x_samplerate * 8 * x->x_tag.channels) / 1000.);

	switch (x->x_tag.format)
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
	SETFLOAT(list, (t_float)x->x_tag.channels);
	outlet_anything(x->x_outlet2, ps_channels, 1, list);

	/* framesize */
	SETFLOAT(list, (t_float)x->x_tag.framesize);
	outlet_anything(x->x_outlet2, ps_framesize, 1, list);

	/* bitrate */
	SETFLOAT(list, (t_float)bitrate);
	outlet_anything(x->x_outlet2, ps_bitrate, 1, list);

	/* IP address */
	SETSYMBOL(list, (t_symbol *)x->x_hostname);
	outlet_anything(x->x_outlet2, ps_hostname, 1, list);

	/* parameters */
	for ( i = 0; i < DEFAULT_AUDIO_CHANNELS; i++)
	{
		SETFLOAT( list, (t_float)x->x_tag.parameters[i].ir_length );
		outlet_anything(x->x_outlet2, ps_parameter, 1, list);
		SETFLOAT( list, (t_float)x->x_tag.parameters[i].ir_changed );
		outlet_anything(x->x_outlet2, ps_parameter, 1, list);
		SETFLOAT( list, (t_float)x->x_tag.parameters[i].device );
		outlet_anything(x->x_outlet2, ps_parameter, 1, list);
		//SETFLOAT( list, (t_float)x->x_tag.parameters[i].channel );
		//outlet_anything(x->x_outlet2, ps_parameter, 1, list);
		SETFLOAT( list, (t_float)x->x_tag.parameters[i].bypass );
		outlet_anything(x->x_outlet2, ps_parameter, 1, list);
	}

#else
	/* --- stream information (t_tag) --- */
	/* audio format */
	SETSYM(list, ps_format);
	SETSYM(list + 1, (t_symbol *)sf_format);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* channels */
	SETSYM(list, ps_channels);
	SETLONG(list + 1, (int)x->x_tag.channels);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* framesize */
	SETSYM(list, ps_framesize);
	SETLONG(list + 1, (int)x->x_tag.framesize);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* ircount */
	SETSYM(list, ps_ircount);
	SETLONG(list + 1, (int)x->x_tag.ircount);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* bitrate */
	SETSYM(list, ps_bitrate);
	SETFLOAT(list + 1, (t_float)bitrate);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* IP address */
	SETSYM(list, (t_symbol *)ps_hostname);
	SETSYM(list + 1, x->x_hostname);
	outlet_list(x->x_outlet2, NULL, 2, list);

	/* parameters */
	for ( i = 0; i < DEFAULT_AUDIO_CHANNELS; i++)
	{
		SETSYM(list, ps_parameter);
		SETLONG(list + 1, (int)x->x_tag.parameters[i].ir_length);
		outlet_list(x->x_outlet2, NULL, 2, list);
		SETSYM(list, ps_parameter);
		SETLONG(list + 1, (int)x->x_tag.parameters[i].ir_changed);
		outlet_list(x->x_outlet2, NULL, 2, list);
		SETSYM(list, ps_parameter);
		SETLONG(list + 1, (int)x->x_tag.parameters[i].device);
		outlet_list(x->x_outlet2, NULL, 2, list);
		//SETSYM(list, ps_parameter);
		//SETLONG(list + 1, (int)x->x_tag.parameters[i].channel);
		//outlet_list(x->x_outlet2, NULL, 2, list);
		SETSYM(list, ps_parameter);
		SETLONG(list + 1, (int)x->x_tag.parameters[i].bypass);
		outlet_list(x->x_outlet2, NULL, 2, list);
	}

#endif
}


#ifdef PD
static void *bcast_tilde_new(t_floatarg inlets, t_floatarg prot)
#else
static void *bcast_tilde_new(long inlets, long prot)
#endif
{
	int i;

#ifdef PD
	t_bcast_tilde *x = (t_bcast_tilde *)pd_new(bcast_tilde_class);
#else
	t_bcast_tilde *x = (t_bcast_tilde *)newobject(bcast_tilde_class);
#endif

	x->x_record = FALSE;
	x->x_file = NULL;

#ifdef PD
    if (x)
    { 
        for (i = sizeof(t_object); i < (int)sizeof(t_bcast_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	x->x_ninlets = CLIP((int)inlets, 1, DEFAULT_AUDIO_CHANNELS);
	
	/* Make double the channels to include the IRs */

	for (i = 1; i < x->x_ninlets; i++)
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);

	x->x_outlet = outlet_new(&x->x_obj, &s_float);
	x->x_outlet2 = outlet_new(&x->x_obj, &s_list);
	x->x_clock = clock_new(x, (t_method)bcast_tilde_notify);
#else
    if (x)
    { 
        for (i = sizeof(t_pxobject); i < sizeof(t_bcast_tilde); i++)  
                ((char *)x)[i] = 0; 
	}

	x->x_ninlets = CLIP((int)inlets, 1, DEFAULT_AUDIO_CHANNELS);
	dsp_setup((t_pxobject *)x, x->x_ninlets);
	x->x_outlet2 = outlet_new(x, "list");
	x->x_outlet = outlet_new(x, "int");
	x->x_clock = clock_new(x, (method)bcast_tilde_notify);
#endif

	x->x_myvec = (t_int **)t_getbytes(sizeof(t_int *) * (x->x_ninlets + 3));
	if (!x->x_myvec)
	{
		error("bcast~: out of memory");
		return NULL;
	}

#ifdef USE_FAAC
	/* allocate a buffer for encoded data */
	x->x_faacbuf = (unsigned char *)t_getbytes(sizeof(char *) * (1.25 * x->x_audiobuffersize + 7200));
	if (!x->x_faacbuf)
	{
		error("bcast~: out of memory");
		return NULL;
	}
	x->x_faac = NULL;	/* encoder not yet nitialized */
#endif

    pthread_mutex_init(&x->x_mutex, 0);
    pthread_cond_init(&x->x_requestcondition, 0);
    pthread_cond_init(&x->x_answercondition, 0);

	x->x_hostname = ps_localhost;
	x->x_portno = 3000;
	x->x_connectstate = 0;
    x->x_childthreadon = FALSE;
	x->x_fd = -1;
	x->x_protocol = SOCK_STREAM;

	if (prot)	/* user wants UDP */
	{
		x->x_protocol = SOCK_DGRAM;
	}

	x->x_tag.format = x->x_format = SF_FLOAT;
	x->x_tag.channels = x->x_channels = x->x_ninlets;
	x->x_tag.version = SF_BYTE_NATIVE;	/* native endianness */
	x->x_vecsize = 64;      /* we'll update this later */
	x->x_bitrate = 0;		/* not specified, use default */
	x->x_cbuf = NULL;
	x->x_blocksize = DEFAULT_AUDIO_BUFFER_SIZE;
	x->x_blockspersend = x->x_blocksize / x->x_vecsize;
	x->x_blockssincesend = 0;
	x->x_cbufsize = x->x_blocksize * sizeof(t_float) * x->x_ninlets;
	x->x_cbuf = (char *)t_getbytes(x->x_cbufsize);
		
#ifdef UNIX
	/* we don't want to get signaled in case send() fails */
	signal(SIGPIPE, SIG_IGN);
#endif

	return (x);
}



static void bcast_tilde_free(t_bcast_tilde* x)
{
	bcast_tilde_disconnect(x);

#ifndef PD
	//inlet_free(x->x_inlet_irlength); /* NEW */
	dsp_free((t_pxobject *)x);	/* free the object */
#endif

	/* free the memory */
	if (x->x_cbuf)t_freebytes(x->x_cbuf, x->x_cbufsize);
	if (x->x_myvec)t_freebytes(x->x_myvec, sizeof(t_int) * (x->x_ninlets + 3));

#ifdef USE_FAAC
	if (x->x_faacbuf)t_freebytes(x->x_faacbuf, sizeof(char *) * (1.25 * x->x_audiobuffersize + 7200));
	bcast_tilde_faac_deinit(x);
#endif

	clock_free((t_object *)x->x_clock);

    pthread_cond_destroy(&x->x_requestcondition);
    pthread_cond_destroy(&x->x_answercondition);
    pthread_mutex_destroy(&x->x_mutex);
}

#ifdef PD

void bcast_tilde_setup(void)
{
    bcast_tilde_class = class_new(gensym("bcast~"), (t_newmethod)bcast_tilde_new, (t_method)bcast_tilde_free,
        sizeof(t_bcast_tilde), 0, A_DEFFLOAT, A_DEFFLOAT, A_NULL);
    class_addmethod(bcast_tilde_class, nullfn, gensym("signal"), (t_atomtype)0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_dsp, gensym("dsp"), (t_atomtype)0);
    class_addfloat(bcast_tilde_class, bcast_tilde_float);
    class_addbang(bcast_tilde_class, bcast_tilde_bang);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_connect, gensym("connect"), A_DEFSYM, A_DEFFLOAT, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_disconnect, gensym("disconnect"), 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_channels, gensym("channels"), A_FLOAT, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_format, gensym("format"), A_SYMBOL, A_DEFFLOAT, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_host, gensym("host"), A_DEFSYM, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_blocksize, gensym("blocksize"), A_FLOAT, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_open, gensym("open"), A_SYMBOL, 0);
    class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_record, gensym("record"), A_FLOAT, 0);
	class_addmethod(bcast_tilde_class, (t_method)bcast_tilde_parameters, gensym("parameters"), A_GIMME, 0);
	class_sethelpsymbol(bcast_tilde_class, gensym("bcast~"));

	post("___ bcast~ %s ___", VERSION);
	post("Copyright (C) 2008 Remu | Written by Olivier Guillerminet");
	post("Based on code copyright (C) 2004-2005 Olaf Matthes");
	post("___");

	ps_nothing = gensym("");
	ps_localhost = gensym("localhost");
	ps_hostname = gensym("ipaddr");
	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_parameter = gensym("parameter");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_aac = gensym("_aac_");
	ps_sf_unknown = gensym("_unknown_");
}

#else

void bcast_tilde_assist(t_bcast_tilde *x, void *b, long m, long a, char *s)
{
	switch(m)
	{
		case 1: // inlet
			switch(a)
			{
				case 0:
					sprintf(s, "Control Messages & Audio Channel 1");
					break;
				default:
					sprintf(s, "Audio Channel %d", (int)a + 1);
					break;
			}
		break;
		case 2: // outlet
			switch(a)
			{
				case 0:
					sprintf(s, "(Int) State of Connection");
					break;
			}
		break;
	}

}

void main()
{
#ifdef _WINDOWS
    short version = MAKEWORD(2, 0);
    WSADATA nobby;
#endif /* _WINDOWS */

	setup((t_messlist **)&bcast_tilde_class, (method)bcast_tilde_new, (method)bcast_tilde_free, 
	      (short)sizeof(t_bcast_tilde), 0L, A_DEFLONG, A_DEFLONG, 0);

	addmess((method)bcast_tilde_dsp, "dsp", A_CANT, 0);
	addmess((method)bcast_tilde_connect, "connect", A_DEFSYM, A_DEFLONG, 0);
	addmess((method)bcast_tilde_disconnect, "disconnect", 0);
	addmess((method)bcast_tilde_format, "format", A_SYM, A_DEFLONG, 0);
	addmess((method)bcast_tilde_channels, "channels", A_LONG, 0);
	addmess((method)bcast_tilde_host, "host", A_DEFSYM, 0);
	addmess((method)bcast_tilde_assist, "assist", A_CANT, 0);
	addbang((method)bcast_tilde_bang);
	addmess((method)bcast_tilde_blocksize, "blocksize", A_LONG, 0);
    addmess((method)bcast_tilde_open, "open", A_SYM, 0);
    addmess((method)bcast_tilde_record, "record", A_LONG, 0);


	dsp_initclass();
	finder_addclass("System", "bcast~");

	post("___ bcast~ %s ___", VERSION);
	post("Copyright (C) 2008 Remu | Written by Olivier Guillerminet");
	post("Based on code copyright (C) 2004-2005 Olaf Matthes");
	post("___");

	ps_nothing = gensym("");
	ps_localhost = gensym("localhost");
	ps_hostname = gensym("ipaddr");
	ps_format = gensym("format");
	ps_channels = gensym("channels");
	ps_framesize = gensym("framesize");
	ps_bitrate = gensym("bitrate");
	ps_parameter = gensym("parameter");
	ps_sf_float = gensym("_float_");
	ps_sf_16bit = gensym("_16bit_");
	ps_sf_8bit = gensym("_8bit_");
	ps_sf_mp3 = gensym("_mp3_");
	ps_sf_aac = gensym("_aac_");
	ps_sf_unknown = gensym("_unknown_");

#ifdef _WINDOWS
    if (WSAStartup(version, &nobby)) error("bcast~: WSAstartup failed");
#endif /* _WINDOWS */
}

#endif	/* PD */
