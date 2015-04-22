/*
 * OpenSLES output driver. This file is part of Shairport.
 * Copyright (c) Joseph C. Lehner
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <SLES/OpenSLES.h>
#include "common.h"
#include "player.h"
#include "audio.h"

#if 0
#include <SLES/OpenSLES_Android.h>
#undef SL_IID_BUFFERQUEUE
#define SL_IID_BUFFERQUEUE SL_IID_ANDROIDSIMPLEBUFFERQUEUE
#define SLBufferQueueItf SLAndroidSimpleBufferQueueItf
#define SLDataLocator_BufferQueue SLDataLocator_AndroidSimpleBufferQueue
#endif

#define BUFFER_COUNT 128
#define BUFFER_SAMPLES 1024
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

static SLObjectItf engine_obj = NULL;
static SLObjectItf mix_obj = NULL;
static SLBufferQueueItf bq_itf = NULL;
static SLObjectItf player_obj = NULL;
static SLPlayItf player_itf = NULL;
static SLVolumeItf volume_itf = NULL;
static SLDataLocator_OutputMix mix_loc ;
static SLDataLocator_BufferQueue bq_loc;
static SLDataFormat_PCM fmt;
static SLDataSource src;
static SLDataSink sink;

static SLInterfaceID ids[3]; 
static SLboolean req[3];

// software volume, in the range 0.0 - 1.0
static double sw_volume = 1.0;

static short *buffers[BUFFER_COUNT];
static int buffer_index = 0;
static short *buffer_p = NULL;
// number of samples stored in current buffer
static int buffer_fill = 0;

// timestamp (in milliseconds) of the first call
// to play() after a stop()
static long long started = 0;
// at 44100 Hz, this overflows every 6.6 million years
static long long samples_played = 0;

void buffers_alloc() {
	int i = 0;
	for (; i != BUFFER_COUNT; ++i) {
		buffers[i] = malloc(4 * BUFFER_SAMPLES);
	}

	buffer_p = buffers[0];
}

void buffers_free() {
	int i = 0;
	for(; i != BUFFER_COUNT; ++i) {
		free(buffers[i]);
		buffers[i] = NULL;
	}
}

void buffers_next() {
	buffer_index = (buffer_index + 1) % BUFFER_COUNT;
	buffer_p = buffers[buffer_index];
	buffer_fill = 0;
}

static void sl_perror(SLresult res, const char* message) {

	fprintf(stderr, "opensles: %s: ", message);
#define HANDLE(result) \
	case SL_RESULT_ ## result: \
		fprintf(stderr, "%s (%d)\n", #result, SL_RESULT_ ## result); \
		break;

	switch (res) {
		HANDLE(PRECONDITIONS_VIOLATED);
		HANDLE(PARAMETER_INVALID);
		HANDLE(MEMORY_FAILURE);
		HANDLE(RESOURCE_ERROR);
		HANDLE(RESOURCE_LOST);
		HANDLE(IO_ERROR);
		HANDLE(BUFFER_INSUFFICIENT);
		HANDLE(CONTENT_CORRUPTED);
		HANDLE(CONTENT_UNSUPPORTED);
		HANDLE(CONTENT_NOT_FOUND);
		HANDLE(PERMISSION_DENIED);
		HANDLE(FEATURE_UNSUPPORTED);
		HANDLE(INTERNAL_ERROR);
		HANDLE(UNKNOWN_ERROR);
		HANDLE(OPERATION_ABORTED);
		HANDLE(CONTROL_LOST);
		default:
			fprintf(stderr, "(unknown error %d)\n", res);
	}
#undef HANDLE
}

static void bq_callback(SLBufferQueueItf caller, void *context) {
	// TODO can we use this for delay calculation?
}

static SLresult do_play(short[], int);
static void start(int sample_rate);
static void stop();

static int init(int argc, char **argv) {

	SLresult res = slCreateEngine(&engine_obj, 0, NULL, 0, NULL, NULL);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to create engine");
		return -1;
	}

	res = (*engine_obj)->Realize(engine_obj, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to realize engine");
		return -1;
	}

	SLEngineItf engine_itf;

	res = (*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine_itf);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to get engine interface");
		return -1;
	}

	res = (*engine_itf)->CreateOutputMix(engine_itf, &mix_obj, 0, NULL, NULL);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to create output mix");
		return -1;
	}

	res = (*mix_obj)->Realize(mix_obj, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to realize output mix");
		return -1;
	}

	mix_loc.locatorType = SL_DATALOCATOR_OUTPUTMIX;
	mix_loc.outputMix = mix_obj;
	
	bq_loc.locatorType = SL_DATALOCATOR_BUFFERQUEUE;
	bq_loc.numBuffers = BUFFER_COUNT;

	fmt.formatType = SL_DATAFORMAT_PCM;
	fmt.numChannels = 2;
	fmt.samplesPerSec = SL_SAMPLINGRATE_44_1;
	fmt.bitsPerSample = SL_PCMSAMPLEFORMAT_FIXED_16;
	fmt.containerSize = SL_PCMSAMPLEFORMAT_FIXED_16;
	fmt.channelMask = SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT;
	fmt.endianness = SL_BYTEORDER_LITTLEENDIAN;

	src.pLocator = &bq_loc;
	src.pFormat = &fmt;

	sink.pLocator = &mix_loc;
	sink.pFormat = NULL;

	ids[0] = SL_IID_BUFFERQUEUE; 
	ids[1] = SL_IID_PLAY;
   	ids[2] = SL_IID_VOLUME;

	req[0] = req[1] = SL_BOOLEAN_TRUE;
	req[2] = SL_BOOLEAN_FALSE;

	res = (*engine_itf)->CreateAudioPlayer(engine_itf, &player_obj, &src, 
			&sink, 2, ids, req);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to create audio player");
		return -1;
	}

	res = (*player_obj)->Realize(player_obj, SL_BOOLEAN_FALSE);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to realize audio player player_obj");
		return -1;
	}

	res = (*player_obj)->GetInterface(player_obj, SL_IID_PLAY, &player_itf);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to get play interface");
		return -1;
	}

	res = (*player_obj)->GetInterface(player_obj, SL_IID_BUFFERQUEUE, &bq_itf);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to get buffer-queue interface");
		return -1;
	}

	res = (*bq_itf)->RegisterCallback(bq_itf, bq_callback, NULL);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to register buffer-queue callback");
		return -1;
	}

	res = (*player_obj)->GetInterface(player_obj, SL_IID_VOLUME, &volume_itf);
	if (res != SL_RESULT_SUCCESS) {
		inform("Hardware volume control not available");
		volume_itf = NULL;
	}

	buffers_alloc();

	// play a 100ms test (silent) test tone, so we can detect errors early on

	short silence[4410 * 4];
	memset(silence, 0, ARRAY_SIZE(silence));

	start(44100);

	if (do_play(silence, 2048) != SL_RESULT_SUCCESS) {
		// do_play will print an error message
		return -1;
	}

	stop();

	return 0;
}

static void start(int sample_rate) {
	if (sample_rate != 44100)
		die("Unexpected sample rate");

	// this is set by the first call to do_play()
	started = 0;
}

static long long now(void) {
#if 1
	static struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_usec + 1e6 * tv.tv_sec;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_nsec / 1000 + ts.tv_sec * 1000000;
#endif
}

static SLresult do_play(short buf[], int samples) {

	if (!started) {
		started = now();
	}

	if (!volume_itf && sw_volume < 1.0) {
		int i = 0;
		for (; i != samples * 2; ++i) {
			buf[i] *= sw_volume;
		}
	}

	SLresult res;

	if (buffer_fill) {
		// We have a partially filled buffer. Let's attempt to
		// fill it

		int count = MIN(BUFFER_SAMPLES - buffer_fill, samples);
		memcpy(buffer_p + (buffer_fill * 4), buf, count * 4);
		buffer_fill += count;

		samples -= count;
		buf += count * 4;

		if (!samples) {
			// Buffer is not full yet, wait for more samples
			//debug(1, "waiting for more data");
			goto out;
		} else {
			res = (*bq_itf)->Enqueue(bq_itf, buffer_p, count * 4);
			if (res != SL_RESULT_SUCCESS) {
				sl_perror(res, "Failed to enqueue buffer");
				goto err;
			}

			buffers_next();
		}
	}

	int copied = 0;
	while (copied < samples) {
		int count = MIN(samples - copied, BUFFER_SAMPLES);
		memcpy(buffer_p, buf, count * 4);
		copied += count;

		if (1 /*&& count == BUFFER_SAMPLES*/) {
			res = (*bq_itf)->Enqueue(bq_itf, buffer_p, count * 4);
			if (res != SL_RESULT_SUCCESS) {
				sl_perror(res, "Failed to enqueue buffer");
				goto err;
			}

			buffers_next();
		} else {
			// Wait for more data
			buffer_fill = count;
		}
	}

	res = (*player_itf)->SetPlayState(player_itf, SL_PLAYSTATE_PLAYING);
	if (res != SL_RESULT_SUCCESS) {
		sl_perror(res, "Failed to set play state");
		goto err;
	}

