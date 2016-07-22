/* ------------------------------- conv~ -------------------------------------- */
/*                                                                              */
/* Description of the external.                                                 */
/*                                                                              */
/* ~/externals/fftw   ~/externals/iemlib/iemlib1/src/FIR                        */
/*                                                                              */
/* Author: Jussi Nieminen                                                       */
/*                                                                              */
/* ---------------------------------------------------------------------------- */

#include "m_pd.h"
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "conv~.h"

#ifdef THREADS
	#include <pthread.h>
#endif



/* Constructor */
void *conv_tilde_new(t_floatarg channels)
{
	int i, n;
	t_conv_tilde *x = (t_conv_tilde *)pd_new(conv_tilde_class);
	x->f = 0;
    x->framesize = DEFAULTFRAMESIZE;    	/* update this later if needed */
	x->fft_size = 2 * x->framesize;
	x->out_gain = 1.0 / x->fft_size;		/* Needs the decimal point! */


	/* Clip the channel between 1 and default maximum channels */
	x->channels = CLIP((int)channels, 1, DEFAULT_AUDIO_CHANNELS);

	/* Threads are not worth with only one channel. */
	//if ( x->channels <= 1 )
	//	#undef THREADS

	x->all_channels = x->channels * 3;

	/* Assign an inlet pair (audio + IR) for each channel.
	One inlet is in place by default. */
	for (i = 1; i < x->channels * 2; i++)
		inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);

	/* Assign outlets for each output channel. */
    for (i = 0; i < x->channels; i++)
		outlet_new(&x->x_obj, &s_signal);



	/* Allocate memory for the DSP vector */
	x->x_myvec = (t_int **)t_getbytes(sizeof(t_int *) * (x->all_channels + 3));

	if (!x->x_myvec)
	{
		error("conv~: out of memory");
		return NULL;
	}

	/* Allocate memory for the parameters vector. */
	//x->parameters = (t_float*) malloc(sizeof(t_float) * NUMBER_OF_PARAMETERS);

	/* Allocate memory for channel variables. 1 variable per channel. */
	x->input_rw_ptr		= (int*) malloc(sizeof(int) * x->channels);
	x->ir_end_ptr		= (int*) malloc(sizeof(int) * x->channels);
	x->ir_frames		= (int*) malloc(sizeof(int) * x->channels);
	x->frames_stored	= (int*) malloc(sizeof(int) * x->channels);
	x->irlength			= (int*) malloc(sizeof(t_float) * x->channels);
	x->ircount			= (int*) malloc(sizeof(t_float) * x->channels);
	x->ircount_prev		= (int*) malloc(sizeof(int) * x->channels);
	x->bypass			= (int*) malloc(sizeof(int) * x->channels);

    /* Allocate multi-dimensional storage arrays and the overlap-save array. */
    x->stored_input		= (fftwf_complex**) malloc( sizeof(fftwf_complex*) * x->channels );
    x->stored_ir		= (fftwf_complex**) malloc( sizeof(fftwf_complex*) * x->channels );
    x->overlap_save		= (float**) malloc( sizeof(float*) * x->channels );

    for ( i = 0; i < x->channels; i++)
    {
        x->stored_input[i] 	= fftwf_alloc_complex(sizeof(fftwf_complex) * 2 * STORAGELENGTH);
        x->stored_ir[i]	    = fftwf_alloc_complex(sizeof(fftwf_complex) * 2 * STORAGELENGTH);
        x->overlap_save[i] = (float*) fftwf_malloc( sizeof(float) * x->framesize );
    }

	/* Init pointers and variables. */
	for ( i = 0; i < x->channels; i++ )
	{
		x->ir_end_ptr[i] = 0;
		x->input_rw_ptr[i] = 0;
		x->ircount[i] = 0;
		x->ircount_prev[i] = -1;
		x->irlength[i] = 0;
		x->ir_frames[i] = 0;
		x->frames_stored[i] = 0;
		x->bypass[i] = 0;
	}

	#ifdef THREADS
		/* round up the half way point. */
		x->channels_halved = ( x->channels + 1 ) / 2;
	#endif

	/* Initialize FFTW arrays and plans. */
	conv_tilde_init_fft_plans(x);

	return (void *)x;
}



