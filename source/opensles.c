/* A minimal OpenSL ES 1.0.1 object model backed by SDL2 audio.
 *
 * Structure follows the MIT-licensed Switch so-loader work by fgsfds; the
 * mixer here is trimmed to what Oboe's OpenSL ES output path exercises.
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "opensles.h"
#include "util.h"

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED 0x0C
#define SL_RESULT_MEMORY_FAILURE 0x03
#define SL_RESULT_BUFFER_INSUFFICIENT 0x07

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE 1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

#define SL_DATAFORMAT_PCM 0x00000002
#define SL_ANDROID_DATAFORMAT_PCM_EX 0x00000004
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT 0x00000003

typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef int16_t SLint16;
typedef uint8_t SLuint8;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t SLmillibel;
typedef void *SLObjectItf;
typedef void *SLInterfaceID;

/* sampleRate is expressed in milliHertz. */
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 sampleRate;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
  SLuint32 representation; /* only present in the _PCM_EX variant */
} SLDataFormatPCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocatorBufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource, SLDataSink;

typedef void (*SLBufferQueueCallback)(void *caller, void *context);

#define DEFINE_IID(name) void *SL_IID_##name = &SL_IID_##name
DEFINE_IID(3DCOMMIT); DEFINE_IID(3DDOPPLER); DEFINE_IID(3DGROUPING);
DEFINE_IID(3DLOCATION); DEFINE_IID(3DMACROSCOPIC); DEFINE_IID(3DSOURCE);
DEFINE_IID(ANDROIDCONFIGURATION); DEFINE_IID(ANDROIDEFFECT);
DEFINE_IID(ANDROIDEFFECTCAPABILITIES); DEFINE_IID(ANDROIDEFFECTSEND);
DEFINE_IID(ANDROIDSIMPLEBUFFERQUEUE); DEFINE_IID(AUDIODECODERCAPABILITIES);
DEFINE_IID(AUDIOENCODER); DEFINE_IID(AUDIOENCODERCAPABILITIES);
DEFINE_IID(AUDIOIODEVICECAPABILITIES); DEFINE_IID(BASSBOOST);
DEFINE_IID(BUFFERQUEUE); DEFINE_IID(DEVICEVOLUME);
DEFINE_IID(DYNAMICINTERFACEMANAGEMENT); DEFINE_IID(DYNAMICSOURCE);
DEFINE_IID(EFFECTSEND); DEFINE_IID(ENGINE); DEFINE_IID(ENGINECAPABILITIES);
DEFINE_IID(ENVIRONMENTALREVERB); DEFINE_IID(EQUALIZER); DEFINE_IID(LED);
DEFINE_IID(METADATAEXTRACTION); DEFINE_IID(METADATATRAVERSAL);
DEFINE_IID(MIDIMESSAGE); DEFINE_IID(MIDIMUTESOLO); DEFINE_IID(MIDITEMPO);
DEFINE_IID(MIDITIME); DEFINE_IID(MUTESOLO); DEFINE_IID(NULL);
DEFINE_IID(OBJECT); DEFINE_IID(OUTPUTMIX); DEFINE_IID(PITCH);
DEFINE_IID(PLAY); DEFINE_IID(PLAYBACKRATE); DEFINE_IID(PREFETCHSTATUS);
DEFINE_IID(PRESETREVERB); DEFINE_IID(RATEPITCH); DEFINE_IID(RECORD);
DEFINE_IID(SEEK); DEFINE_IID(THREADSYNC); DEFINE_IID(VIBRA);
DEFINE_IID(VIRTUALIZER); DEFINE_IID(VISUALIZATION); DEFINE_IID(VOLUME);
#undef DEFINE_IID

