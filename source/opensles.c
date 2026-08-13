/* opensles.c -- minimal OpenSL ES shim backed by SDL2 audio
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 the SQEX "Sd" sound driver uses:
 * the Object interface (Realize/GetInterface/Destroy), the Engine interface
 * (CreateOutputMix/CreateAudioPlayer), and on each player the Play, Volume and
 * AndroidSimpleBufferQueue interfaces. Players are software-mixed into one SDL2
 * audio device; the buffer-queue completion callback is fired from the SDL
 * audio thread, exactly like Android's fast-track callback.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>   /* snprintf for phi_audio_stats */
#include <math.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "opensles.h"
#include "util.h"

// --- OpenSL ES constants ----------------------------------------------------

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

// callback: (SLAndroidSimpleBufferQueueItf caller, void *pContext)
typedef void (*slBufferQueueCallback)(void *caller, void *context);

// --- interface-id sentinels -------------------------------------------------

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

// --- vtable structs (method order matches the OpenSL ES 1.0.1 spec) ---------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

// only CreateAudioPlayer (slot 2) and CreateOutputMix (slot 7) are used; the
// rest keep the correct layout but are generic so a shared stub assigns
// cleanly. The engine calls each slot with its own typed vtable.
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

// SLPlaybackRateItf (Chaos Rings 3 requests it for pitch-shifted SE). We don't
// resample, so SetRate is accepted but ignored; the spec method order matters.
typedef struct {
  SLresult (*SetRate)(void *self, SLint16 rate);
  SLresult (*GetRate)(void *self, SLint16 *p);
  SLresult (*SetPropertyConstraints)(void *self, SLuint32 c);
  SLresult (*GetProperties)(void *self, SLuint32 *p);
  SLresult (*GetCapabilitiesOfRate)(void *self, SLuint32 *p);
  SLresult (*GetRateRange)(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop);
} SLPlaybackRateItf_;

// SLAndroidConfigurationItf -- CR3 requests it with req=SL_BOOLEAN_TRUE at
// CreateAudioPlayer, so it MUST be obtainable or the engine aborts the player
// (the symptom: a player is created but never set PLAYING / never enqueues).
// It only uses SetConfiguration (stream type / usage), which we can ignore.
typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value, SLuint32 valueSize);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *pValueSize, void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 proxyType, void *pProxyObj);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 proxyType);
} SLAndroidConfigurationItf_;

// --- objects ----------------------------------------------------------------

#define MAX_PLAYERS 64
// FMOD's OpenSL output enqueues N = (dspNumBuffers*dspBufferLength)/framesPerBuffer
// buffers up front (see libunity +0xce697c) before the first callback drains any.
// If the queue fills, bq_Enqueue returns an error and FMOD aborts the player with
// its generic "internal" error (33). With a 256-frame period and typical DSP
// buffer totals N is ~8-32, but keep the ring comfortably larger so a big baked
// dspBufferLength can't overflow it.
#define BQ_SLOTS 256

typedef struct {
  const void *data;   // -> own; never the producer's memory (see bq_Enqueue)
  SLuint32 size;
  void    *own;       // shim-owned copy of the enqueued block
  SLuint32 own_cap;   // capacity of own
} BQBuffer;