t_int *conv_tilde_perform(t_int *w)
{
	/* The pointer to the object struct */
	t_conv_tilde *x = (t_conv_tilde*) (w[1]);

	unsigned int H_ptr;
	int chan, i, n /*, vector_size */;
	/* t_sample is a PD datatype for sample data. It is basically just float. */
	t_sample* __restrict in;
	t_sample* __restrict ir;
	t_sample* __restrict out;

    /* Audio starts from w[3] */
	const unsigned int offset = 3;

	/* We do not really need vector size for anything if we keep the frame size
	constant. Changing framesize on the fly causes undefined behaviour as no
	good logic has been implemented for updating the storage ring-buffering in
	the event of a framesize change. */

	/*
	vector_size = (int)(w[2]);
	if ( vector_size != DEFAULTFRAMESIZE )
		post("conv~: Problem! Framesize %d != %d", vector_size, DEFAULTFRAMESIZE);
	*/



	/* - PROCESS THE SIGNAL ------------------------------------------------- */

	#ifdef THREADS
	int rc = pthread_create(&x->childthread, NULL,
							conv_tilde_parallel_thread, (void*) x );
	if ( rc != 0 )
		post("conv~: Error in pthread_create(). Error code %d", rc);
	for ( chan = 0; chan < x->channels_halved; chan++ )

	#else
	for ( chan = 0; chan < x->channels; chan++ )
	#endif
	{

		/* Check whether the IR has changed for either channel */
		if (x->ircount[chan] != x->ircount_prev[chan])
		{
			post("conv~: IR%d changed", chan);

			/* Start filling the arrays from the beginning */
			x->ir_end_ptr[chan] = 0;
			x->input_rw_ptr[chan] = 0;

			/* Truncate the IR if it is longer than STORAGELENGTH. */
			if ( x->irlength[chan] > STORAGELENGTH )
				x->irlength[chan] = STORAGELENGTH;

			/* Calculate the number of frames to store */
			x->ir_frames[chan] = ceil((float)x->irlength[chan] / x->framesize);

			x->frames_stored[chan] = 0;
			x->ircount_prev[chan] = x->ircount[chan];
		}

		/* ----- BYPASS ----------------------------------------------------- */

		/* If no IR is loaded, bypass the convolution to save cpu */
		if ( x->irlength[chan] <= 0 || x->bypass[chan] )
		{
			in = (t_sample *)(w[offset + chan]);
			out = (t_sample *)(w[offset - chan + x->all_channels - 1]);

			memcpy( out,
					in,
					sizeof(t_sample) * x->framesize );

			/* Skip the processing */
			continue;
		}

		/* ------------------------------------------------------------------ */

		/* Assign the signal arrays to temporary pointers for simplicity. Note
		the clockwise arrangement of channels in Pd (in in2 ir ir2 out2 out). */

		in 	= (t_sample *)(w[offset + chan]);
		ir 	= (t_sample *)(w[offset + chan + x->channels]);
		out	= (t_sample *)(w[offset - chan + x->all_channels - 1]);


		/* This bit decides whether we append the frame to the storage or
		    overwrite the oldest one */
		if (x->frames_stored[chan] < x->ir_frames[chan])
		{
			/* This means that there are still IR buffers coming. */

			/* 1) Copy the input buffer to the REAL part of the input_complex array. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->input_complex, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->input_complex[x->framesize + i][0] = in[i];
					x->input_complex[x->framesize + i][1] = 0;
				}
			#else
				memcpy(x->input_to_fft + x->framesize,
					in,
					sizeof(float) * x->framesize);
			#endif

			/* 2) Transform and store the input buffer. */

			fftwf_execute(x->fftplan_in);

			memcpy(x->stored_input[chan] + x->ir_end_ptr[chan],
				x->input_complex,
				sizeof(fftwf_complex) * x->fft_size);

			/* 3) Do the same to the IR buffer. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->ir_complex + x->framesize, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->ir_complex[i][0] = ir[i];
					x->ir_complex[i][1] = 0;
				}
			#else
				memcpy(x->ir_to_fft,
					ir,
					sizeof(float) * x->framesize);
			#endif

		    fftwf_execute(x->fftplan_ir);

			memcpy(x->stored_ir[chan] + x->ir_end_ptr[chan],
				x->ir_complex,
				sizeof(fftwf_complex) * x->fft_size);

			/* 4) Increment storage pointers. */
			x->ir_end_ptr[chan] += x->fft_size;
			x->frames_stored[chan]++;

		    /* 5) Set the input read/write pointer forwards */
		    x->input_rw_ptr[chan] += x->fft_size;
		    if (x->input_rw_ptr[chan] >= x->ir_end_ptr[chan])
		        x->input_rw_ptr[chan] -= x->ir_end_ptr[chan];
		}
		else
		{
			/* IR is fully loaded. Overwrite stored audio frames (FIFO). */

			/* 1) Set the input read/write pointer forwards. */
		    x->input_rw_ptr[chan] += x->fft_size;
		    if (x->input_rw_ptr[chan] >= x->ir_end_ptr[chan])
		        x->input_rw_ptr[chan] -= x->ir_end_ptr[chan];

			/* 2) Copy the input buffer to the fft_input_array. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->input_complex, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->input_complex[x->framesize + i][0] = in[i];
					x->input_complex[x->framesize + i][1] = 0;
				}
			#else
				memcpy(x->input_to_fft + x->framesize,
					in,
					sizeof(float) * x->framesize);
			#endif

			/* 3) Transform and store the input buffer. */

			fftwf_execute(x->fftplan_in);

			memcpy(x->stored_input[chan] + x->input_rw_ptr[chan],
				x->input_complex,
				sizeof(fftwf_complex) * x->fft_size);
		}



		/* - Convolve block by block -----------------------------------------*/

		H_ptr = 0;

		/* If the process is here then there is bound to be at least one frame stored.
		The first frame in the sum overwrites the output array. The iteration goes down
		as ARM CPU compares against zero the fastest. */
		for (n = x->fft_size - 1; n >= 0; n--)
		{
		    /* Complex multiplication (frequency domain convolution):

			Re(Y) = Re(X)Re(H) - Im(X)Im(H)
			Im(Y) = Im(X)Re(H) + Re(X)Im(H)

			Nice 4 divisible iteration length for compiler SIMD optimization.
			Note that the first iteration has to overwrite the output array.

                                        | chan | sample                     | Re/Im | */
		    x->out_complex[n][0]
					=	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [0]
					*	x->stored_ir    [chan] [H_ptr + n]                  [0]
					-	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [1]
					*	x->stored_ir    [chan] [H_ptr + n]                  [1];

		    x->out_complex[n][1]
					=	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [1]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [0]
					+	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [0]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [1];
		 }

		/* Move the IR pointer forwards */
		H_ptr += x->fft_size;

		/* Move the input read/write pointer backwards. */
		x->input_rw_ptr[chan] -= x->fft_size;
		if (x->input_rw_ptr[chan] < 0 )
		    x->input_rw_ptr[chan] += x->ir_end_ptr[chan];

		/* If many frames in storage, repeat and sum the results. */
		for (i = x->frames_stored[chan] - 1; i > 0; i--)
		{
		    for (n = x->fft_size - 1; n >= 0; n--)
		    {
		    	x->out_complex[n][0]
					+=	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [0]
					*	x->stored_ir    [chan] [H_ptr + n]                  [0]
					-	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [1]
					*	x->stored_ir    [chan] [H_ptr + n]                  [1];

		    	x->out_complex[n][1]
					+=	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [1]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [0]
					+	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [0]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [1];
		     }

		    /* Move the IR pointer forwards */
		    H_ptr += x->fft_size;

		    /* Move the input read/write pointer backwards. */
		    x->input_rw_ptr[chan] -= x->fft_size;
		    if (x->input_rw_ptr[chan] < 0 )
		        x->input_rw_ptr[chan] += x->ir_end_ptr[chan];
		}


		/* Insert the previous overlap save portion before overwriting the OS. */
		memcpy(out,
				x->overlap_save[chan],
				sizeof(float) * x->framesize);

		/* Inverse fft. Result is stored in x->outTemp. */
		fftwf_execute(x->fftplan_inverse);

		/* Store the overlap save portion. */
        memcpy(x->overlap_save[chan],
				x->outTemp,
				sizeof(float) * x->framesize);

		/* Sum the output with the previous overlap and scale the amplitude. */

		for (i = x->framesize - 1; i >= 0; i--)
		{
			out[i] += x->outTemp[i + x->framesize];
			out[i] *= x->out_gain;
		}


	}
	/* - END ---------------------------------------------------------------- */

	#ifdef THREADS
		rc = pthread_join(x->childthread, NULL);

		if ( rc != 0 )
			post("conv~: pthread_join() error code %d", rc);
	#endif

	/* Return a pointer after the last output channel */
	return (w + offset + x->all_channels);
}