/* ------------------------------------------------------------- interfaces */

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *state);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *out);
  SLresult (*RegisterCallback)(void *self, void *callback, void *context);
  SLresult (*AbortAsyncOperation)(void *self);
  void (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *priority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 count,
                                         SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *player,
                                SLDataSource *source, SLDataSink *sink,
                                SLuint32 count, const SLInterfaceID *ids,
                                const SLboolean *required);
  SLresult (*CreateAudioRecorder)(void *self, SLObjectItf *recorder,
                                  SLDataSource *source, SLDataSink *sink,
                                  SLuint32 count, const SLInterfaceID *ids,
                                  const SLboolean *required);
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *mix, SLuint32 count,
                              const SLInterfaceID *ids, const SLboolean *required);
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
  SLresult (*GetPlayState)(void *self, SLuint32 *state);
  SLresult (*GetDuration)(void *self, SLuint32 *msec);
  SLresult (*GetPosition)(void *self, SLuint32 *msec);
  SLresult (*RegisterCallback)(void *self, void *callback, void *context);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *mask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 msec);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *msec);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 msec);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *msec);
} SLPlayItf_;

typedef struct {
  SLuint32 count;
  SLuint32 index;
} SLBufferQueueState;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *buffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, SLBufferQueueState *state);
  SLresult (*RegisterCallback)(void *self, SLBufferQueueCallback callback,
                               void *context);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *level);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *level);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *mute);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *enabled);
  SLresult (*SetStereoPosition)(void *self, SLint32 per_mille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *per_mille);
} SLVolumeItf_;

typedef struct {
  SLresult (*SetConfiguration)(void *self, const void *key, const void *value,
                               SLuint32 size);
  SLresult (*GetConfiguration)(void *self, const void *key, SLuint32 *size,
                               void *value);
  SLresult (*AcquireJavaProxy)(void *self, SLuint32 type, void *proxy);
  SLresult (*ReleaseJavaProxy)(void *self, SLuint32 type);
} SLAndroidConfigurationItf_;

/* ----------------------------------------------------------------- players */

#define MAX_PLAYERS 8
#define QUEUE_SLOTS 64

typedef struct {
  const void *data;
  SLuint32 size;
} QueuedBuffer;

/* Oboe sets a handful of Android-specific keys before realizing a player and
 * reads the performance mode back afterwards. Reporting that key as
 * unsupported fails the whole stream open on API 28 and above, so whatever
 * was set is remembered and handed back. */
#define CONFIG_KEYS 8
#define CONFIG_KEY_LENGTH 40

typedef struct {
  char key[CONFIG_KEY_LENGTH];
  SLuint32 value;
} ConfigEntry;

typedef struct Player {
  const SLObjectItf_ *object_vt;
  const SLPlayItf_ *play_vt;
  const SLBufferQueueItf_ *queue_vt;
  const SLVolumeItf_ *volume_vt;
  const SLAndroidConfigurationItf_ *config_vt;

  int in_use;
  int channels;
  int rate;
  int sample_bytes;
  int is_float;
  int playing;
  float gain;
  uint64_t frames_played;

  SLBufferQueueCallback callback;
  void *callback_context;

  ConfigEntry config[CONFIG_KEYS];
  int config_count;

  QueuedBuffer queue[QUEUE_SLOTS];
  int head, tail;
  const uint8_t *current;
  SLuint32 current_size;
  double current_frame;

  SDL_mutex *lock;
} Player;

typedef struct {
  const SLObjectItf_ *object_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *object_vt;
  const SLEngineItf_ *engine_vt;
} Engine;

#define CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

/* Enough to tell a silent device from a starved mixer from a stalled
 * buffer queue, without flooding the log. */
static volatile unsigned g_callback_count;
static volatile unsigned g_enqueue_count;
static volatile unsigned g_completion_count;
static volatile unsigned g_underrun_count;
static volatile unsigned g_mixed_frames;

static SDL_AudioDeviceID g_device;
static int g_device_rate = 48000;
static Player *g_players[MAX_PLAYERS];
static int g_player_count;
static SDL_mutex *g_registry_lock;

static float millibel_to_linear(SLmillibel level) {
  if (level <= -9600) return 0.0f;
  return powf(10.0f, (float)level / 2000.0f); /* 100 mB per dB */
}

static inline int32_t read_sample(const void *buffer, long index, int sample_bytes,
                                  int is_float) {
  if (is_float) {
    float value = ((const float *)buffer)[index];
    if (value > 1.0f) value = 1.0f;
    else if (value < -1.0f) value = -1.0f;
    return (int32_t)(value * 32767.0f);
  }
  if (sample_bytes == 4) return ((const int32_t *)buffer)[index] >> 16;
  return ((const int16_t *)buffer)[index];
}

