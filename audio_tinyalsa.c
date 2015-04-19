/*
 * tinyalsa output driver. This file is part of Shairport.
 * Copyright (c) Joseph C. Lehner 2015
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
#include <memory.h>
#include <tinyalsa/asoundlib.h>
#include "common.h"
#include "audio.h"

static void help(void);
static int init(int argc, char **argv);
static void deinit(void);
static void start(int sample_rate);
static void play(short buf[], int samples);
static void stop(void);
static void volume(double vol);

audio_output audio_tinyalsa = {
    .name = "tinyalsa",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .start = &start,
    .stop = &stop,
	.flush = NULL,
	.delay = NULL,
    .play = &play,
    .volume = NULL,
	.parameters = NULL
};

static struct pcm *pcm = NULL;

static struct mixer *alsa_mix_handle = NULL;
static struct mixer_ctl *volume_ctl = NULL;

static char *alsa_out_dev = "default";
static char *alsa_mix_dev = NULL;
static char *alsa_mix_ctrl = "Master";
static int alsa_mix_index = 0;
static int card = 0;
static int device = 0;
static int rate = 44100;

static void help(void) {
    printf("    -C alsa-card        set the card id [0*|...]\n"
           "    -d alsa-device      set the device id [0*|...]\n"
		   "    -r sample-rate      set the sample rate[44100*|...]\n"
           //"    -i mixer-index      set the mixer index [0*|...]\n"
           "    *) default option\n"
          );
}

static int init(int argc, char **argv) {
    int hardware_mixer = 0;

    optind = 1; // optind=0 is equivalent to optind=1 plus special behaviour
    argv--;     // so we shift the arguments to satisfy getopt()
    argc++;
    // some platforms apparently require optreset = 1; - which?
    int opt;
    while ((opt = getopt(argc, argv, "C:d:r:")) > 0) {
        switch (opt) {
            case 'C':
                card = strtol(optarg, NULL, 10);
                break;
            case 'd':
                device = strtol(optarg, NULL, 10);
                break;
			case 'r':
                rate = strtol(optarg, NULL, 10);
                break;
            default:
                help();
                die("Invalid audio option -%c specified", opt);
        }
    }

    if (optind < argc)
        die("Invalid audio argument: %s", argv[optind]);

	struct pcm_config config = {
		.channels = 2,
		.rate = rate,
		.period_size = 512,
		.period_count = 4,
		.format = PCM_FORMAT_S16_LE,
		.start_threshold = 512 * 1
	};

	pcm = pcm_open(card, device, PCM_OUT, &config);
	if (!pcm || !pcm_is_ready(pcm))
		die("Failed to open PCM device %d:%d: %s", card, device, pcm_get_error(pcm));

	alsa_mix_handle = mixer_open(card);
	if (alsa_mix_handle) {
		volume_ctl = mixer_get_ctl_by_name(alsa_mix_handle, "Volume");
	} else {
		warn("No mixer available!");
	}

	return 0;
}

static void deinit(void) {
	if (alsa_mix_handle)
		mixer_close(alsa_mix_handle);

    stop();
}

static void start(int sample_rate) {
}

static void play(short buf[], int samples) {
	int err = pcm_write(pcm, buf, samples);
	if (err < 0)
        die("Failed to write to PCM device: %s", pcm_get_error(pcm));
}

static void stop(void) {
    if (pcm) {
		pcm_close(pcm);
		pcm = NULL;
    }
}

static void volume(double vol) {

}