/* The “dsp”-method has two arguments, the pointer to the class-data space, and
a pointer to an array of signals. */
void conv_tilde_dsp(t_conv_tilde *x, t_signal **sp)
{
    int i;

	/* PD's system for passing the signal to the dsp method as int pointers is	*/
	/* a bit unclear. Passing the frame length as an t_int pointer especially 	*/
	/* seems like chewing gum solution but hey, it works. 						*/

	/* Add struct pointer. */
	x->x_myvec[0] = (t_int*)x;

	/* Add the audio buffer length second. */
	x->x_myvec[1] = (t_int*)(sp[0]->s_n);

	/* Add each input and output channel */
	for (i = 0; i < x->all_channels; i++)
	{
		x->x_myvec[2 + i] = (t_int*)sp[i]->s_vec;
	}

	/* Add the pointers to the DSP tree as a vector. */
    dsp_addv(conv_tilde_perform, x->all_channels + 2, (t_int*)x->x_myvec);
}


void conv_tilde_bang(t_conv_tilde *x)
{
	int i;
	for ( i = 0; i < x->channels; i++)
	{
		post("CHANNEL %d", i);
		post("Ir length: %d", x->irlength[i]);
		post("Ir length in frames: %d", x->ir_frames[i]);
		post("Ir count: %d", x->ircount[i]);
		post("Previous ir count: %d", x->ircount_prev[i]);
		post("Bypass: %d", x->bypass[i]);
		post("Frames stored: %d", x->frames_stored[i]);
		post("ir_end_ptr: %d",x->ir_end_ptr[i]);
		post("input_rw_ptr: %d", x->input_rw_ptr[i]);
	}
}