typedef struct Player {
  void    *cur_own;      // mixer-owned copy of the block being played
  SLuint32 cur_own_cap;  // capacity of cur_own
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;
  const SLPlaybackRateItf_ *rate_vt;
  const SLAndroidConfigurationItf_ *config_vt;

  int in_use;
  int channels;
  int rate;
  int sbytes;    // bytes per sample in the enqueued buffers (2=16-bit, 4=32-bit)
  int is_float;  // 1 if samples are 32-bit float, 0 if signed integer
  int playing;
  int drained;   // consecutive callbacks this playing player produced no audio
  float gain; // linear, from SetVolumeLevel (millibels)

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  // currently draining buffer
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;
  double cur_fpos; // fractional sample index into cur (for rate conversion)

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

// --- global SDL device + player registry ------------------------------------

static SDL_AudioDeviceID g_dev = 0;
static int g_dev_rate = 44100;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

#define MOVIE_RING_FRAMES 65536
static SDL_mutex *g_movie_lock = NULL;
static int16_t *g_movie_pcm = NULL;
static int g_movie_active = 0;
static int g_movie_paused = 0;
static int g_movie_head = 0;
static int g_movie_count = 0;
static uint64_t g_movie_samples_queued = 0;
static uint64_t g_movie_samples_played = 0;

/* --- diagnostic counters --------------------------------------------------
 * The audio callback must not do file I/O -- logging from an audio thread is
 * what stopped CloverPit's console booting, and the same applies here. So the
 * audio path only bumps plain counters and phi_audio_stats() formats them for
 * the MAIN thread to print. Enough to separate the two candidate causes of bad
 * audio without another guess:
 *
 *   consumed > enq   -> the mixer is replaying blocks (aliasing/duplication)
 *   dry high         -> the queue empties mid-callback (underrun)
 *   short high       -> callbacks ending early, i.e. partial silence (clicks)
 */
static volatile uint32_t g_st_enq, g_st_consumed, g_st_dry, g_st_short, g_st_cb;

void phi_audio_stats(char *out, size_t cap) {
  snprintf(out, cap, "enq=%u consumed=%u cb=%u dry=%u short=%u",
           g_st_enq, g_st_consumed, g_st_cb, g_st_dry, g_st_short);
}

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// Read one sample (at sample-index k within the buffer) and return it scaled to
// signed-16-bit range, regardless of the source format. The shim's accumulator
// and SDL device are S16; FMOD's master output player can be 16-bit int, 32-bit
// int, or 32-bit float, so normalise here.
static inline int32_t read_sample_s16(const void *buf, long k, int sbytes, int is_float) {
  if (is_float) {
    float f = ((const float *)buf)[k];
    if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
    return (int32_t)(f * 32767.0f);
  }
  if (sbytes == 4)
    return ((const int32_t *)buf)[k] >> 16;   // S32 -> S16 range
  return (int32_t)((const int16_t *)buf)[k];  // S16
}

// mix one playing player into the S16 stereo accumulator (int32 to avoid clip).
// cur/cur_pos are touched only by this (audio) thread, so they need no lock;
// only the buffer queue is shared with Enqueue. Critically, the engine's
// completion callback is fired WITHOUT our lock held -- Android's contract --
// otherwise the engine's mixer thread (holding its own mutex, calling Enqueue
// which wants our lock) deadlocks against us.
static void mix_player(Player *p, int32_t *acc, int frames) {
  if (!p->playing)
    return;

  // A playing player with nothing queued is a finished one-shot SE the engine
  // fired and never Destroy'd; count the dry callbacks so alloc can recycle it.
  SDL_LockMutex(p->lock);
  const int dry = (!p->cur) && (p->q_head == p->q_tail);
  SDL_UnlockMutex(p->lock);
  if (dry) {
    /* Counted so "dry=" in the per-frame [audio] line means something. Both this
     * tree and CloverPit declared g_st_dry and never incremented it, so it read
     * 0 forever -- a counter that always reports healthy is worse than no
     * counter, because it gets used as evidence. A LIVE stream going dry is
     * starvation; a finished one-shot going dry is normal, so only count while
     * the player has been dry briefly. */
    if (p->drained < 4) g_st_dry++;
    if (p->drained < (1 << 20)) p->drained++;
    return;
  }
  p->drained = 0;

  const float g = p->gain;
  const int stereo = (p->channels >= 2);
  const int sbytes = p->sbytes > 0 ? p->sbytes : 2;    // bytes per sample
  const int is_float = p->is_float;
  const int bps = stereo ? sbytes * 2 : sbytes;        // bytes per input frame
  // resample the player's own rate to the device rate (players come in at 22050
  // AND 44100; without this, off-rate voices play at the wrong speed/pitch).
  const double ratio = g_dev_rate > 0 ? (double)p->rate / (double)g_dev_rate : 1.0;

  for (int i = 0; i < frames; i++) {
    // ensure cur holds a buffer whose integer sample index covers cur_fpos,
    // carrying the fractional remainder across buffer boundaries.
    for (;;) {
      if (!p->cur) {
        /* Copy the block into a buffer the MIXER owns, while holding the lock.
         *
         * Taking a pointer into the queue slot is not safe, even though the slot
         * was just released. Two producer actions can reach that same memory
         * while this thread is still playing out of it:
         *
         *   - bq_Clear() resets q_tail to 0, so the very next Enqueue writes
         *     slot 0 and memcpy's over it. Exceed7 NativeAudio calls Clear on
         *     every hitsound stop/replay, so this fires constantly.
         *   - Enqueue reallocs a slot's buffer when a bigger block arrives,
         *     which MOVES it and frees the old pointer outright.
         *
         * Either one restarts or garbles the sound mid-playback -- the exact
         * "sound effects repeating" symptom. Copying into the producer's memory
         * was the original bug; pointing at our own slot memory just moved the
         * race one level down. Owning the in-flight block ends it: once this
         * memcpy returns, nothing the producer does can touch what is playing.
         *
         * One memcpy per BLOCK (not per sample), and cur_own settles at the
         * steady-state block size after the first few, so this does not
         * allocate on the audio thread in normal running. */
        SDL_LockMutex(p->lock);
        const int have = (p->q_head != p->q_tail);
        SLuint32 got = 0;
        if (have) {
          BQBuffer *b = &p->q[p->q_head];
          if (p->cur_own_cap < b->size) {
            void *nb = realloc(p->cur_own, b->size);
            if (nb) { p->cur_own = nb; p->cur_own_cap = b->size; }
          }
          if (p->cur_own && p->cur_own_cap >= b->size) {
            if (b->data && b->size) memcpy(p->cur_own, b->data, b->size);
            got = b->size;
          }
          p->q_head = (p->q_head + 1) % BQ_SLOTS;
        }
        SDL_UnlockMutex(p->lock);
        if (!have || !got) {
          g_st_short++;
          return; // underrun (or the copy could not be made): stay silent
        }
        p->cur = p->cur_own;
        p->cur_size = got;
        g_st_consumed++;
      }
      const long n = (long)(p->cur_size / (SLuint32)bps);
      if (n > 0 && (long)p->cur_fpos < n)
        break; // position is inside the current buffer
      // buffer consumed (or empty): carry remainder, notify engine, fetch next
      p->cur_fpos -= (double)n;
      if (p->cur_fpos < 0.0) p->cur_fpos = 0.0;
      p->cur = NULL;
      if (p->cb) g_st_cb++;
      if (p->cb) {
        static int once = 0;
        if (!once) { once = 1; debugPrintf("[fmod] OpenSL buffer-queue callback firing -> FMOD mixer is producing PCM\n"); }
        p->cb(&p->bq_vt, p->cb_ctx);
      }
    }

    const long n = (long)(p->cur_size / (SLuint32)bps);
    const long idx = (long)p->cur_fpos;
    const double frac = p->cur_fpos - (double)idx;
    const void *s = p->cur;
    int32_t l, r;
    if (stereo) {
      const long j0 = idx * 2, j1 = (idx + 1 < n ? idx + 1 : idx) * 2;
      const int32_t l0 = read_sample_s16(s, j0,     sbytes, is_float);
      const int32_t l1 = read_sample_s16(s, j1,     sbytes, is_float);
      const int32_t r0 = read_sample_s16(s, j0 + 1, sbytes, is_float);
      const int32_t r1 = read_sample_s16(s, j1 + 1, sbytes, is_float);
      l = (int32_t)(l0 * (1.0 - frac) + l1 * frac);
      r = (int32_t)(r0 * (1.0 - frac) + r1 * frac);
    } else {
      const int32_t a  = read_sample_s16(s, idx, sbytes, is_float);
      const int32_t b2 = (idx + 1 < n) ? read_sample_s16(s, idx + 1, sbytes, is_float) : a;
      l = r = (int32_t)(a * (1.0 - frac) + b2 * frac);
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
    p->cur_fpos += ratio;
  }
}

static void mix_movie(int32_t *acc, int frames) {
  if (!g_movie_lock)
    return;

  SDL_LockMutex(g_movie_lock);
  if (!g_movie_active || g_movie_paused || !g_movie_pcm) {
    SDL_UnlockMutex(g_movie_lock);
    return;
  }

  const int n = g_movie_count < frames ? g_movie_count : frames;
  for (int i = 0; i < n; i++) {
    const int idx = (g_movie_head + i) % MOVIE_RING_FRAMES;
    acc[i * 2 + 0] += g_movie_pcm[idx * 2 + 0];
    acc[i * 2 + 1] += g_movie_pcm[idx * 2 + 1];
  }
  g_movie_head = (g_movie_head + n) % MOVIE_RING_FRAMES;
  g_movie_count -= n;
  g_movie_samples_played += (uint64_t)n;
  SDL_UnlockMutex(g_movie_lock);
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;

  // The engine's completion callback (fired from here via mix_player) reads its
  // stack-guard from tpidr_el0+0x28; SDL's audio thread never set that up. Give
  // this thread its OWN bionic TLS block (single audio thread, so static is fine
  // and persists for its lifetime).
  static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  static int tls_ready = 0;
  if (!tls_ready) { install_bionic_tls(audio_tls); tls_ready = 1; }

  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] && g_players[i]->in_use)
      mix_player(g_players[i], acc, frames);
  SDL_UnlockMutex(g_reg_lock);

  mix_movie(acc, frames);

  int16_t *out = (int16_t *)stream;
  int32_t peak = 0;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
    int32_t a = v < 0 ? -v : v;
    if (a > peak) peak = a;
  }
  if (peak > 64) {
    static int once = 0;
    if (!once) { once = 1; debugPrintf("[fmod] OpenSL: first non-silent audio to device (peak=%d) -- SOUND IS ON\n", peak); }
  }
}