/* Completion callbacks run with the queue lock released: Oboe re-enqueues
 * from inside them. */
static void mix_player(Player *player, int32_t *accumulator, int frames) {
  if (!player->playing) return;

  SDL_LockMutex(player->lock);
  const int dry = !player->current && player->head == player->tail;
  SDL_UnlockMutex(player->lock);
  if (dry) return;

  const float gain = player->gain;
  const int stereo = player->channels >= 2;
  const int sample_bytes = player->sample_bytes > 0 ? player->sample_bytes : 2;
  const int frame_bytes = stereo ? sample_bytes * 2 : sample_bytes;
  const double ratio =
      g_device_rate > 0 ? (double)player->rate / (double)g_device_rate : 1.0;

  for (int i = 0; i < frames; i++) {
    for (;;) {
      if (!player->current) {
        SDL_LockMutex(player->lock);
        const int have = player->head != player->tail;
        QueuedBuffer buffer = {NULL, 0};
        if (have) {
          buffer = player->queue[player->head];
          player->head = (player->head + 1) % QUEUE_SLOTS;
        }
        SDL_UnlockMutex(player->lock);
        if (!have) {
          __atomic_fetch_add(&g_underrun_count, 1, __ATOMIC_RELAXED);
          return; /* underrun: the rest of the block stays silent */
        }
        player->current = buffer.data;
        player->current_size = buffer.size;
      }
      const long available = (long)(player->current_size / (SLuint32)frame_bytes);
      if (available > 0 && (long)player->current_frame < available) break;

      player->current_frame -= (double)available;
      if (player->current_frame < 0.0) player->current_frame = 0.0;
      player->current = NULL;
      if (player->callback) {
        __atomic_fetch_add(&g_completion_count, 1, __ATOMIC_RELAXED);
        player->callback(&player->queue_vt, player->callback_context);
      }
    }

    const long available = (long)(player->current_size / (SLuint32)frame_bytes);
    const long index = (long)player->current_frame;
    const double fraction = player->current_frame - (double)index;
    const long next = (index + 1 < available) ? index + 1 : index;
    const void *source = player->current;

    int32_t left, right;
    if (stereo) {
      const int32_t l0 = read_sample(source, index * 2, sample_bytes, player->is_float);
      const int32_t l1 = read_sample(source, next * 2, sample_bytes, player->is_float);
      const int32_t r0 = read_sample(source, index * 2 + 1, sample_bytes, player->is_float);
      const int32_t r1 = read_sample(source, next * 2 + 1, sample_bytes, player->is_float);
      left = (int32_t)(l0 * (1.0 - fraction) + l1 * fraction);
      right = (int32_t)(r0 * (1.0 - fraction) + r1 * fraction);
    } else {
      const int32_t a = read_sample(source, index, sample_bytes, player->is_float);
      const int32_t b = read_sample(source, next, sample_bytes, player->is_float);
      left = right = (int32_t)(a * (1.0 - fraction) + b * fraction);
    }
    accumulator[i * 2 + 0] += (int32_t)(left * gain);
    accumulator[i * 2 + 1] += (int32_t)(right * gain);
    player->current_frame += ratio;
  }
  player->frames_played += (uint64_t)frames;
  __atomic_fetch_add(&g_mixed_frames, (unsigned)frames, __ATOMIC_RELAXED);
}