void conv_tilde_parameters(t_conv_tilde *x, t_symbol *s, int argc, t_atom *argv)
{
	int chan, n;

	n = 0;

	for ( chan = 0; chan < x->channels; chan++ )
	{
		/* Type checking would be nice but we will strip it out to save cycles*/
		/*if ( argv[n].a_type != A_FLOAT)
			post("conv~: Parameter %d in wrong format. Expected A_FLOAT.", n);
		else*/
		
		/* Assign the new parameters to respective variables. */
		x->irlength[chan] = (int)atom_getfloat(&argv[n]);
		if ( ++n > argc )
			break;
		x->ircount[chan] = (int)atom_getfloat(&argv[n]);
		if ( ++n > argc )
			break;
		x->bypass[chan] = (int)atom_getfloat(&argv[n]);
		if ( ++n > argc )
			break;
	}
}


void conv_tilde_free(t_conv_tilde *x)
{
	#ifdef THREADS
		int rc = pthread_join(x->childthread, NULL);
		if (rc != 0 )
			post("conv~: pthread_join() error %d.", rc);
	#endif

	/* Free channel variable arrays. */
	//free(x->parameters);
	free(x->input_rw_ptr);
	free(x->ir_end_ptr);
	free(x->ir_frames);
	free(x->frames_stored);
	free(x->irlength);
	free(x->ircount);
	free(x->ircount_prev);

	/* Free fft plans and associated arrays. */
	conv_tilde_free_fft_plans(x);

	/* free the dsp vector */
	if (x->x_myvec)
		t_freebytes(x->x_myvec, sizeof(t_int) * (x->all_channels + 3));
}


void conv_tilde_setup(void)
{
    /* Construct the class */
    conv_tilde_class = class_new(gensym("conv~"),
        (t_newmethod)conv_tilde_new,
        (t_method)conv_tilde_free, sizeof(t_conv_tilde),
        CLASS_DEFAULT, A_DEFFLOAT, 0);

    /* Add methods */
    class_addmethod(conv_tilde_class, (t_method)conv_tilde_dsp, gensym("dsp"), 0);
	class_addlist(conv_tilde_class, conv_tilde_parameters);
    class_addbang(conv_tilde_class, (t_method)conv_tilde_bang);

    /* Provide the default signal inlet */
    CLASS_MAINSIGNALIN(conv_tilde_class, t_conv_tilde, f);
}


