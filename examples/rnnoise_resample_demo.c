/* Copyright (c) 2018 Gregor Richards
 * Copyright (c) 2017 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if defined(_WIN32) && !defined(__CYGWIN__)
# include <io.h>
# include <fcntl.h>
# define SET_BINARY_MODE(handle) setmode(fileno(handle), O_BINARY)
#else
# define SET_BINARY_MODE(handle) ((void)0)
#endif

#define SET_BINARY_MODE(handle) ((void)0)

#define debug_fprintf if(debug) fprintf

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "rnnoise.h"

#include "libsamplerate/samplerate.h"

#define FRAME_SIZE_48K 480
#define FRAME_SIZE_16K 160

void shortToFloat(short *input, float *output, int length) {
  for (int i = 0; i < length; i++) {
	float f = ((float) input[i]) / (float) 32768;
	if(f > 1) {
		f = 1;
	} else if(f < -1 ) {
		f = -1;
	}
	output[i] = f;
  }
}

void floatToShort(float *input, short *output, int length) {
  for (int i = 0; i < length; i++) {
	float f = input[i] * 32768;
	if(f > 32767) {
		f = 32767;
	} else if(f < -32768 ) {
		f = -32768;
	}
	output[i] = (short)f;
  }
}

/*
size_t myfread2(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t tmp = 0;
	size_t r = 0;
	char *cptr = ptr;

	while(nmemb > 0 && (tmp = fread(cptr + (size * r), size, nmemb, stream)) > 0) {
		r += tmp;
		nmemb -= tmp;
//		printf("read %ld\n", (long int)tmp);
	}

	return r;
}

size_t myfread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	ssize_t r = read(fileno(stream), ptr, size * nmemb);

	if(r < 0) {
		perror("read");
		exit(1);
	}
	return r / size;
}
*/

