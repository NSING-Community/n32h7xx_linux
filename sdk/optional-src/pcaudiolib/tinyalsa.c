/* TinyALSA output backend for pcaudiolib.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pcaudiolib's own list of sinks is PulseAudio, ALSA, QSA and OSS; there is no
 * TinyALSA backend upstream, and this board has none of the four.  It does have
 * TinyALSA - the only ALSA userland in its rootfs - so this file implements the
 * pcaudiolib backend interface on top of it.  espeak-ng is the caller that
 * matters here, but anything linked against libpcaudio gets the same sink.
 *
 * What the board's PCM will and will not accept
 * ---------------------------------------------
 * The I2S DAI (sound/soc/nsing/n32h7xx-i2s.c) advertises SNDRV_PCM_RATE_8000_48000
 * - the seven rates 8000/11025/16000/22050/32000/44100/48000 - and
 * SNDRV_PCM_FMTBIT_S16_LE, on the DAI, on the codec and in n32_pcm_hw.
 * n32_hw_params() then rejects anything that is not S16_LE and *exactly two
 * channels*, so:
 *
 *   - S16LE is the only format this backend accepts;
 *   - any rate outside the seven is refused up front, with a message naming the
 *     set, rather than being left to fail inside snd_pcm_hw_params();
 *   - the PCM is always opened with two channels, and a mono caller has each
 *     sample duplicated into left and right.
 *
 * There is no resampler.  espeak-ng synthesises at a fixed 22050 Hz and the
 * codec is the clock master on this link, so the rate is simply passed through;
 * 22050 is one of the seven.
 */
#include "config.h"
#include "audio_priv.h"

#ifdef HAVE_TINYALSA

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyalsa/pcm.h>

/* The DAI requires this many channels, whatever the caller asked for. */
#define OUT_CHANNELS	2
#define SAMPLE_BYTES	2			/* S16LE */

/*
 * 1024 frames is 4096 bytes of stereo S16LE, which is exactly
 * n32_pcm_hw.period_bytes_max, and it is the period size tinyplay uses on this
 * board.  Four periods give 16 KiB of buffer, a quarter of buffer_bytes_max.
 */
#define PERIOD_FRAMES	1024
#define PERIOD_COUNT	4

/*
 * Widening scratch: one period worth.  Kept as a fixed-size array inside the
 * object rather than malloc()ed, because this target has no MMU: there, every
 * malloc() is its own mapping which must be physically contiguous and is
 * rounded up to a power of two pages, so a buffer sized by a runtime argument
 * is exactly the allocation pattern that fails when memory is fragmented.
 * An array in the object is one allocation, of a size known at compile time.
 */
#define SCRATCH_FRAMES	PERIOD_FRAMES
#define SCRATCH_BYTES	(SCRATCH_FRAMES * OUT_CHANNELS * SAMPLE_BYTES)

/*
 * pcm_writei() reports the number of frames the kernel took, and 0 is a
 * legitimate answer (it means nothing was consumed).  Retrying forever on that
 * would be a spin, so give up after a few attempts and report the failure.
 */
#define MAX_STALLED_WRITES	8

/* Exactly the rates n32_pcm_hw lists; anything else the DAI will not clock. */
static const uint32_t pcm_rates[] = {
	8000, 11025, 16000, 22050, 32000, 44100, 48000
};

struct tinyalsa_object
{
	struct audio_object vtable;
	struct pcm *handle;
	char *device;
	/* in_frame and out_frame are bytes per frame, as in alsa.c's sample_size */
	uint8_t in_frame;
	uint8_t out_frame;
	/* saved audio_object_open parameters */
	int is_open;
	enum audio_object_format format;
	uint32_t rate;
	uint8_t channels;
	/*
	 * The message from a failed pcm_open() only exists in the handle
	 * tinyalsa is about to tear down, and the caller formats the error long
	 * after that, so a copy has to be kept.
	 */
	char last_error[128];
	unsigned char scratch[SCRATCH_BYTES];
};

#define to_tinyalsa_object(object) container_of(object, struct tinyalsa_object, vtable)

static int
tinyalsa_fail(struct tinyalsa_object *self, int error, const char *message)
{
	snprintf(self->last_error, sizeof self->last_error, "%s", message);
	return error;
}