void conv_tilde_init_fft_plans(t_conv_tilde* x)
{
	post("Allocating FFT arrays..");

	/* Allocate complex arrays for the fftw plans. */
	x->input_complex = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
	x->ir_complex = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
	x->out_complex = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
	x->outTemp = (float*) fftwf_malloc(sizeof(float) * x->fft_size);

	post("Allocating plans. This might take a while...");

	/* Create plans for in-place or out-of-place transform according to the
	INPLACE macro. */
	#ifdef INPLACE

        x->fftplan_in = fftwf_plan_dft_1d(x->fft_size,
            x->input_complex, x->input_complex, FFTW_FORWARD, FFTW_MEASURE);

        x->fftplan_ir = fftwf_plan_dft_1d(x->fft_size,
            x->ir_complex, x->ir_complex, FFTW_FORWARD, FFTW_MEASURE);

    #else
		/* Need additional real arrays for out-of-place transform. */
        x->input_to_fft = (float*) fftwf_malloc(sizeof(float) * x->fft_size);
        x->ir_to_fft = (float*) fftwf_malloc(sizeof(float) * x->fft_size);

        x->fftplan_in = fftwf_plan_dft_r2c_1d(x->fft_size,
            x->input_to_fft, x->input_complex, FFTW_MEASURE);

        x->fftplan_ir = fftwf_plan_dft_r2c_1d(x->fft_size,
            x->ir_to_fft, x->ir_complex, FFTW_MEASURE);
    #endif

	x->fftplan_inverse = fftwf_plan_dft_c2r_1d(x->fft_size,
		x->out_complex, x->outTemp, FFTW_MEASURE);

	/* With threads. */
	#ifdef THREADS
		post("Allocating arrays for thread 2...");

		x->input_complex_2 = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
		x->ir_complex_2 = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
		x->out_complex_2 = fftwf_alloc_complex(sizeof(fftwf_complex) * x->fft_size);
		x->outTemp_2 = (float*) fftwf_malloc(sizeof(float) * x->fft_size);

		post("Allocating plans for thread 2. This might take a while...");

		#ifdef INPLACE

		    x->fftplan_in_2 = fftwf_plan_dft_1d(x->fft_size,
		        x->input_complex_2, x->input_complex_2, FFTW_FORWARD, FFTW_MEASURE);

		    x->fftplan_ir_2 = fftwf_plan_dft_1d(x->fft_size,
		        x->ir_complex_2, x->ir_complex_2, FFTW_FORWARD, FFTW_MEASURE);

		#else
			/* Need additional real arrays for out-of-place transform. */
		    x->input_to_fft_2 = (float*) fftwf_malloc(sizeof(float) * x->fft_size);
		    x->ir_to_fft_2 = (float*) fftwf_malloc(sizeof(float) * x->fft_size);

		    x->fftplan_in_2 = fftwf_plan_dft_r2c_1d(x->fft_size,
		        x->input_to_fft_2, x->input_complex_2, FFTW_MEASURE);

		    x->fftplan_ir_2 = fftwf_plan_dft_r2c_1d(x->fft_size,
		        x->ir_to_fft_2, x->ir_complex_2, FFTW_MEASURE);
		#endif

		x->fftplan_inverse_2 = fftwf_plan_dft_c2r_1d(x->fft_size,
			x->out_complex_2, x->outTemp_2, FFTW_MEASURE);

	#endif
	post("Done!");
}


void conv_tilde_free_fft_plans(t_conv_tilde *x)
{
	int i;

	/* free storage vectors */
    for ( i = 0; i < x->channels; i++)
    {
        fftwf_free(x->stored_input[i]);
        fftwf_free(x->stored_ir[i]);
        fftwf_free(x->overlap_save[i]);
    }
    free(x->stored_input);
    free(x->stored_ir);
    free(x->overlap_save);

	/* free fft arrays */
	fftwf_free(x->input_complex);
	fftwf_free(x->ir_complex);
	fftwf_free(x->out_complex);
	fftwf_free(x->outTemp);

	#ifndef INPLACE
		fftwf_free(x->input_to_fft);
		fftwf_free(x->ir_to_fft);
	#endif

	/* destroy plans */
	fftwf_destroy_plan(x->fftplan_in);
	fftwf_destroy_plan(x->fftplan_ir);
	fftwf_destroy_plan(x->fftplan_inverse);

	#ifdef THREADS

		/* free fft arrays */
		fftwf_free(x->input_complex_2);
		fftwf_free(x->ir_complex_2);
		fftwf_free(x->out_complex_2);
		fftwf_free(x->outTemp_2);

		#ifndef INPLACE
			fftwf_free(x->input_to_fft_2);
			fftwf_free(x->ir_to_fft_2);
		#endif

		/* destroy plans */
		fftwf_destroy_plan(x->fftplan_in_2);
		fftwf_destroy_plan(x->fftplan_ir_2);
		fftwf_destroy_plan(x->fftplan_inverse_2);
	#endif


	/* Clean up to make sure all memory used by fftw is freed */
	//fftwf_cleanup_threads();
	fftwf_cleanup();
}