static void SDLCALL audio_callback(void *user, Uint8 *stream, int length) {
  (void)user;

  /* Guest completion callbacks run on this thread, so it needs a bionic TLS
   * block for the stack-protector canary. */
  static uint8_t audio_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  static int tls_ready;
  if (!tls_ready) {
    install_bionic_tls(audio_tls);
    tls_ready = 1;
  }

  const int frames = length / 4; /* signed 16-bit stereo */
  static int32_t accumulator[4096 * 2];
  if (frames > 4096) {
    memset(stream, 0, (size_t)length);
    return;
  }
  memset(accumulator, 0, (size_t)frames * 2 * sizeof(int32_t));

  SDL_LockMutex(g_registry_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] && g_players[i]->in_use) mix_player(g_players[i], accumulator, frames);
  SDL_UnlockMutex(g_registry_lock);

  /* Roughly every five seconds at the usual buffer size. */
  const unsigned tick = __atomic_fetch_add(&g_callback_count, 1, __ATOMIC_RELAXED);
  if (tick % 240 == 0) {
    trace("audio: %u callbacks, %u enqueued, %u completed, %u underruns, "
          "%u frames mixed",
          tick + 1, g_enqueue_count, g_completion_count, g_underrun_count,
          g_mixed_frames);
  }

  int16_t *out = (int16_t *)stream;
  for (int i = 0; i < frames * 2; i++) {
    int32_t value = accumulator[i];
    if (value > 32767) value = 32767;
    else if (value < -32768) value = -32768;
    out[i] = (int16_t)value;
  }
}

static int ensure_device(int rate) {
  if (g_device) {
    trace("audio device already open (id %u)", (unsigned)g_device);
    return 1;
  }

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof want);
  want.freq = rate > 0 ? rate : 48000;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_callback;

  const char *driver = SDL_GetCurrentAudioDriver();
  trace("opening audio: driver=%s devices=%d requested %d Hz",
        driver ? driver : "(none)", SDL_GetNumAudioDevices(0), want.freq);

  memset(&have, 0, sizeof have);
  g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_device) {
    /* Asking for an exact spec fails if the console's driver wants a
     * different buffer size or rate. The mixer already resamples and the
     * callback works from whatever size it is handed, so let the driver
     * pick and adapt to it. */
    trace("exact audio spec refused (%s); retrying with driver defaults",
          SDL_GetError());
    memset(&have, 0, sizeof have);
    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                       SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  }
  if (!g_device) {
    trace("SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return 0;
  }
  if (have.format != AUDIO_S16SYS || have.channels != 2) {
    /* The mixer writes interleaved stereo signed 16-bit and cannot convert
     * on the fly, so anything else has to be refused rather than played as
     * noise. */
    trace("unusable audio format 0x%x with %d channels; closing",
          (unsigned)have.format, have.channels);
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
    return 0;
  }
  g_device_rate = have.freq;
  SDL_PauseAudioDevice(g_device, 0);
  trace("audio device %u open at %d Hz, %d frames, %d ch, format 0x%x",
        (unsigned)g_device, have.freq, have.samples, have.channels,
        (unsigned)have.format);
  return 1;
}

/* --------------------------------------------------------- object vtables */

static SLresult object_realize(void *self, SLboolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_SUCCESS;
}
static SLresult object_resume(void *self, SLboolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_SUCCESS;
}
static SLresult object_get_state(void *self, SLuint32 *state) {
  (void)self;
  if (state) *state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}