static void ensure_device(int rate) {
  (void)rate; // players run at mixed rates (22050/44100); open at the Switch's
              // native 48000 and resample each player in mix_player instead of
              // letting the first player pin the device rate.
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (g_dev)
    return;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    debugPrintf("opensles: SDL audio init failed: %s\n", SDL_GetError());
    return;
  }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = PHI_AUDIO_DEVICE_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = PHI_AUDIO_CALLBACK_FRAMES;
  want.callback = audio_callback;
  /* ALLOW_FREQUENCY_CHANGE: if the backend cannot do the requested rate it
   * substitutes its own instead of failing the open, and g_dev_rate follows so
   * mix_player's resampler stays correct. Degrades to resampling, never to
   * silence. (CloverPit needed this because its FMOD ran at 24000; ours is
   * already 48000 end-to-end, so it should be a no-op here -- but a no-op that
   * costs nothing and removes a whole failure mode is worth having.) */
  g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                              SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!g_dev) {
    debugPrintf("opensles: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return;
  }
  g_dev_rate = have.freq;
  debugPrintf("[fmod] SDL device: asked %d Hz / %d frames, got %d Hz / %d frames, %d ch%s\n",
              PHI_AUDIO_DEVICE_RATE, PHI_AUDIO_CALLBACK_FRAMES,
              have.freq, have.samples, have.channels,
              have.freq == PHI_AUDIO_DEVICE_RATE ? " (no resampling)"
                                                 : " (mix_player will resample)");
  SDL_PauseAudioDevice(g_dev, 0);
}