int main(int argc, char **argv) {
	int i;
	float x[FRAME_SIZE_48K];
	FILE *fin, *fout;
	DenoiseState *st;
	int error = 0;
	int debug = 0;

	if (argc==4 && strcmp(argv[3], "debug") != 0 || (argc!=4 && argc!=3)) {
		fprintf(stderr, "usage: %s <noisy speech> <output denoised>\n", argv[0]);
		return 1;
	}

	debug = argc == 4;

	st = rnnoise_create(NULL);

	SRC_STATE *stateTo48k = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
	if(!stateTo48k) {
	  fprintf(stderr, "src_new 48k = %d %s\n", error, src_strerror(error));
	  exit(1);
	}
	SRC_STATE *stateTo16k = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
	if(!stateTo16k) {
	  fprintf(stderr, "src_new 16k = %d %s\n", error, src_strerror(error));
	  exit(1);
	}

	SRC_DATA dataTo48k;
	SRC_DATA dataTo16k;
	float fframes48k[FRAME_SIZE_48K];
	float fframes16k[FRAME_SIZE_16K];
	memset(fframes48k, 0, sizeof(fframes48k));
	memset(fframes16k, 0, sizeof(fframes16k));
	{
		dataTo48k.data_in = fframes16k;
		dataTo48k.data_out = fframes48k;
		dataTo48k.input_frames = FRAME_SIZE_16K;
		dataTo48k.output_frames = FRAME_SIZE_48K;
		dataTo48k.end_of_input = 0;
		dataTo48k.src_ratio = 48000.0 / 16000.0;
		debug_fprintf(stderr, "48k input_frames: %ld output_frames: %ld src_ratio: %f\n", dataTo48k.input_frames, dataTo48k.output_frames, dataTo48k.src_ratio);
		debug_fprintf(stderr, "48k data_in: %p data_out: %p end_of_input: %d\n", (void*)dataTo48k.data_in, (void*)dataTo48k.data_out, dataTo48k.end_of_input);
	}

	{
		dataTo16k.data_in = fframes48k;
		dataTo16k.data_out = fframes16k;
		dataTo16k.input_frames = FRAME_SIZE_48K;
		dataTo16k.output_frames = FRAME_SIZE_16K;
		dataTo16k.end_of_input = 0;
		dataTo16k.src_ratio = 16000.0 / 48000.0;
		debug_fprintf(stderr, "16k input_frames: %ld output_frames: %ld src_ratio: %f\n", dataTo16k.input_frames, dataTo16k.output_frames, dataTo16k.src_ratio);
		debug_fprintf(stderr, "16k data_in: %p data_out: %p end_of_input: %d\n", (void*)dataTo16k.data_in, (void*)dataTo16k.data_out, dataTo16k.end_of_input);
	}

	/* set empty frame:
	 * "By way of example, the first time you call src_process() you might only get 1900 samples out.
	 * However, after that first call all subsequent calls will probably get you about 2000 samples out for every 1000 samples you put in."
	 */
	// it will be better when we insert more samples in the first call.
	if(error = src_process(stateTo48k, &dataTo48k)) {
		fprintf(stderr, "src_process/48k = %d %s\n", error, src_strerror(error));
		fprintf(stderr, "src_ratio: %f\n", dataTo48k.src_ratio);
		exit(1);
	}
	debug_fprintf(stderr, "48k input_frames: %ld output_frames: %ld src_ratio: %f input_frames_used: %ld output_frames_gen: %ld\n", dataTo48k.input_frames, dataTo48k.output_frames, dataTo48k.src_ratio, dataTo48k.input_frames_used, dataTo48k.output_frames_gen);
	debug_fprintf(stderr, "48k data_in: %p data_out: %p end_of_input: %d\n", (void*)dataTo48k.data_in, (void*)dataTo48k.data_out, dataTo48k.end_of_input);
	if(error = src_process(stateTo16k, &dataTo16k)) {
		fprintf(stderr, "src_process/16k = %d %s\n", error, src_strerror(error));
		fprintf(stderr, "src_ratio: %f\n", dataTo16k.src_ratio);
		exit(1);
	}
	debug_fprintf(stderr, "16k input_frames: %ld output_frames: %ld src_ratio: %f input_frames_used: %ld output_frames_gen: %ld\n", dataTo16k.input_frames, dataTo16k.output_frames, dataTo16k.src_ratio, dataTo16k.input_frames_used, dataTo16k.output_frames_gen);
	debug_fprintf(stderr, "16k data_in: %p data_out: %p end_of_input: %d\n", (void*)dataTo16k.data_in, (void*)dataTo16k.data_out, dataTo16k.end_of_input);

	SET_BINARY_MODE(stdin);
	SET_BINARY_MODE(stdout);
//	fin = (strcmp(argv[1], "-") == 0) ? stdin : fopen(argv[1], "rb");
//	fout = (strcmp(argv[2], "-") == 0) ? stdout : fopen(argv[2], "wb");
	fin = (strcmp(argv[1], "-") == 0) ? fdopen(fileno(stdin), "rb") : fopen(argv[1], "rb");
	fout = (strcmp(argv[2], "-") == 0) ? fdopen(fileno(stdout), "wb") : fopen(argv[2], "wb");

	setvbuf(fin, NULL, _IONBF, 0);
	setvbuf(fout, NULL, _IONBF, 0);

	while (!feof(fin)) {
		short sframes16k[FRAME_SIZE_16K];
		short sframes48k[FRAME_SIZE_48K];
		size_t frames = fread(sframes16k, sizeof(short), FRAME_SIZE_16K, fin);
		debug_fprintf(stderr, "read %lu frames\n", (long unsigned int)frames);
		if(!frames) {
			debug_fprintf(stderr, "EOF\n");
			break;
		}

		if(frames != FRAME_SIZE_16K) {
			debug_fprintf(stderr, "last frame\n");
			dataTo48k.end_of_input = 1;
			dataTo16k.end_of_input = 1;
			dataTo48k.input_frames = frames;
			dataTo16k.input_frames = frames;
			dataTo48k.output_frames = frames * 3.0;
			dataTo16k.output_frames = frames / 3.0;
		}

		// upsample to 48k
		{
			shortToFloat(sframes16k, fframes16k, frames);
			{
				if(error = src_process(stateTo48k, &dataTo48k)) {
				  fprintf(stderr, "src_process = %d %s\n", error, src_strerror(error));
				  exit(1);
				}

				if(!dataTo48k.output_frames_gen || dataTo48k.output_frames_gen != dataTo48k.output_frames) {
					fprintf(stderr, "missing frames in %ld/%ld out %ld/%ld ratio %f\n", dataTo48k.input_frames, (long int)frames, dataTo48k.output_frames, dataTo48k.output_frames_gen, dataTo48k.src_ratio);
					break;
				}
				debug_fprintf(stderr, "up %ld / %ld frames\n", dataTo48k.output_frames_gen, dataTo48k.output_frames);
			}
			floatToShort(fframes48k, sframes48k, dataTo48k.output_frames_gen);
		}

		// rnnoise_process_frame
		{
			for (i=0;i<dataTo48k.output_frames_gen;i++) {
				x[i] = sframes48k[i];
			}
			rnnoise_process_frame(st, x, x);
			for (i=0;i<dataTo48k.output_frames_gen;i++) {
				sframes48k[i] = x[i];
			}
		}

		// downsample to 16k
		{
			shortToFloat(sframes48k, fframes48k, dataTo48k.output_frames_gen);
			{
				if(error = src_process(stateTo16k, &dataTo16k)) {
				  fprintf(stderr, "src_process = %d %s\n", error, src_strerror(error));
				  exit(1);
				}
				if(!dataTo16k.output_frames_gen || dataTo16k.output_frames_gen != dataTo16k.output_frames) {
					fprintf(stderr, "missing frames in %ld/%ld out %ld/%ld ratio %f\n", dataTo16k.input_frames, dataTo48k.output_frames_gen, dataTo16k.output_frames, dataTo16k.output_frames_gen, dataTo16k.src_ratio);
					break;
				}
				debug_fprintf(stderr, "down %ld / %ld frames\n", dataTo16k.output_frames_gen, dataTo16k.output_frames);
			}
			floatToShort(fframes16k, sframes16k, dataTo16k.output_frames_gen);
		}

		frames = fwrite(sframes16k, sizeof(short), dataTo16k.output_frames_gen, fout);
		debug_fprintf(stderr, "write %lu frames\n", (long unsigned int)frames);
	}

	debug_fprintf(stderr, "finished\n");
	if(strcmp(argv[1], "-") != 0) {
		fclose(fin);
	}
	if(strcmp(argv[2], "-") != 0) {
		fclose(fout);
	}
	src_delete(stateTo16k);
	src_delete(stateTo48k);
	rnnoise_destroy(st);

	return 0;
}