/*
 * Push one run of stereo frames, looping until all of them are in.  pcm_writei()
 * returns the frame count actually written - not 0/-1 - so a short return is a
 * partial success and has to be advanced past, not treated as an error.
 * pcm_generic_transfer() already hides the interesting kernel errors from us: it
 * re-prepares after EPIPE/ESTRPIPE, and reports -1 only when that fails too.
 */
static int
tinyalsa_write_frames(struct tinyalsa_object *self, const void *data, size_t frames)
{
	const unsigned char *cursor = data;
	unsigned int stalled = 0;

	while (frames) {
		unsigned int chunk = frames < SCRATCH_FRAMES ?
			(unsigned int)frames : SCRATCH_FRAMES;
		int written = pcm_writei(self->handle, cursor, chunk);
		unsigned int done;

		if (written < 0) {
			const char *message = pcm_get_error(self->handle);

			return tinyalsa_fail(self, -EIO,
				message && message[0] ? message : strerror(errno));
		}
		if (written == 0) {
			if (++stalled >= MAX_STALLED_WRITES)
				return tinyalsa_fail(self, -EIO,
					"PCM accepted no frames");
			continue;
		}
		stalled = 0;
		done = (unsigned int)written;
		if (done > chunk)	/* cannot happen; do not run off the buffer */
			done = chunk;
		cursor += (size_t)done * self->out_frame;
		frames -= done;
	}
	return 0;
}

static int
tinyalsa_object_open(struct audio_object *object,
                     enum audio_object_format format,
                     uint32_t rate,
                     uint8_t channels)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);
	struct pcm_config config;
	const char *message;
	unsigned int i;

	if (self->handle)
		return -EEXIST;

	self->last_error[0] = '\0';

	if (format != AUDIO_OBJECT_FORMAT_S16LE)
		return tinyalsa_fail(self, -EINVAL,
			"the I2S DAI on this board takes S16LE only");
	if (channels != 1 && channels != 2)
		return tinyalsa_fail(self, -EINVAL,
			"the I2S DAI takes exactly two channels; 1 or 2 accepted here");
	for (i = 0; i < sizeof pcm_rates / sizeof pcm_rates[0]; i++)
		if (pcm_rates[i] == rate)
			break;
	if (i == sizeof pcm_rates / sizeof pcm_rates[0])
		return tinyalsa_fail(self, -EINVAL,
			"rate must be one of 8000 11025 16000 22050 32000 44100 48000");

	memset(&config, 0, sizeof config);
	config.channels = OUT_CHANNELS;
	config.rate = rate;
	config.period_size = PERIOD_FRAMES;
	config.period_count = PERIOD_COUNT;
	config.format = PCM_FORMAT_S16_LE;
	/*
	 * Start as soon as one period is queued so playback begins promptly, but
	 * let four periods accumulate before declaring an underrun - the
	 * widening copy is done on the CPU and a shallower stop threshold would
	 * turn a slow write into an XRUN.
	 */
	config.start_threshold = PERIOD_FRAMES;
	config.stop_threshold = PERIOD_FRAMES * PERIOD_COUNT;
	config.silence_threshold = PERIOD_FRAMES * PERIOD_COUNT;
	config.silence_size = 0;

	/*
	 * No device name is the normal case: espeak-ng calls
	 * create_audio_device_object() with NULL, and card 0 device 0 is the
	 * only PCM on the board.  A name, if one is given, is tinyalsa's
	 * "hw:card,device" form.
	 */
	if (self->device && self->device[0])
		self->handle = pcm_open_by_name(self->device, PCM_OUT, &config);
	else
		self->handle = pcm_open(0, 0, PCM_OUT, &config);

	if (!self->handle)
		return tinyalsa_fail(self, -ENOMEM, "out of memory");
	if (!pcm_is_ready(self->handle)) {
		message = pcm_get_error(self->handle);
		snprintf(self->last_error, sizeof self->last_error, "%s",
			 message && message[0] ? message : "cannot open PCM device");
		/* pcm_open() hands back a static not-ready object on failure and
		 * pcm_close() recognises it, so this only releases the real thing. */
		pcm_close(self->handle);
		self->handle = NULL;
		return -EIO;
	}

	self->is_open = 1;
	self->format = format;
	self->rate = rate;
	self->channels = channels;
	self->in_frame = SAMPLE_BYTES * channels;
	self->out_frame = SAMPLE_BYTES * OUT_CHANNELS;
	return 0;
}

