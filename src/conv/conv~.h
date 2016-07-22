/* ------------------------------- conv~ ------------------------------------- */
/*                                                                              */
/* Description of the external.                                                 */
/*                                                                              */
/* Author: Jussi Nieminen                                                       */
/*                                                                              */
/* ---------------------------------------------------------------------------- */

#define DEFAULT_AUDIO_CHANNELS 32
#define DEFAULT_AUDIO_SOURCES 16
#define STORAGELENGTH 960000 	/* 20s @48,0kHz | 10s @96kHz sample rate*/
#define DEFAULTFRAMESIZE 1024
#define NUMBER_OF_PARAMETERS ( 3 * DEFAULT_AUDIO_SOURCES )

/* For clipping a value between lo and hi */
#ifndef CLIP
#define CLIP(a, lo, hi) ( (a)>(lo)?( (a)<(hi)?(a):(hi) ):(lo) )
#endif


static t_class *conv_tilde_class;
typedef struct _conv_tilde {

	t_object  x_obj;				/* PD object. size 48 */
	t_int** x_myvec;				/* vector we pass on in the DSP routine */

	unsigned int fft_size;			/* fft size */

	fftwf_complex** __restrict stored_input;		/* arrays to store input ...	*/
	fftwf_complex** __restrict stored_ir;		/* ... and ir data in f-domain	*/

	fftwf_plan fftplan_in;						/* fft plan for the input. */
	fftwf_complex* __restrict input_complex;	/* Complex output of the input FFT */
	
	fftwf_plan fftplan_ir;					/* fft plan for the impulse resp. */
	fftwf_complex* __restrict ir_complex;	/* Complex output of the ir FFT */

	fftwf_plan fftplan_inverse;				/* fft plan for the output. */
	fftwf_complex* __restrict out_complex;	/* Complex input to the out FFT. */
	float* __restrict outTemp;				/* Real output -> out of place transform.*/

	/* Additional real arrays for out-of-place Transform. */
	#ifndef INPLACE
		float* __restrict input_to_fft;
		float* __restrict ir_to_fft;
	#endif

	float** __restrict overlap_save;

	int* ir_end_ptr;			/* Points to the end of the input storage. */
	int* input_rw_ptr;			/* Ring buffer ptr to the input storage array */
	
	int framesize;					/* the current framesize */
	
	t_float* parameters;			/* temporary array for storing parameters */
	int* irlength;					/* ir length */
	int* ircount;					/* used to determine when an ir changes */
	int* ir_frames;					/* the ir length in number of frames */
	int* ircount_prev;				/* the ir count at the previous frame */
	int* frames_stored;				/* number of frames currently in store */
	int* bypass;					/* Vector for bypass flags. */

	#ifdef THREADS
		pthread_t childthread;
		
		t_sample* __restrict in;
		t_sample* __restrict ir;
		t_sample* __restrict out;

		fftwf_plan fftplan_in_2;					/* fft plan for the input. */
		fftwf_complex* __restrict input_complex_2;	/* Complex output of the input FFT */
	
		fftwf_plan fftplan_ir_2;				/* fft plan for the impulse resp. */
		fftwf_complex* __restrict ir_complex_2;	/* Complex output of the ir FFT */

		fftwf_plan fftplan_inverse_2;			/* fft plan for the output. */
		fftwf_complex* __restrict out_complex_2;/* Complex input to the out FFT. */
		float* __restrict outTemp_2;			/* Real output -> out of place transform.*/

		#ifndef INPLACE
			float* __restrict input_to_fft_2;
			float* __restrict ir_to_fft_2;
		#endif

		int channels_halved;

	#endif

	/* Some utility variables */
	float out_gain;					/* Coefficient for output scaling. Replaces div with mul. */
	uint32_t samplerate;			/* samplerate we're running at */
	int channels;					/* number of channels we want to stream */
    int all_channels;
	t_float f;                  	/* dummy variable for silent input */

} t_conv_tilde;


void conv_tilde_init_fft_plans(t_conv_tilde* x);

void conv_tilde_free_fft_plans(t_conv_tilde* x);

#ifdef THREADS
	void* conv_tilde_parallel_thread(void* arg);
#endif