int opensles_movie_begin(int requested_rate) {
  if (!g_movie_lock)
    g_movie_lock = SDL_CreateMutex();
  if (!g_movie_pcm)
    g_movie_pcm = calloc(MOVIE_RING_FRAMES * 2, sizeof(int16_t));
  if (!g_movie_lock || !g_movie_pcm)
    return 0;

  ensure_device(requested_rate > 0 ? requested_rate : 44100);
  if (!g_dev)
    return 0;

  SDL_LockMutex(g_movie_lock);
  g_movie_active = 1;
  g_movie_paused = 1;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_samples_queued = 0;
  g_movie_samples_played = 0;
  SDL_UnlockMutex(g_movie_lock);
  return g_dev_rate;
}

int opensles_movie_queue(const int16_t *pcm, int frames) {
  int done = 0;
  while (done < frames) {
    if (!g_movie_lock)
      return done;

    SDL_LockMutex(g_movie_lock);
    if (!g_movie_active || !g_movie_pcm) {
      SDL_UnlockMutex(g_movie_lock);
      return done;
    }

    const int space = MOVIE_RING_FRAMES - g_movie_count;
    int n = frames - done;
    if (n > space)
      n = space;
    for (int i = 0; i < n; i++) {
      const int idx = (g_movie_head + g_movie_count + i) % MOVIE_RING_FRAMES;
      g_movie_pcm[idx * 2 + 0] = pcm[(done + i) * 2 + 0];
      g_movie_pcm[idx * 2 + 1] = pcm[(done + i) * 2 + 1];
    }
    g_movie_count += n;
    g_movie_samples_queued += (uint64_t)n;
    SDL_UnlockMutex(g_movie_lock);

    done += n;
    if (done < frames)
      SDL_Delay(2);
  }
  return done;
}

