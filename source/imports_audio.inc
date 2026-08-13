/* imports_audio.inc -- audio output wiring for ZOOKEEPER DX.
 *
 * THE SHIM ITSELF IS cr3_nx's source/opensles.c + opensles.h -- reuse them
 * UNCHANGED. They implement the full Android OpenSL ES object model (Engine ->
 * OutputMix / AudioPlayer with an SLAndroidSimpleBufferQueue, plus the Play,
 * Volume and AndroidConfiguration interfaces) and back the buffer queue with an
 * SDL2 audio device, software-mixing players and firing the completion callback
 * from the audio thread -- exactly the contract Unity's FMOD opensl backend
 * (../android/src/fmod_output_opensl.cpp) relies on. It opens at the Switch's
 * native 48000 Hz and resamples each player to the device rate, so whatever
 * sample rate FMOD declares in its SLDataFormat_PCM is handled. No new audio
 * code is needed.
 *
 * WHY THIS FILE: unlike Chaos Rings III (which *links* OpenSL ES, so its symbols
 * resolve at load), libunity.so has NO OpenSL ES undefined symbols -- it
 * dlopen("libOpenSLES.so") and dlsym()s slCreateEngine + the SL_IID_* tokens at
 * runtime. cr3_nx's dlsym_fake() routes through so_resolve_external(), i.e. the
 * import table below. So the symbols must be registered here for dlsym to find
 * them. (dlopen_fake already returns a non-NULL handle for any name, so the
 * dlopen("libOpenSLES.so") succeeds.)
 *
 * INTEGRATION (three steps in imports.c):
 *   1. add at the top:                  #include "opensles.h"
 *   2. paste the block below into the default_dynlib[] table.
 *   3. nothing else -- dlopen/dlsym are already registered by cr3_nx.
 * Plus in main.c: SDL_Init(... | SDL_INIT_AUDIO ...) is already done; call
 *   opensles_shutdown();   on exit (after nativeDone()).
 *
 * Verified complete against libunity.so's dlsym surface: FMOD's opensl backend
 * dlsyms only slCreateEngine and the SL_IID_* interface tokens; every vtable
 * method (CreateAudioPlayer/SetPlayState/Enqueue/RegisterCallback/...) is reached
 * through the engine object, not via dlsym.
 */

/* ---- paste into default_dynlib[] in imports.c ---------------------------- */
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  /* the five libunity actually dlsyms: */
  SL_IID(ENGINE), SL_IID(PLAY), SL_IID(RECORD),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(ANDROIDCONFIGURATION),
  /* harmless extras (FMOD builds vary; all are self-addressed sentinels): */
  SL_IID(BUFFERQUEUE), SL_IID(VOLUME), SL_IID(OBJECT),
  SL_IID(OUTPUTMIX), SL_IID(PLAYBACKRATE),
  #undef SL_IID
/* ------------------------------------------------------------------------- */

/* Notes / what to watch on first run:
 *  - SL_IID_RECORD is the one cr3_nx's own imports.c list omits; FMOD's opensl
 *    init dlsyms it (capability probe) even though ZOOKEEPER DX records nothing.
 *    Missing it makes slCreateEngine-era init dlsym -> NULL and FMOD fall back
 *    to fmod_output_nosound (silent but not crashing) -- so if you get a clean
 *    boot but zero audio, check the dlsym(SL_IID_*) log lines first.
 *  - opensles.h also declares opensles_movie_*() -- those are CR3's FMV hooks,
 *    unreferenced by Unity. Leave them (dead) or delete; harmless either way.
 *  - The completion callback fires from the SDL audio thread without the shim
 *    lock held; FMOD's mixer enqueues from its own thread -- same threading the
 *    cr3_nx implementation was written for, so no change needed.
 *  - If audio is present but pitched/slow, the per-player resample ratio
 *    (p->rate / g_dev_rate in opensles.c) is the place to look -- but FMOD on a
 *    48000 device requests 48000, giving ratio 1.0 and no resampling.
 */
