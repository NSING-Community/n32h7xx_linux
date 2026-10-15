/* Build configuration for the pcaudiolib cross build.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Upstream generates this file with autoconf, which probes the build host for
 * ALSA, PulseAudio, QSA and OSS headers.  Every one of those probes must stay
 * undefined here, and that is the whole point of the file: each vendor backend
 * is written as `#ifdef HAVE_<backend> ... #else return NULL; #endif`, so with
 * nothing defined they all compile down to their stub arms and
 * create_audio_device_object() reaches the TinyALSA backend this port adds.
 *
 * Nothing else from upstream's config.h is needed - the sources included here
 * do not use the feature macros autoconf also emits.
 */

/*
 * ...except this one, which is not an autoconf probe but the switch the
 * TinyALSA backend is written against.  It is spelled HAVE_TINYALSA and used
 * the way the vendor backends use their own guards, so the upstream delta
 * stays in their idiom: audio.c calls create_tinyalsa_object() next to the
 * four existing calls, tinyalsa.c compiles to a NULL-returning stub when the
 * macro is absent, and a build without TinyALSA still links.
 */
#define HAVE_TINYALSA 1