void opensles_movie_set_paused(int paused) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  if (g_movie_active)
    g_movie_paused = paused != 0;
  SDL_UnlockMutex(g_movie_lock);
}

uint64_t opensles_movie_samples_queued(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_queued;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

uint64_t opensles_movie_samples_played(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_played;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

int opensles_movie_buffered_frames(void) {
  int ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_count;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

void opensles_movie_end(void) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  g_movie_active = 0;
  g_movie_paused = 0;
  g_movie_head = 0;
  g_movie_count = 0;
  SDL_UnlockMutex(g_movie_lock);
}

// --- FMOD (Unity native audio) output sink ----------------------------------
// Unity's Android audio backend is FMOD's AudioTrack driver. Its Java run()
// loop never executes here (no JVM), so jni_fake drives fmodProcess on a native
// thread and pushes the drained PCM into this dedicated, queue-mode SDL device.
// Kept separate from g_dev (the OpenSL software mixer, which a Unity title never
// opens) so the two paths can't interfere.

static SDL_AudioDeviceID g_fmod_dev = 0;
static int g_fmod_dev_rate = 0;

int audio_fmod_open(int rate, int channels) {
  if (g_fmod_dev)
    return g_fmod_dev_rate;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    debugPrintf("[fmod] SDL audio init failed: %s\n", SDL_GetError());
    return 0;
  }
  if (rate <= 0)     rate = 48000;
  if (channels <= 0) channels = 2;
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq     = rate;
  want.format   = AUDIO_S16SYS;
  want.channels = (Uint8)channels;
  want.samples  = 1024;
  want.callback = NULL; // queue mode: we push via SDL_QueueAudio
  g_fmod_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!g_fmod_dev) {
    debugPrintf("[fmod] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    return 0;
  }
  g_fmod_dev_rate = have.freq;
  debugPrintf("[fmod] output device opened: %d Hz, %d ch (asked %d Hz)\n",
              have.freq, have.channels, rate);
  SDL_PauseAudioDevice(g_fmod_dev, 0);
  return g_fmod_dev_rate;
}

// Append PCM (S16, interleaved) to the device queue. Returns currently queued
// bytes after the append, so the caller can pace itself to realtime.
uint32_t audio_fmod_write(const void *pcm, int bytes) {
  if (!g_fmod_dev || bytes <= 0)
    return g_fmod_dev ? SDL_GetQueuedAudioSize(g_fmod_dev) : 0;
  SDL_QueueAudio(g_fmod_dev, pcm, (Uint32)bytes);
  return SDL_GetQueuedAudioSize(g_fmod_dev);
}

uint32_t audio_fmod_queued(void) {
  return g_fmod_dev ? SDL_GetQueuedAudioSize(g_fmod_dev) : 0;
}

// --- buffer queue interface -------------------------------------------------

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PARAMETER_INVALID;
  }
  /* COPY. Do not keep the caller's pointer.
   *
   * OpenSL's BufferQueue contract lets the producer reuse its buffer as soon as
   * Enqueue returns -- it only has to keep the memory valid until the callback
   * says the block was consumed. Both producers here do exactly that: FMOD
   * cycles a small set of mix buffers, and Exceed7 NativeAudio re-fills one
   * staging block per voice. Storing the pointer meant the software mixer read
   * whatever had landed there by the time it got around to that slot, so the
   * same block was replayed or half-overwritten -- audio that stutters and
   * repeats while every counter says the queue is healthy.
   *
   * The buffer is grown once per slot and reused, so steady-state playback does
   * no allocation at all; only the memcpy is per-block. Same fix CloverPit
   * shipped for the identical symptom. */
  BQBuffer *b = &p->q[p->q_tail];
  if (b->own_cap < size) {
    void *nb = realloc(b->own, size);
    if (!nb) {                       // out of memory: drop rather than alias
      SDL_UnlockMutex(p->lock);
      return SL_RESULT_PARAMETER_INVALID;
    }
    b->own = nb;
    b->own_cap = size;
  }
  if (pBuffer && size)
    memcpy(b->own, pBuffer, size);
  b->data = b->own;
  b->size = size;
  p->q_tail = next;
  g_st_enq++;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

