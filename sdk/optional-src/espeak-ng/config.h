/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Hand-written replacement for the config.h that upstream's CMake build
 * generates from src/libespeak-ng/config.h.in.
 *
 * Upstream configures this file by probing the build machine.  A cross build
 * cannot do that (and this workspace has no cmake), so the values are pinned
 * here for the two configurations the port actually uses: an x86-64 glibc
 * host build whose only job is to compile the voice data, and the ARMv7-M
 * uClibc FDPIC target build.
 *
 * Keep the "#define X 0"/"#define X 1" form.  Upstream uses #cmakedefine01 and
 * the sources test these with #if, never #ifdef, so an undefined symbol is a
 * compile error rather than a silently disabled feature.
 */

/* Host and target both have mkstemp; neither glibc nor uClibc-ng has
 * sys/endian.h (that is a BSD/musl header) or the BSD optreset extension. */
#define HAVE_MKSTEMP 1
#define HAVE_NBTOOL_CONFIG_H 0
#define HAVE_DECL_OPTRESET 0
#define HAVE_SYS_ENDIAN_H 0

/* iswblank() on the host comes from glibc; on the target it comes from the
 * port's compat layer (see compat/), so it is available in both. */
#define HAVE_ISWBLANK 1

/* The asynchronous API needs an event thread.  The board has pthreads, but
 * nothing in this port needs to talk to the library while it is speaking, and
 * leaving it out drops event.c/fifo.c/espeak_command.c and the thread state. */
#define USE_ASYNC 0

/* Formant synthesis path used by voices that ask for "klatt".  None of the
 * voices shipped in the trimmed data set use it, but it is cheap to keep and
 * removing it changes behaviour of any voice that does. */
#define USE_KLATT 1

/* USE_LIBPCAUDIO is the one that is on, for the target only.
 *
 * pcaudiolib is espeak-ng's only real audio sink (libsonic is time stretching,
 * mbrola a different synth, sPlayer an external player).  Without it
 * "just speak" is a silent success: dispatch_audio()'s SPEAK_AUDIO arm is
 * entirely inside #if USE_LIBPCAUDIO, so the CLI returns 0 having made no
 * sound at all.  The port ships a pcaudiolib with a TinyALSA backend (see
 * sdk-src/project/pcaudiolib/), and the cross build links it.
 *
 * The host build must NOT set it: that build exists only to run
 * --compile-phonemes/--compile-intonations/--compile=<lang> and has no audio
 * device or library anywhere near it, so ESPEAK_NG_TARGET is what selects
 * this - the cross recipe passes -DESPEAK_NG_TARGET, the native one does not.
 */
#if defined(ESPEAK_NG_TARGET) && ESPEAK_NG_TARGET
#define USE_LIBPCAUDIO 1
#else
#define USE_LIBPCAUDIO 0
#endif

/* The remaining three off: none is needed to synthesise speech, none of the
 * libraries is cross-built here, and mbrola would drag in fork()/execlp()
 * which a no-MMU process cannot use for this purpose. */
#define USE_LIBSONIC 0
#define USE_MBROLA 0
#define USE_SPEECHPLAYER 0

#define PACKAGE_VERSION "1.53.0"
