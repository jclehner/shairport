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
#include <sound/asound.h>
#include "common.h"
#include "audio.h"

static void help(void);
static int init(int argc, char **argv);
static void deinit(void);
static void start(int sample_rate);
static uint32_t delay(void);
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

static int autodetect = 0;
#ifdef __ANDROID__
static int mmap = 0;
#else
static int mmap = 1;
#endif
static int card = 0;
static int device = 0;
static int rate = 44100;

static void help(void) {
    printf("    -A                  autodetect alsa card/device\n"
#ifdef __ANDROID__
           "    -M mmap             set mmap enabled/disabled [0|1*]\n"
#else
           "    -M mmap             set mmap enabled/disabled [0*|1]\n"
#endif

           "    -C alsa-card        set the card id [0*|...]\n"
           "    -d alsa-device      set the device id [0*|...]\n"
		   //"    -r sample-rate      set the sample rate[44100*|...]\n"
           "    *) default option\n"
          );
}

static struct pcm *open_pcm(int card, int device)
{
    struct pcm_params *p;
    int flags = PCM_OUT | (mmap ? PCM_MMAP : 0);

	struct pcm_config config = {
		.channels = 2,
		.rate = rate,
		.period_size = 512,
		.period_count = 4,
		.format = PCM_FORMAT_S16_LE,
		.start_threshold = 512 * 1
	};

    p = pcm_params_get(card, device, PCM_OUT);
    if (p) {
        config.period_size = pcm_params_get_min(p, PCM_PARAM_PERIOD_SIZE);
        config.period_count = pcm_params_get_min(p, PCM_PARAM_PERIODS);
        config.start_threshold = config.period_size * 1;

        pcm_params_free(p);
    }

    return pcm_open(card, device, flags, &config);
}

static init_pcm() {

    char path[128];

    if (autodetect) {
        int found = 0;
        for (; card != 127; ++card) {
            for (; device != 255; ++device) {
                snprintf(path, sizeof(path), 
                        "/dev/snd/pcmC%dD%dp", card, device);

                if (access(path, F_OK) == 0) {
                    pcm = open_pcm(card, device);
                    if (pcm && pcm_is_ready(pcm)) {
                        inform("Using PCM %d:%d", card, device);
                        found = 1;
                        break;
                    } 
                } else {
                    //break;
                }
            }

            if (found /*|| device == 0*/) {
                break;
            }
        }

        if (!found) {
            die("Autodetection of PCM device failed");
        }
    } else {
        pcm = open_pcm(card, device);
        if (!pcm || !pcm_is_ready(pcm))
            die("PCM device %d:%d: %s", card, device, pcm_get_error(pcm));
    }

	debug(2, "PCM device %d:%d intialized", card, device);
}


static int init(int argc, char **argv) {
    int hardware_mixer = 0;

    optind = 1; // optind=0 is equivalent to optind=1 plus special behaviour
    argv--;     // so we shift the arguments to satisfy getopt()
    argc++;
    // some platforms apparently require optreset = 1; - which?
    int opt;
    while ((opt = getopt(argc, argv, "AM:D:C:d:")) > 0) {
        switch (opt) {
            case 'A':
                autodetect = 1;
                break;
            case 'M':
                mmap = optarg[0] == '1' ? 1 : 0;
                break;
            case 'C':
                card = strtol(optarg, NULL, 10);
                break;
            case 'd':
                device = strtol(optarg, NULL, 10);
                break;
			//case 'r':
            //    rate = strtol(optarg, NULL, 10);
            //    break;
            default:
                help();
                die("Invalid audio option -%c specified", opt);
        }
    }

    if (optind < argc)
        die("Invalid audio argument: %s", argv[optind]);

    init_pcm();

    if(pcm_write(pcm, NULL, 0) < 0)
        die("First write failed: %s", pcm_get_error(pcm));

	return 0;
}

static void deinit(void) {
    stop();
}

static void start(int sample_rate) {
	if (rate != 44100)
		die("Unexpected sample rate");
}

static void play(short buf[], int samples) {
	int err = pcm_write(pcm, (char *)buf, samples * 4);
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