/* Every path that retires a Player must run this or it leaks one block per
 * slot, forever -- and eng_CreateAudioPlayer recycles players continuously. */
static void bq_free_slots(Player *p) {
  free(p->cur_own);
  p->cur_own = NULL;
  p->cur_own_cap = 0;
  p->cur = NULL;             /* pointed into cur_own */
  for (int i = 0; i < BQ_SLOTS; i++) {
    free(p->q[i].own);
    p->q[i].own = NULL;
    p->q[i].own_cap = 0;
    p->q[i].data = NULL;
    p->q[i].size = 0;
  }
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  /* Queue indices only. cur/cur_pos/cur_size/cur_fpos describe the block the
   * MIXER is playing out of, and the mixer reads them without the lock -- so
   * clearing them from the producer thread is a race: null p->cur between the
   * mixer's `if (!p->cur)` test and its next use and it dereferences NULL.
   *
   * Leaving them alone makes those fields mixer-private, which removes the race
   * entirely rather than narrowing it. The cost is that a cleared player
   * finishes the block already in flight -- a few milliseconds -- before it
   * finds the queue empty and goes quiet. SetPlayState(STOPPED) still stops it
   * immediately, so an actual stop is unaffected; this only concerns Clear.
   *
   * The block itself is safe to keep playing: since the mixer copies into its
   * own cur_own buffer, it does not point into the queue any more. */
  p->q_head = p->q_tail = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS + (p->cur ? 1 : 0);
    st->index = 0;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  p->cb = cb;
  p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// --- play interface ---------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  SDL_LockMutex(p->lock);
  p->playing = (state == SL_PLAYSTATE_PLAYING);
  SDL_UnlockMutex(p->lock);
  if (p->playing) {
    static int once = 0;
    if (!once) { once = 1; debugPrintf("[fmod] OpenSL SetPlayState(PLAYING) -> output running\n"); }
  }
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pState) *pState = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

// --- volume interface -------------------------------------------------------

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  // Clamp to the valid OpenSL volume range [-9600, 0] mB before converting. CR3
  // sometimes passes a garbage/zero-extended value (e.g. 32768) that would yield
  // an astronomically huge gain; treat out-of-range high as 0 mB (unity/full) so
  // the single unified-mix player is never accidentally muted or overflowed.
  int mb = (int)level;
  if (mb > 0) mb = 0;
  if (mb < -9600) mb = -9600;
  p->gain = mb_to_linear(mb);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (m) p->gain = 0.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

// --- playback rate interface (accepted but not resampled) -------------------