static SLresult object_register_callback(void *self, void *cb, void *ctx) {
  (void)self;
  (void)cb;
  (void)ctx;
  return SL_RESULT_SUCCESS;
}
static SLresult object_abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult object_set_priority(void *self, SLint32 p, SLboolean e) {
  (void)self; (void)p; (void)e;
  return SL_RESULT_SUCCESS;
}
static SLresult object_get_priority(void *self, SLint32 *p) {
  (void)self;
  if (p) *p = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult object_set_loss_of_control(void *self, SLint32 n,
                                           SLInterfaceID *ids, SLboolean e) {
  (void)self; (void)n; (void)ids; (void)e;
  return SL_RESULT_SUCCESS;
}

/* ---- player ---- */

static SLresult play_set_state(void *self, SLuint32 state) {
  Player *player = CONTAINER_OF(self, Player, play_vt);
  const int playing = (state == SL_PLAYSTATE_PLAYING);
  if (playing != player->playing) trace("audio play state -> %d", playing);
  player->playing = playing;
  return SL_RESULT_SUCCESS;
}
static SLresult play_get_state(void *self, SLuint32 *state) {
  Player *player = CONTAINER_OF(self, Player, play_vt);
  if (state) *state = player->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_get_duration(void *self, SLuint32 *msec) {
  (void)self;
  if (msec) *msec = 0xFFFFFFFFu; /* SL_TIME_UNKNOWN */
  return SL_RESULT_SUCCESS;
}
static SLresult play_get_position(void *self, SLuint32 *msec) {
  Player *player = CONTAINER_OF(self, Player, play_vt);
  if (msec)
    *msec = (SLuint32)(player->rate > 0
                           ? (player->frames_played * 1000ull) / (uint64_t)g_device_rate
                           : 0);
  return SL_RESULT_SUCCESS;
}
static SLresult play_noop_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_noop_pu32(void *self, SLuint32 *v) {
  (void)self;
  if (v) *v = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult play_noop(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_register_callback(void *self, void *cb, void *ctx) {
  (void)self; (void)cb; (void)ctx;
  return SL_RESULT_SUCCESS;
}

static const SLPlayItf_ kPlayVT = {
    play_set_state, play_get_state, play_get_duration, play_get_position,
    play_register_callback, play_noop_u32, play_noop_pu32, play_noop_u32,
    play_noop, play_noop_pu32, play_noop_u32, play_noop_pu32};

/* ---- buffer queue ---- */

static SLresult queue_enqueue(void *self, const void *buffer, SLuint32 size) {
  Player *player = CONTAINER_OF(self, Player, queue_vt);
  const unsigned seen = __atomic_fetch_add(&g_enqueue_count, 1, __ATOMIC_RELAXED);
  if (seen < 4)
    trace("audio enqueue #%u: %u bytes, playing=%d", seen + 1, (unsigned)size,
          player->playing);
  SDL_LockMutex(player->lock);
  const int next = (player->tail + 1) % QUEUE_SLOTS;
  if (next == player->head) {
    SDL_UnlockMutex(player->lock);
    return SL_RESULT_MEMORY_FAILURE; /* queue full */
  }
  player->queue[player->tail].data = buffer;
  player->queue[player->tail].size = size;
  player->tail = next;
  SDL_UnlockMutex(player->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_clear(void *self) {
  Player *player = CONTAINER_OF(self, Player, queue_vt);
  SDL_LockMutex(player->lock);
  player->head = player->tail = 0;
  player->current = NULL;
  player->current_size = 0;
  player->current_frame = 0.0;
  SDL_UnlockMutex(player->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_get_state(void *self, SLBufferQueueState *state) {
  Player *player = CONTAINER_OF(self, Player, queue_vt);
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  SDL_LockMutex(player->lock);
  state->count =
      (SLuint32)((player->tail - player->head + QUEUE_SLOTS) % QUEUE_SLOTS);
  state->index = (SLuint32)player->head;
  SDL_UnlockMutex(player->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult queue_register_callback(void *self, SLBufferQueueCallback cb,
                                        void *ctx) {
  Player *player = CONTAINER_OF(self, Player, queue_vt);
  player->callback = cb;
  player->callback_context = ctx;
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ kQueueVT = {queue_enqueue, queue_clear,
                                           queue_get_state,
                                           queue_register_callback};

/* ---- volume ---- */

static SLresult volume_set_level(void *self, SLmillibel level) {
  Player *player = CONTAINER_OF(self, Player, volume_vt);
  player->gain = millibel_to_linear(level);
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_level(void *self, SLmillibel *level) {
  (void)self;
  if (level) *level = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_max(void *self, SLmillibel *level) {
  (void)self;
  if (level) *level = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_set_mute(void *self, SLboolean mute) {
  Player *player = CONTAINER_OF(self, Player, volume_vt);
  player->gain = mute ? 0.0f : 1.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_mute(void *self, SLboolean *mute) {
  (void)self;
  if (mute) *mute = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_enable_stereo(void *self, SLboolean enable) {
  (void)self; (void)enable;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_is_stereo(void *self, SLboolean *enabled) {
  (void)self;
  if (enabled) *enabled = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_set_position(void *self, SLint32 per_mille) {
  (void)self; (void)per_mille;
  return SL_RESULT_SUCCESS;
}
static SLresult volume_get_position(void *self, SLint32 *per_mille) {
  (void)self;
  if (per_mille) *per_mille = 0;
  return SL_RESULT_SUCCESS;
}

static const SLVolumeItf_ kVolumeVT = {
    volume_set_level, volume_get_level, volume_get_max, volume_set_mute,
    volume_get_mute, volume_enable_stereo, volume_is_stereo,
    volume_set_position, volume_get_position};

/* ---- android configuration ---- */

static ConfigEntry *config_find(Player *player, const char *key) {
  for (int i = 0; i < player->config_count; i++)
    if (!strcmp(player->config[i].key, key)) return &player->config[i];
  return NULL;
}

static SLresult config_set(void *self, const void *key, const void *value,
                           SLuint32 size) {
  Player *player = CONTAINER_OF(self, Player, config_vt);
  if (!key || !value || size < sizeof(SLuint32)) return SL_RESULT_PARAMETER_INVALID;

  ConfigEntry *entry = config_find(player, (const char *)key);
  if (!entry) {
    if (player->config_count >= CONFIG_KEYS) return SL_RESULT_SUCCESS;
    entry = &player->config[player->config_count++];
    snprintf(entry->key, sizeof entry->key, "%s", (const char *)key);
  }
  entry->value = *(const SLuint32 *)value;
  return SL_RESULT_SUCCESS;
}

static SLresult config_get(void *self, const void *key, SLuint32 *size,
                           void *value) {
  Player *player = CONTAINER_OF(self, Player, config_vt);
  if (!key || !size) return SL_RESULT_PARAMETER_INVALID;

  const ConfigEntry *entry = config_find(player, (const char *)key);
  if (!entry) return SL_RESULT_FEATURE_UNSUPPORTED;

  if (!value) {
    *size = sizeof(SLuint32);
    return SL_RESULT_SUCCESS;
  }
  if (*size < sizeof(SLuint32)) {
    *size = sizeof(SLuint32);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  *(SLuint32 *)value = entry->value;
  *size = sizeof(SLuint32);
  return SL_RESULT_SUCCESS;
}
static SLresult config_acquire(void *self, SLuint32 type, void *proxy) {
  (void)self; (void)type; (void)proxy;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult config_release(void *self, SLuint32 type) {
  (void)self; (void)type;
  return SL_RESULT_SUCCESS;
}

static const SLAndroidConfigurationItf_ kConfigVT = {config_set, config_get,
                                                     config_acquire,
                                                     config_release};

/* ---- object dispatch ---- */

static void player_destroy(void *self);
static SLresult player_get_interface(void *self, const SLInterfaceID iid, void *out);
static void mix_destroy(void *self);
static SLresult mix_get_interface(void *self, const SLInterfaceID iid, void *out);
static void engine_destroy(void *self);
static SLresult engine_get_interface(void *self, const SLInterfaceID iid, void *out);

static const SLObjectItf_ kPlayerObjectVT = {
    object_realize, object_resume, object_get_state, player_get_interface,
    object_register_callback, object_abort, player_destroy, object_set_priority,
    object_get_priority, object_set_loss_of_control};

static const SLObjectItf_ kMixObjectVT = {
    object_realize, object_resume, object_get_state, mix_get_interface,
    object_register_callback, object_abort, mix_destroy, object_set_priority,
    object_get_priority, object_set_loss_of_control};

static const SLObjectItf_ kEngineObjectVT = {
    object_realize, object_resume, object_get_state, engine_get_interface,
    object_register_callback, object_abort, engine_destroy, object_set_priority,
    object_get_priority, object_set_loss_of_control};

static SLresult player_get_interface(void *self, const SLInterfaceID iid, void *out) {
  Player *player = CONTAINER_OF(self, Player, object_vt);
  if (!out) return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) { *(void **)out = &player->play_vt; return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)out = &player->queue_vt;
    return SL_RESULT_SUCCESS;
  }
  if (iid == SL_IID_VOLUME) { *(void **)out = &player->volume_vt; return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_ANDROIDCONFIGURATION) {
    *(void **)out = &player->config_vt;
    return SL_RESULT_SUCCESS;
  }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void player_destroy(void *self) {
  Player *player = CONTAINER_OF(self, Player, object_vt);
  SDL_LockMutex(g_registry_lock);
  player->in_use = 0;
  player->playing = 0;
  SDL_UnlockMutex(g_registry_lock);
}

static SLresult mix_get_interface(void *self, const SLInterfaceID iid, void *out) {
  (void)self;
  (void)iid;
  (void)out;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void mix_destroy(void *self) {
  OutputMix *mix = CONTAINER_OF(self, OutputMix, object_vt);
  free(mix);
}

/* ---- engine ---- */

static SLresult engine_create_output_mix(void *self, SLObjectItf *out,
                                         SLuint32 count, const SLInterfaceID *ids,
                                         const SLboolean *required) {
  (void)self; (void)count; (void)ids; (void)required;
  if (!out) return SL_RESULT_PARAMETER_INVALID;
  OutputMix *mix = calloc(1, sizeof *mix);
  if (!mix) return SL_RESULT_MEMORY_FAILURE;
  mix->object_vt = &kMixObjectVT;
  *out = &mix->object_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_audio_player(void *self, SLObjectItf *out,
                                           SLDataSource *source, SLDataSink *sink,
                                           SLuint32 count,
                                           const SLInterfaceID *ids,
                                           const SLboolean *required) {
  (void)self; (void)sink; (void)count; (void)ids; (void)required;
  if (!out || !source || !source->pFormat) return SL_RESULT_PARAMETER_INVALID;

  const SLDataFormatPCM *format = source->pFormat;
  const int channels = format->numChannels >= 2 ? 2 : 1;
  const int rate = (int)(format->sampleRate / 1000u); /* milliHz to Hz */
  const int sample_bytes = (int)(format->bitsPerSample / 8u);
  const int is_float = format->formatType == SL_ANDROID_DATAFORMAT_PCM_EX &&
                       format->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT;

  if (!ensure_device(rate)) return SL_RESULT_MEMORY_FAILURE;

  Player *player = calloc(1, sizeof *player);
  if (!player) return SL_RESULT_MEMORY_FAILURE;
  player->object_vt = &kPlayerObjectVT;
  player->play_vt = &kPlayVT;
  player->queue_vt = &kQueueVT;
  player->volume_vt = &kVolumeVT;
  player->config_vt = &kConfigVT;
  player->channels = channels;
  player->rate = rate > 0 ? rate : g_device_rate;
  player->sample_bytes = sample_bytes > 0 ? sample_bytes : 2;
  player->is_float = is_float;
  player->gain = 1.0f;
  player->in_use = 1;
  player->lock = SDL_CreateMutex();
  if (!player->lock) { free(player); return SL_RESULT_MEMORY_FAILURE; }

  SDL_LockMutex(g_registry_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++) {
    if (!g_players[i] || !g_players[i]->in_use) { slot = i; break; }
  }
  if (slot < 0 && g_player_count < MAX_PLAYERS) slot = g_player_count++;
  if (slot >= 0) g_players[slot] = player;
  SDL_UnlockMutex(g_registry_lock);

  if (slot < 0) {
    SDL_DestroyMutex(player->lock);
    free(player);
    return SL_RESULT_MEMORY_FAILURE;
  }

  trace("OpenSL player: %d ch, %d Hz, %d-bit %s", channels, player->rate,
        player->sample_bytes * 8, is_float ? "float" : "int");

  *out = &player->object_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_create_audio_recorder(void *self, SLObjectItf *out,
                                             SLDataSource *source,
                                             SLDataSink *sink, SLuint32 count,
                                             const SLInterfaceID *ids,
                                             const SLboolean *required) {
  (void)self; (void)out; (void)source; (void)sink; (void)count; (void)ids;
  (void)required;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLEngineItf_ kEngineVT = {
    NULL, NULL, engine_create_audio_player, engine_create_audio_recorder,
    NULL, NULL, NULL, engine_create_output_mix,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static Engine g_engine;

static SLresult engine_get_interface(void *self, const SLInterfaceID iid, void *out) {
  (void)self;
  if (!out) return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)out = &g_engine.engine_vt;
    return SL_RESULT_SUCCESS;
  }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void engine_destroy(void *self) { (void)self; }

uint32_t slCreateEngine(void **engine, uint32_t num_options, const void *options,
                        uint32_t num_interfaces, const void *interface_ids,
                        const void *required) {
  (void)num_options; (void)options; (void)num_interfaces; (void)interface_ids;
  (void)required;
  if (!engine) return SL_RESULT_PARAMETER_INVALID;

  if (!g_registry_lock) {
    trace("slCreateEngine: bringing up SDL audio");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
      trace("SDL audio init failed: %s", SDL_GetError());
    g_registry_lock = SDL_CreateMutex();
    if (!g_registry_lock) return SL_RESULT_MEMORY_FAILURE;
  }

  g_engine.object_vt = &kEngineObjectVT;
  g_engine.engine_vt = &kEngineVT;
  *engine = &g_engine.object_vt;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  if (g_device) {
    SDL_PauseAudioDevice(g_device, 1);
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
  }
}

/* ------------------------------------------------------------ name lookup */

typedef struct {
  const char *name;
  void *value;
} NamedSymbol;

#define IID_ENTRY(n) {"SL_IID_" #n, &SL_IID_##n}

static const NamedSymbol kSymbols[] = {
    {"slCreateEngine", (void *)slCreateEngine},
    IID_ENTRY(3DCOMMIT), IID_ENTRY(3DDOPPLER), IID_ENTRY(3DGROUPING),
    IID_ENTRY(3DLOCATION), IID_ENTRY(3DMACROSCOPIC), IID_ENTRY(3DSOURCE),
    IID_ENTRY(ANDROIDCONFIGURATION), IID_ENTRY(ANDROIDEFFECT),
    IID_ENTRY(ANDROIDEFFECTCAPABILITIES), IID_ENTRY(ANDROIDEFFECTSEND),
    IID_ENTRY(ANDROIDSIMPLEBUFFERQUEUE), IID_ENTRY(AUDIODECODERCAPABILITIES),
    IID_ENTRY(AUDIOENCODER), IID_ENTRY(AUDIOENCODERCAPABILITIES),
    IID_ENTRY(AUDIOIODEVICECAPABILITIES), IID_ENTRY(BASSBOOST),
    IID_ENTRY(BUFFERQUEUE), IID_ENTRY(DEVICEVOLUME),
    IID_ENTRY(DYNAMICINTERFACEMANAGEMENT), IID_ENTRY(DYNAMICSOURCE),
    IID_ENTRY(EFFECTSEND), IID_ENTRY(ENGINE), IID_ENTRY(ENGINECAPABILITIES),
    IID_ENTRY(ENVIRONMENTALREVERB), IID_ENTRY(EQUALIZER), IID_ENTRY(LED),
    IID_ENTRY(METADATAEXTRACTION), IID_ENTRY(METADATATRAVERSAL),
    IID_ENTRY(MIDIMESSAGE), IID_ENTRY(MIDIMUTESOLO), IID_ENTRY(MIDITEMPO),
    IID_ENTRY(MIDITIME), IID_ENTRY(MUTESOLO), IID_ENTRY(NULL),
    IID_ENTRY(OBJECT), IID_ENTRY(OUTPUTMIX), IID_ENTRY(PITCH),
    IID_ENTRY(PLAY), IID_ENTRY(PLAYBACKRATE), IID_ENTRY(PREFETCHSTATUS),
    IID_ENTRY(PRESETREVERB), IID_ENTRY(RATEPITCH), IID_ENTRY(RECORD),
    IID_ENTRY(SEEK), IID_ENTRY(THREADSYNC), IID_ENTRY(VIBRA),
    IID_ENTRY(VIRTUALIZER), IID_ENTRY(VISUALIZATION), IID_ENTRY(VOLUME),
};

#undef IID_ENTRY

void *opensles_lookup(const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < sizeof kSymbols / sizeof kSymbols[0]; i++)
    if (!strcmp(kSymbols[i].name, name)) return kSymbols[i].value;
  return NULL;
}