void
tinyalsa_object_close(struct audio_object *object)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);

	if (self->handle) {
		pcm_close(self->handle);
		self->handle = NULL;
		/* Unlike alsa.c, which leaves this at 1 and makes a later
		 * flush() reopen a closed device, a closed object is closed. */
		self->is_open = 0;
	}
}

void
tinyalsa_object_destroy(struct audio_object *object)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);

	free(self->device);
	free(self);
}

int
tinyalsa_object_drain(struct audio_object *object)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);
	int ret = 0;

	if (self->handle) {
		pcm_drain(self->handle);
		/* DRAIN leaves the stream in SETUP, so re-arm it for whatever
		 * gets written next - the same pairing alsa.c makes with
		 * snd_pcm_prepare(). */
		ret = pcm_prepare(self->handle);
	}
	return ret;
}

int
tinyalsa_object_flush(struct audio_object *object)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);

	if (!self->handle || !self->is_open)
		return 0;

	/*
	 * alsa.c has to close and reopen here because snd_pcm_drop() does not
	 * discard queued audio on ALSA.  TinyALSA's pcm_stop() is
	 * SNDRV_PCM_IOCTL_DROP, which throws away everything still queued and
	 * returns the stream to SETUP; the next pcm_writei() prepares it again.
	 * That is what cancel_audio() is asking for, and it avoids re-opening a
	 * device node that is exclusive to this process.
	 */
	return pcm_stop(self->handle) < 0 ? -EIO : 0;
}

int
tinyalsa_object_write(struct audio_object *object,
                      const void *data,
                      size_t bytes)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);
	const unsigned char *cursor = data;
	size_t frames;

	if (!self->handle)
		return 0;

	frames = bytes / self->in_frame;
	if (!frames)
		return 0;

	if (self->channels == OUT_CHANNELS)
		return tinyalsa_write_frames(self, cursor, frames);

	/*
	 * Mono source, stereo PCM: duplicate each sample into both channels.
	 * The copy is bytewise because the data is little-endian S16 and nothing
	 * here may assume the buffer is aligned to 2 bytes.
	 */
	while (frames) {
		unsigned int chunk = frames < SCRATCH_FRAMES ?
			(unsigned int)frames : SCRATCH_FRAMES;
		unsigned int i;
		int err;

		for (i = 0; i < chunk; i++) {
			self->scratch[i * 4 + 0] = cursor[i * 2 + 0];
			self->scratch[i * 4 + 1] = cursor[i * 2 + 1];
			self->scratch[i * 4 + 2] = cursor[i * 2 + 0];
			self->scratch[i * 4 + 3] = cursor[i * 2 + 1];
		}
		err = tinyalsa_write_frames(self, self->scratch, chunk);
		if (err)
			return err;
		cursor += (size_t)chunk * self->in_frame;
		frames -= chunk;
	}
	return 0;
}

const char *
tinyalsa_object_strerror(struct audio_object *object,
                         int error)
{
	struct tinyalsa_object *self = to_tinyalsa_object(object);

	/* Whatever tinyalsa said is more use than a generic errno string. */
	if (self->last_error[0])
		return self->last_error;

	return strerror(error < 0 ? -error : error);
}

struct audio_object *
create_tinyalsa_object(const char *device,
                       const char *application_name,
                       const char *description)
{
	struct tinyalsa_object *self = malloc(sizeof(struct tinyalsa_object));
	if (!self)
		return NULL;

	self->handle = NULL;
	self->in_frame = 0;
	self->out_frame = 0;
	self->device = device ? strdup(device) : NULL;
	self->is_open = 0;
	self->last_error[0] = '\0';

	(void)application_name;
	(void)description;

	self->vtable.open = tinyalsa_object_open;
	self->vtable.close = tinyalsa_object_close;
	self->vtable.destroy = tinyalsa_object_destroy;
	self->vtable.write = tinyalsa_object_write;
	self->vtable.drain = tinyalsa_object_drain;
	self->vtable.flush = tinyalsa_object_flush;
	self->vtable.strerror = tinyalsa_object_strerror;

	return &self->vtable;
}

#else

/*
 * Same shape as the four vendor backends: with the backend compiled out, the
 * entry point still exists and reports that it can offer nothing.
 */
struct audio_object *
create_tinyalsa_object(const char *device,
                       const char *application_name,
                       const char *description)
{
	(void)device;
	(void)application_name;
	(void)description;
	return NULL;
}

#endif