out:
	;

#if 0
	samples_played += samples;

	long long finish = started + (samples_played * 1e6) / 44100;
	long long time = finish - now();
	if (time > 0) {
		usleep(time);
	}
#else
	SLBufferQueueState state;

	do
	{
		usleep(100);
		res = (*bq_itf)->GetState(bq_itf, &state);
	} while (res == SL_RESULT_SUCCESS && state.count);
#endif

err:
	return res;
}

static void play(short buf[], int samples) {
	if (do_play(buf, samples) != SL_RESULT_SUCCESS) {
		die("Fatal error while playing");
	}
}

static void stop(void) {
	if (player_itf) {
		(*player_itf)->SetPlayState(player_itf, SL_PLAYSTATE_STOPPED);
		// Ignore return value
	}

	if (bq_itf) {
		(*bq_itf)->Clear(bq_itf);
		// Ignore return value
	}

	started = 0;
}

static void volume(double vol) {
    if (!volume_itf) {
		// According to the RAOP specification, volume is in the range
		// -30.0 to 0.0 (loudest). Mute is -144.0. For our purposes,
		// we use the range 0.0 (mute) to 1.0 (loudest).
		sw_volume = vol <= -30.0 ? 0.0 : ((vol + 30.0) / 30.0);
	} else {

		if (vol >= 30.0) {
			if (vol < SL_MILLIBEL_MIN) {
				vol = SL_MILLIBEL_MIN;
			} else if (vol > 0.0) {
				vol = 0.0;
			}

			SLresult res = (*volume_itf)->SetVolumeLevel(volume_itf, vol);
			if (res != SL_RESULT_SUCCESS) {
				sl_perror(res, "Failed to set volume level");
				goto err;
			}
		} else {
			SLresult res = (*volume_itf)->SetMute(volume_itf, 1);
			if (res != SL_RESULT_SUCCESS) {
				sl_perror(res, "Failed to mute volume");
				goto err;
			}
		}

		return;

err:
		// Assume it will also fail next time, so we use software
		// volume control as fallback.
		volume_itf = NULL;
		volume(vol);
		
	}
}

static void flush(void) {
	if (!player_itf || !bq_itf) {
		return;
	}

	(*player_itf)->SetPlayState(player_itf, SL_PLAYSTATE_STOPPED);
	(*bq_itf)->Clear(bq_itf);
	(*player_itf)->SetPlayState(player_itf, SL_PLAYSTATE_PLAYING);
}

static void deinit(void) {
	if (player_obj) {
		(*player_obj)->Destroy(player_obj);
		player_obj = NULL;
	}
	if (mix_obj) {
		(*mix_obj)->Destroy(mix_obj);
		mix_obj = NULL;
	}
	if (engine_obj) {
		(*engine_obj)->Destroy(engine_obj);
		engine_obj = NULL;
	}

	buffers_free();
}

static void help(void) {
    printf("    opensles takes no arguments");
}

audio_output audio_opensles = {
    .name = "opensles",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .start = &start,
    .stop = &stop,
    .flush = &flush,
    .delay = NULL,
    .play = &play,
    .volume = &volume,
    .parameters = NULL
};