#ifdef THREADS
void* conv_tilde_parallel_thread(void* arg)
{
	int chan, n, i,H_ptr;
	t_sample *in, *ir, *out;

	/* The pointer to the object struct */
	t_conv_tilde *x = (t_conv_tilde*) arg;

	const unsigned int offset = 2;

	/* Loop the latter half of the total channels. */
	for ( chan = x->channels_halved; chan < x->channels; chan++ )
	{
		/* Check whether the IR has changed for either channel */
		if (x->ircount[chan] != x->ircount_prev[chan])
		{
			post("conv~: IR%d changed", chan);

			/* Start filling the arrays from the beginning */
			x->ir_end_ptr[chan] = 0;
			x->input_rw_ptr[chan] = 0;

			/* Truncate the IR if it is longer than STORAGELENGTH. */
			if ( x->irlength[chan] > STORAGELENGTH )
				x->irlength[chan] = STORAGELENGTH;

			/* Calculate the number of frames to store */
			x->ir_frames[chan] = ceil((float)x->irlength[chan] / x->framesize);

			x->frames_stored[chan] = 0;
			x->ircount_prev[chan] = x->ircount[chan];
		}

		/* ----- BYPASS ----------------------------------------------------- */

		/* If no IR is loaded, bypass the convolution to save cpu */
		if ( x->irlength[chan] <= 0 || x->bypass[chan] )
		{
			in = (t_sample *)(x->x_myvec[offset + chan]);
			out = (t_sample *)(x->x_myvec[offset - chan + x->all_channels - 1]);

			memcpy( out,
					in,
					sizeof(t_sample) * x->framesize );

			/* Skip the processing */
			continue;
		}

		/* ------------------------------------------------------------------ */

		/* Get the sample data from x->x_myvec this time. */

		in 	= (t_sample *)(x->x_myvec[offset + chan]);
		ir 	= (t_sample *)(x->x_myvec[offset + chan + x->channels]);
		out	= (t_sample *)(x->x_myvec[offset - chan + x->all_channels - 1]);

		/* This bit decides whether we append the frame to the storage or
		    overwrite the oldest one */
		if (x->frames_stored[chan] < x->ir_frames[chan])
		{
			/* This means that there are still IR buffers coming. */

			/* 1) Copy the input buffer to the REAL part of the input_complex array. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->input_complex_2, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->input_complex_2[x->framesize + i][0] = in[i];
					x->input_complex_2[x->framesize + i][1] = 0;
				}
			#else
				memcpy(x->input_to_fft_2 + x->framesize,
					in,
					sizeof(float) * x->framesize);
			#endif

			/* 2) Transform and store the input buffer. */

			fftwf_execute(x->fftplan_in_2);

			memcpy(x->stored_input[chan] + x->ir_end_ptr[chan],
				x->input_complex_2,
				sizeof(fftwf_complex) * x->fft_size);

			/* 3) Do the same to the IR buffer. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->ir_complex_2 + x->framesize, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->ir_complex_2[i][0] = ir[i];
					x->ir_complex_2[i][1] = 0;
				}
			#else
				memcpy(x->ir_to_fft_2,
					ir,
					sizeof(float) * x->framesize);
			#endif

		    fftwf_execute(x->fftplan_ir_2);

			memcpy(x->stored_ir[chan] + x->ir_end_ptr[chan],
				x->ir_complex_2,
				sizeof(fftwf_complex) * x->fft_size);

			/* 4) Increment storage pointers. */
			x->ir_end_ptr[chan] += x->fft_size;
			x->frames_stored[chan]++;

		    /* 5) Set the input read/write pointer forwards */
		    x->input_rw_ptr[chan] += x->fft_size;
		    if (x->input_rw_ptr[chan] >= x->ir_end_ptr[chan])
		        x->input_rw_ptr[chan] -= x->ir_end_ptr[chan];
		}
		else
		{
			/* IR is fully loaded. Overwrite stored audio frames (FIFO). */

			/* 1) Set the input read/write pointer forwards. */
		    x->input_rw_ptr[chan] += x->fft_size;
		    if (x->input_rw_ptr[chan] >= x->ir_end_ptr[chan])
		        x->input_rw_ptr[chan] -= x->ir_end_ptr[chan];

			/* 2) Copy the input buffer to the fft_input_array. */

			#ifdef INPLACE
				/* Zero pad. */
				memset(x->input_complex_2, 0, sizeof(fftwf_complex) * x->framesize);
				/* Copy samples. */
				for (i = x->framesize - 1; i >= 0; i--)
				{
					x->input_complex_2[x->framesize + i][0] = in[i];
					x->input_complex_2[x->framesize + i][1] = 0;
				}
			#else
				memcpy(x->input_to_fft_2 + x->framesize,
					in,
					sizeof(float) * x->framesize);
			#endif

			/* 3) Transform and store the input buffer. */

			fftwf_execute(x->fftplan_in_2);

			memcpy(x->stored_input[chan] + x->input_rw_ptr[chan],
				x->input_complex_2,
				sizeof(fftwf_complex) * x->fft_size);
		}



		/* - Convolve block by block -----------------------------------------*/

		H_ptr = 0;

		/* If the process is here then there is bound to be at least one frame stored.
		The first frame in the sum overwrites the output array. The iteration goes down
		as ARM CPU compares against zero the fastest. */
		for (n = x->fft_size - 1; n >= 0; n--)
		{
		    /* Complex multiplication (frequency domain convolution):

			Re(Y) = Re(X)Re(H) - Im(X)Im(H)
			Im(Y) = Im(X)Re(H) + Re(X)Im(H)

			Nice 4 divisible iteration length for compiler SIMD optimization.
			Note that the first iteration has to overwrite the output array.

                                        | chan | sample                     | Re/Im | */
		    x->out_complex_2[n][0]
					=	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [0]
					*	x->stored_ir    [chan] [H_ptr + n]                  [0]
					-	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [1]
					*	x->stored_ir    [chan] [H_ptr + n]                  [1];

		    x->out_complex_2[n][1]
					=	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [1]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [0]
					+	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [0]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [1];
		 }

		/* Move the IR pointer forwards */
		H_ptr += x->fft_size;

		/* Move the input read/write pointer backwards. */
		x->input_rw_ptr[chan] -= x->fft_size;
		if (x->input_rw_ptr[chan] < 0 )
		    x->input_rw_ptr[chan] += x->ir_end_ptr[chan];

		/* If many frames in storage, repeat and sum the results. */
		for (i = x->frames_stored[chan] - 1; i > 0; i--)
		{
		    for (n = x->fft_size - 1; n >= 0; n--)
		    {
		    	x->out_complex_2[n][0]
					+=	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [0]
					*	x->stored_ir    [chan] [H_ptr + n]                  [0]
					-	x->stored_input [chan] [x->input_rw_ptr[chan] + n]  [1]
					*	x->stored_ir    [chan] [H_ptr + n]                  [1];

		    	x->out_complex_2[n][1]
					+=	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [1]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [0]
					+	x->stored_input [chan]  [x->input_rw_ptr[chan] + n] [0]
					*	x->stored_ir    [chan]  [H_ptr + n]                 [1];
		     }

		    /* Move the IR pointer forwards */
		    H_ptr += x->fft_size;

		    /* Move the input read/write pointer backwards. */
		    x->input_rw_ptr[chan] -= x->fft_size;
		    if (x->input_rw_ptr[chan] < 0 )
		        x->input_rw_ptr[chan] += x->ir_end_ptr[chan];
		}


		/* Insert the previous overlap save portion before overwriting the OS. */
		memcpy(out,
				x->overlap_save[chan],
				sizeof(float) * x->framesize);

		/* Inverse fft. Result is stored in x->outTemp_2. */
		fftwf_execute(x->fftplan_inverse_2);

		/* Store the overlap save portion. */
        memcpy(x->overlap_save[chan],
				x->outTemp_2,
				sizeof(float) * x->framesize);

		/* Sum the output with the previous overlap and scale the amplitude. */

		for (i = x->framesize - 1; i >= 0; i--)
		{
			out[i] += x->outTemp_2[i + x->framesize];
			out[i] *= x->out_gain;
		}
	}
	/* Exit thread. */
	pthread_exit(NULL);
}
#endif