static SLresult rate_SetRate(void *self, SLint16 r) { (void)self; (void)r; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRate(void *self, SLint16 *p) { (void)self; if (p) *p = 1000; return SL_RESULT_SUCCESS; }
static SLresult rate_SetProps(void *self, SLuint32 c) { (void)self; (void)c; return SL_RESULT_SUCCESS; }
static SLresult rate_GetProps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetCaps(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult rate_GetRange(void *self, SLuint8 i, SLint16 *min, SLint16 *max, SLint16 *step, SLuint32 *prop) {
  (void)self; (void)i;
  if (min) *min = 500; if (max) *max = 2000; if (step) *step = 1; if (prop) *prop = 0;
  return SL_RESULT_SUCCESS;
}
static const SLPlaybackRateItf_ rate_vtable = {
  rate_SetRate, rate_GetRate, rate_SetProps, rate_GetProps, rate_GetCaps, rate_GetRange,
};

// --- android configuration interface (accepted, ignored) --------------------

static SLresult cfg_SetConfiguration(void *self, const void *key, const void *value, SLuint32 sz) {
  (void)self; (void)key; (void)value; (void)sz; return SL_RESULT_SUCCESS;
}
static SLresult cfg_GetConfiguration(void *self, const void *key, SLuint32 *psz, void *value) {
  (void)self; (void)key; (void)value; if (psz) *psz = 0; return SL_RESULT_SUCCESS;
}
static SLresult cfg_AcquireJavaProxy(void *self, SLuint32 t, void *p) {
  (void)self; (void)t; if (p) *(void **)p = NULL; return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult cfg_ReleaseJavaProxy(void *self, SLuint32 t) { (void)self; (void)t; return SL_RESULT_SUCCESS; }

static const SLAndroidConfigurationItf_ cfg_vtable = {
  cfg_SetConfiguration, cfg_GetConfiguration, cfg_AcquireJavaProxy, cfg_ReleaseJavaProxy,
};

// --- player object ----------------------------------------------------------

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else if (iid == SL_IID_PLAYBACKRATE) {
    *(void **)pInterface = &p->rate_vt;
  } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)pInterface = &p->config_vt;
  } else {
    *(void **)pInterface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  if (p->lock) SDL_DestroyMutex(p->lock);
  bq_free_slots(p);
  free(p);
}

// --- engine interface -------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(*p));
  if (!p)
    return SL_RESULT_PARAMETER_INVALID;
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->rate_vt = &rate_vtable;
  p->config_vt = &cfg_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->channels = 2;
  p->rate = 44100;
  p->sbytes = 2;     // assume 16-bit signed PCM unless the format says otherwise
  p->is_float = 0;
  p->lock = SDL_CreateMutex();

  if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    // formatType 2 = SL_DATAFORMAT_PCM (integer); 4 = SL_ANDROID_DATAFORMAT_PCM_EX
    // (adds a trailing 'representation' field: 1=signed int, 2=float, 3=unsigned).
    if (fmt->formatType == 2 || fmt->formatType == 4) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
      // stride is the container size (bits) when given, else the sample width.
      uint32_t stride_bits = fmt->containerSize ? fmt->containerSize : fmt->bitsPerSample;
      p->sbytes = stride_bits >= 32 ? 4 : 2;
      if (fmt->formatType == 4) {
        uint32_t representation = ((const uint32_t *)fmt)[7]; // field after endianness
        p->is_float = (representation == 2);
      }
    }
    debugPrintf("[fmod] OpenSL fmt: type=%u ch=%u rate=%uHz bits=%u container=%u -> %d-byte %s\n",
                fmt->formatType, fmt->numChannels, p->rate, fmt->bitsPerSample,
                fmt->containerSize, p->sbytes, p->is_float ? "float" : "int");
  }
  debugPrintf("[fmod] OpenSL CreateAudioPlayer: %d Hz, %d ch (FMOD output player)\n",
              p->rate, p->channels);

  ensure_device(p->rate);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot < 0) {
    // pool full: the engine never Destroys finished SEs, so reclaim one that has
    // been playing-but-silent for >~0.8s (a live BGM re-enqueues far sooner, so
    // it never becomes a victim). Safe to free here -- the mixer holds g_reg_lock
    // while mixing, so it can't touch the victim concurrently.
    for (int i = 0; i < g_player_count; i++) {
      Player *q = g_players[i];
      if (q && q->playing && q->drained > 40) {
        g_players[i] = NULL;
        if (q->lock) SDL_DestroyMutex(q->lock);
        bq_free_slots(q);   /* or one block per slot leaks, forever */
        free(q);
        slot = i;
        break;
      }
    }
  }
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  *pPlayer = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_PARAMETER_INVALID;
  m->obj_vt = &mix_obj_vtable;
  if (pMix) *pMix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

// --- entry point ------------------------------------------------------------

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  debugPrintf("[fmod] slCreateEngine called -> OpenSL output selected\n");
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_PARAMETER_INVALID;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  opensles_movie_end();
  if (g_dev) {
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
  }
  free(g_movie_pcm);
  g_movie_pcm = NULL;
  if (g_movie_lock) {
    SDL_DestroyMutex(g_movie_lock);
    g_movie_lock = NULL;
  }
}
