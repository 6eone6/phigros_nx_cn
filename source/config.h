/* config.h -- Plants vs Zombies Fusion 3.6.1 Switch wrapper configuration
 * (forked from the Zookeeper DX / CR3 wrapper config.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

/* ============================ MEMORY LAYOUT ==============================
 * These are engine-fitting parameters, not game content -- identical to the
 * Zookeeper DX port because Phigros is the SAME Unity minor version
 * (2022.3.62) and we apply the SAME 256MB->64MB region-granularity patch
 * (see nx_patch_phigros.h). Do not change unless you know the allocator math.
 * ======================================================================== */

// The engine + libc++ + il2cpp heap need a generous newlib heap; the rest of
// system memory is handed to the .so loader (see __libnx_initheap).
#define MEMORY_MB 768

// Anonymous-mmap arena. Unity reserves big region-aligned pools by over-mmapping
// then munmapping the unaligned head/tail. We back anonymous mmaps from a
// dedicated, region-aligned arena with a per-page used-bitmap so sub-range
// munmap frees exactly the trimmed pages. Region granularity is 64MB to match
// the libunity patch.
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)    // 64MB region granularity (libunity patched 256MB->64MB, nx_patch_phigros.h). NOTE: 16MB was tried and CORRUPTED Unity's Dynamic Heap allocator at init (overlapping regions from the over-map/trim pattern -> null-prev free-list crash). 64MB is the known-good floor.
/* Round 152: 192 -> 896 MB. The r151 log finally produced the failure this
 * comment has predicted for dozens of rounds, with the exact recorded
 * signature:
 *
 *   [oc] ARMED: window 704 MB          <- only 704 this boot; 1152-1600 in others
 *   [mem] arena 81% reserved (157 of 192 MB)
 *   [mmap] OC window full for 319 MB -> heap-backed arena
 *   [mmap] arena full -> newlib fallback (#1) -- memory pressure
 *   [mmap] 127 MB (prot=0x0 anon=1) -> 0x0     <- mmap returned NULL
 *   [mmap] arena full -> newlib fallback (#2) -- memory pressure
 *   -> crash in libunity
 *
 * "couldn't fit a 127MB OC-window overflow -> mmap NULL -> Unity crash" is
 * word for word what 512 MB did. We were running 192.
 *
 * It also explains the intermittency: the OC window is clamped to the largest
 * stack-region hole, which is different every boot (704 MB here, 1152-1600 in
 * runs that were fine). A big window absorbs the spill; a small one pushes it
 * into this arena, which at 192 MB could not take it. "A couple of perfect runs
 * then a crash" is that lottery.
 *
 * 896 is the value this comment has recommended all along = 1.75x the 512 fail
 * point. Anything at or below 512 is KNOWN to fail -- do not "compromise" at
 * 400. Paid for by right-sizing OC_POOL_BYTES below. */
#define MMAP_ARENA_RESERVE  ((size_t)896 * 1024 * 1024)

// Stack-region overcommit (OC) arena (see libc_shim.c): PROT_NONE reservations
// held in a stack-region window, committed pages backed from a small heap pool.
#define OC_WINDOW_BYTES     ((size_t)2048 * 1024 * 1024)  // 32x64MB cheap PROT_NONE reservation. Was 1536; the window-finder clamps to the largest stack-region hole (min(this, hole)), so raising the cap lets a run use its full hole and spill fewer reservations into the (real-memory) arena.
// Commit-pool: real memory backing touched pages of the OC window. Unity is told it
// has 512 MB (libc_shim.c __sysconf PHYS_PAGES + dalvik.vm.heapsize), and it reserves
// its heaps as big PROT_NONE regions that route here, so this pool must be able to
// back the full 512 MB Unity believes it has -- at 256 MB the scene load exhausted it
// (~270 MB working set) and the next uncommitted page faulted -> hard OOM crash.
// malloc. UPDATE 2: trimming the pool 1280->1024 + arena 1024->512 fixed the malloc
// OOM (malloc 1409 -> game pushed past the null-buffer crash) but 512 starved the
// arena (see above). That round settled on pool 896 + arena 896 + malloc 1153.
//
// UPDATE 3 (round 107): those last two no longer describe the tree. The ACTUAL
// values are pool 512 and MMAP_ARENA_RESERVE 896 as of round 152. (Between
// r107 and r152 they were pool 1024 / arena 192, and neither comment said so --
// the recorded "balance" was fiction for 45 rounds. It is accurate again now.) Against the observed 2752 MB newlib heap that gives
// pool 1024 + arena 192 + so_region 160 = 1376 MB, leaving ~1376 MB for malloc.
// Known failure points, for reference when one of these OOMs again:
//   pool live 551 | arena fail 512 | malloc fail 641
// Note the arena is now BELOW its recorded failure point. It has been booting, so
// the enlarged OC window is evidently absorbing what used to spill there, but it
// is the next thing to look at if an allocation failure shows up in the arena.
// RAISED 896 -> 1024 (round 107). Phigros's live footprint is not PvZ's --
// 244 MB observed at ModeSelect here, but the out-of-memory crash happened later,
// in play, and this is the pool that has to absorb it. +128 MB comes out of the
// plain-malloc share, which currently sits ~1376 MB against a recorded failure
// point of 641 MB, so there is room to give.
//
// Safe to raise now: main.c retries in 128 MB steps down to OC_POOL_MIN_BYTES if
// the allocation does not fit, and logs what it settled on. Previously a pool
// that was too big meant OC DISABLED and a dead boot, which is why this number
// had not been touched.
/* Round 152: 1024 -> 512 MB, to pay for the arena above without asking newlib
 * for more in total. 512 is this comment's own documented floor -- "must be
 * able to back the full 512 MB Unity believes it has" -- and the r151 session
 * peaked at `[oc] committed 235 MB (pool 235/1024)`, so it never used even half
 * of 512. The pool was over-reserved by ~4x while the arena starved.
 *
 * Budget against the observed 2752 MB newlib heap:
 *     before  pool 1024 + arena 192 = 1216   -> malloc ~1536, arena OOM'd
 *     after   pool  512 + arena 896 = 1408   -> malloc ~1344, still above the
 *                                               1153 the history settled on
 * If a scene ever pushes the pool past 512 the symptom is a hard OOM on an
 * uncommitted page (see above), and the fix is to take it back off the arena. */
#define OC_POOL_BYTES       ((size_t)512 * 1024 * 1024)   // commit-pool (touched pages only)
#define OC_POOL_MIN_BYTES   ((size_t)640 * 1024 * 1024)   // ladder floor; below the
                                                          // 551 MB live high-water there
                                                          // is no point continuing

// Overcommit (alias-region) mode: reserve a big *virtual* window (PROT_NONE
// costs only address space) and commit physical pages on demand -- true
// overcommit, matching Android.
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)  // 6 GB virtual reservation window
#define OVERCOMMIT_HEAP_MB  608u                          // newlib malloc + .so load zone

/* ============================ GAME IDENTITY =============================== */

// Phigros ships the engine as the standard modern Unity trio; libmain.so
// dlopens libunity.so which dlopens libil2cpp.so. (No libcrx/MVGL here -- this
// is a normal IL2CPP game, so main.c loads libmain/libunity/libil2cpp directly
// and these SO_NAME macros are unused, kept only for parity with the base.)
#define SO_NAME      "libunity.so"
#define SO_CPP_NAME  "libil2cpp.so"

// The SD-card folder holding the .nro + the game files.
/* FALLBACK ONLY -- you almost certainly do not need to change this.
 *
 * The loader resolves its data root at RUNTIME (nx_root.c): the folder the .nro
 * was launched from, or failing that a scan of /switch for the folder that
 * actually contains libmain.so. Name the folder whatever you like.
 *
 * This constant is used only if both of those fail, which in practice means
 * argv[0] was empty AND nothing under /switch had libmain.so -- i.e. the game
 * is not staged at all, and the next check is about to say so with a path.
 *
 * Covered by tools/test/run.sh. */
#define GAME_FOLDER  "phigros"

/* Bump when shipping. Printed at compile time (#pragma message in main.c)
 * and at boot, so a stale source tree is obvious from either the build
 * output or debug.log. */
#define PHI_SRC_REV   "phigros-r1.0-release"

/* ---- Android package name -- VERIFY THIS AGAINST YOUR APK -----------------
 * Returned by our fake getPackageName(). Unity surfaces it as
 * Application.identifier, and game code (and any SDK that keys off it) can
 * read it.
 *
 * NOTE: the PvZ tree inherited Zookeeper's "jp.kiteretsu.zookeeper_dx" here and
 * never changed it -- both of those ports shipped reporting the WRONG package
 * name. Do not repeat that.
 *
 * The package name is not in libunity/libil2cpp (IL2CPP string literals live
 * in global-metadata.dat, not the .so). It DOES appear in classes.dex, which
 * carries com/PigeonGames/Phigros/R -- that is where the default below comes
 * from. Confirm against your own APK anyway:
 *     aapt dump badging Phigros.apk | head -1                                */
#define GAME_PACKAGE "com.PigeonGames.Phigros"  /* <-- CHECK YOUR MANIFEST */

/* ---- split-asset auto-join -----------------------------------------------
 * REMOVED. Fruit Ninja shipped 1 MiB .split0/.split1 parts that Unity joins in
 * Java; Phigros does not. nx_splitjoin.c is deleted from this tree. If a
 * future Phigros build ever ships .splitN parts, lift the file back from the
 * fruitninja_nx tree -- it is self-contained. */
#define JOIN_DELETE_PARTS 0

/* Diagnostic: trace futex WAIT/WAKE in the contended allocator region to
 * locate a lost wake. Verbose; enabled for this bring-up build only. */
#define PHI_FUTEX_TRACE 0

/* ---- multitouch ----------------------------------------------------------
 * The Switch panel reports up to 10 simultaneous touches
 * (hidGetTouchScreenStates), and nx_dual_pointer.c already tracks them with
 * stable per-finger ids and proper DOWN/MOVE/UP transitions -- that machinery
 * is inherited and works; only the CAPS needed changing for this game.
 *
 * Phigros judges on finger INDEX (FingerManagement.CheckNote(fingerIndex),
 * CheckFlick(fingerIndex)), so a finger that never gets an id is a missed note
 * and nothing in the log will say why. All 10 panel slots therefore go to real
 * fingers, and the two stick cursors are pushed above them.
 *
 * Raising this above 10 gains nothing (the panel will not report more) and
 * costs pointer-pool headroom; UI_MAX_POINTERS in unity_input.h must stay at
 * least PHI_TOUCH_SLOTS + 2.
 *
 * The two stick cursors take the next two ids (PHI_TOUCH_SLOTS and +1), so this
 * one knob keeps everything consistent. Fingers and cursors never coexist --
 * cursors are hidden in handheld (nxdp_init sets s_visible = nxdp_docked()) and
 * there is no touchscreen when docked -- so they cannot contend.
 *
 * IF DOCKED CURSOR TAPS DO NOT REGISTER: Unity maps Android pointer ids into a
 * fixed-size pool and drops large ones (this is why the module's 100/101
 * defaults failed upstream). Ids 10/11 are past anything the lineage has
 * tested. Drop this to 8 and the cursors fall back to 8/9, which is the
 * known-good Fruit Ninja arrangement -- at the cost of the 9th and 10th
 * finger, which docked mode cannot use anyway. */
#define PHI_TOUCH_SLOTS 10

/* ---- virtual cursor: REMOVED --------------------------------------------
 * The two stick-driven cursors are gone. Phigros judges on finger index, so a
 * pair of analogue cursors could never play it -- they existed only so DOCKED
 * mode had some pointer, and docked was always menus-only.
 *
 * IMPORTANT: this does NOT remove touch. The Switch touchscreen path
 * (hidInitializeTouchScreen / hidGetTouchScreenStates, PHI_TOUCH_SLOTS real
 * fingers) lives in the same file, nx_dual_pointer.c, and is what the game is
 * actually played with. Deleting that file to "remove the cursor" would remove
 * all input and make the game unplayable -- so the cursor is gated out and the
 * touch path is left exactly as it was.
 *
 * Consequence, stated plainly: DOCKED MODE NOW HAS NO POINTER INPUT AT ALL.
 * There is no touchscreen when docked and no cursor any more, so menus cannot
 * be operated docked. Handheld is the only playable mode. Set this back to 1
 * to restore the cursors. */
#define PHI_VIRTUAL_CURSOR 0

/* ---- locale override ------------------------------------------------------
 * Report en/US to the game when the Switch system language is Simplified or
 * Traditional Chinese.
 *
 * Phigros' Chinese locales require a TapTap account login. That needs the real
 * Java TapTap SDK and a live network session; this port has neither, and the
 * in-game language menu is behind the login, so a Chinese console would reach a
 * sign-in screen with no way forward and no way to change language. English has
 * no such gate.
 *
 * The override is applied at the single point every locale query funnels
 * through (lang_code() in jni_fake.c), so language, country and the ISO3 and
 * display forms all agree -- a half-applied override that says "English" but
 * "CN" is worse than either answer on its own.
 *
 * Set to 0 to report the real system locale. That is the correct setting if the
 * login is ever solved; today it means a Chinese console cannot start the game. */
#define PHI_FORCE_ENGLISH_FOR_ZH 1

/* ---- NativeAudio device report -- THE LATENCY BUDGET ----------------------
 * Phigros drives chart audio and hitsounds through Exceed7 NativeAudio
 * (libnativeaudioe7.so) over OpenSL ES. Its Java Initialize() normally asks
 * Android for the device's native sample rate and frames-per-burst; there is
 * no Android here, so phigros_jni.c reports these instead.
 *
 * NativeAudio sizes its buffers from these numbers and Phigros derives its
 * audio offset from the latency NativeAudio reports. If they do not match what
 * the Switch is actually doing, EVERY CHART WILL FEEL OFFSET and no amount of
 * in-game calibration will fully fix it -- the error is in the reported
 * latency, not the user's reflexes.
 *
 * 48000 / 256 is a starting point, not a measured value. Tune on hardware:
 * play a chart you know well, and if you are consistently early or late by a
 * fixed amount, the buffer figure is the first thing to change. Expect to end
 * up somewhere between 128 and 512 frames. */
#define PHI_AUDIO_SAMPLE_RATE     48000
#define PHI_AUDIO_BUFFER_FRAMES     256
#define PHI_AUDIO_NATIVE_SOURCES     16   /* NativeAudio voice pool          */

/* ---- SDL mixer device ----------------------------------------------------
 * PHI_AUDIO_DEVICE_RATE is what the shim asks SDL for. Keep it equal to
 * PHI_AUDIO_SAMPLE_RATE: the boot log confirms the whole chain now runs at one
 * rate (plugin 48000 -> OpenSL players 48000 -> device 48000), so nothing
 * resamples. If they ever disagree, mix_player resamples and the log says so.
 *
 * PHI_AUDIO_CALLBACK_FRAMES is the mixer callback size and therefore the OUTPUT
 * latency floor. 1024 frames at 48 kHz is ~21 ms; 512 is ~10.7 ms.
 *
 * BACK AT 1024 after 512 was tried and music stuttered.
 *
 * Worth being precise about what that does and does not prove. The queue
 * counters showed no starvation at 512 ("short=0"), so the mixer was never
 * short of data -- but those counters cannot see the SDL device missing a
 * callback DEADLINE, and at 512 frames the audio thread has only ~10.7 ms to
 * run on a 3-core machine shared with ~25 engine threads. A missed deadline
 * sounds exactly like stutter and leaves every one of our counters at zero.
 *
 * It is not conclusive: music only started working in the same build that
 * lowered this, so there is no clean 1024 music sample to compare against.
 * 1024 is the value the whole lineage ships, so it is the right thing to be at
 * while a different bug is being chased. If music is clean here, 512 is worth
 * retrying -- and if "short=" and "dry=" are still 0 while it stutters, the
 * cause is deadline misses and the fix is more headroom, not less. */
#define PHI_AUDIO_DEVICE_RATE     PHI_AUDIO_SAMPLE_RATE

/* ---- FMOD mix rate -- DELIBERATELY LOWER, DO NOT "UNIFY" ------------------
 * What we report to Unity as AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE /
 * PROPERTY_OUTPUT_FRAMES_PER_BUFFER. Unity configures FMOD's software mixer
 * from these, so they set how hard FMOD has to work -- they are NOT a
 * description of the output device, and they are NOT PHI_AUDIO_SAMPLE_RATE.
 *
 * This looked like an inconsistency (24000 here, 48000 everywhere else) and was
 * "unified" to 48000 once. That reintroduced the stuttering audio immediately:
 *
 *     24000 Hz / 256 frames -> 10.7 ms deadline,  94 mixer wakeups/sec
 *     48000 Hz / 256 frames ->  5.3 ms deadline, 188 mixer wakeups/sec
 *
 * Halving the deadline while doubling the samples is about 4x the pressure on a
 * software mixer running on 3 cores shared with ~25 engine threads. FMOD misses
 * buffers, the queue starves, and it sounds like repeating/stuttering audio.
 *
 * The shim resamples 24000 -> 48000 on the way out, which costs far less than
 * asking FMOD to mix at twice the rate. CloverPit sets its device rate to 24000
 * for exactly this reason. Music quality is not the constraint here; keeping up
 * is. Raise it only if you have CPU headroom to spare and can verify no
 * stutter. */
#define PHI_FMOD_MIX_RATE   24000
#define PHI_FMOD_MIX_FRAMES 256

/* Compile-time tripwire. This value has been "tidied up" to match the device
 * rate once already, costing a build to stuttering audio each time, because the
 * two numbers look like they should agree and nothing stopped the edit. If you
 * genuinely want FMOD mixing at the device rate, delete this check deliberately
 * -- and then verify on hardware that music does not stutter. */
#if PHI_FMOD_MIX_RATE >= PHI_AUDIO_SAMPLE_RATE
#error "PHI_FMOD_MIX_RATE must stay BELOW PHI_AUDIO_SAMPLE_RATE -- see the comment above it. Raising it to the device rate brings back stuttering audio."
#endif
#define PHI_AUDIO_CALLBACK_FRAMES 1024

#include "nx_root.h"   /* nx_root(), nx_rp() -- DATA_ROOT/GAME_HOME are these */

#define CONFIG_NAME "config.txt"
#define LOG_NAME    (nx_rp("/debug.log"))

// Returned for getenv("HOME")/getpwuid()->pw_dir. Point it at the (writable)
// game data root instead of letting the engine deref a NULL passwd.
/* ---- data root: RESOLVED AT RUNTIME --------------------------------------
 * The loader works from any folder under /switch. GAME_FOLDER is only the
 * fallback used when the root cannot be detected (see nx_root.c); it is no
 * longer the thing that has to match your SD card.
 *
 * Use nx_root() / nx_rp("/sub/path") rather than these macros in new code.
 * DATA_ROOT and GAME_HOME are kept as aliases so the inherited substrate keeps
 * compiling, but they are now FUNCTION CALLS, not string literals -- so
 *     DATA_ROOT "/assets"        (concatenation)
 * no longer compiles and must be written
 *     nx_rp("/assets")
 * which is exactly the mechanical change that makes the root movable. */
#define DATA_ROOT_DEFAULT "sdmc:/switch/" GAME_FOLDER
#define GAME_HOME   (nx_root())
#ifndef DATA_ROOT
#define DATA_ROOT   (nx_root())
#endif

// flip to 1 (and rebuild) to get file logging (debug.log) for on-hardware debugging
/* File logging.
 *
 *   1 = ON  [shipped]. debug.log is written normally: boot trace, per-frame
 *           diagnostics, [gc]/[mem] notes and the [xd] crash dump.
 *
 *   0 = ABSOLUTE SILENCE. debug.log is never created -- not by a crash, not by
 *       a "note", not at all. Every logging entry point compiles to an empty
 *       stub, so there is no file, no formatting cost and no SD traffic.
 *
 * Note that 0 does NOT disable the watchdog thread: it is also the escape hatch
 * that undoes a wedged GC stop-the-world (round 101), so it always runs. */
/* RELEASE BUILD: verbose logging off.
 *
 * The crash handler still force-enables logging when it fires
 * (debugLogForceOn() in __libnx_exception_handler), so a fault still writes a
 * full [xd] dump to debug.log. What is gone is the per-frame and per-JNI-call
 * chatter, which on a long session was both a lot of SD writes and a real
 * source of jitter on a 3-core machine. Set back to 1 to diagnose. */
#define DEBUG_LOG 0

/* GC stop-the-world (round 100).
 *
 *   1 = CORRECT. Mutator threads are really paused while the collector marks.
 *       This is what a garbage collector requires, and without it the mark loop
 *       can read a half-published object and fault (round 99: klass == 0).
 *       The cost is real: the game's worker threads are stopped for the whole
 *       mark, so a large collection shows up as an occasional frame hitch.
 *
 *   0 = OLD BEHAVIOUR. Ack the suspend without pausing anyone. Smooth, and
 *       wrong -- this is the configuration that crashed ~20% of the time when
 *       starting a new game.
 *
 * Left ON: an occasional hitch is a better failure than a crash. Flip it to 0
 * if you would rather have the old behaviour back. */
#define PHI_GC_STOP_WORLD 1

/* Strip "gc-max-time-slice" from boot.config as it is served to the engine.
 *
 * That key puts Unity's collector in INCREMENTAL mode: marking is split across
 * many short slices with the mutators running in between, and the invariant is
 * held together by write barriers plus a correct stop-the-world for each slice.
 * This port's stop-the-world is not reliable enough for that -- it works for ~32
 * collections in 33 and bails on the rest (round 110) -- and every crash so far
 * has landed in the mark loop reading a klass of 0, which is what a broken
 * incremental invariant looks like.
 *
 * With the key removed the collector runs non-incremental: fewer, larger, atomic
 * collections, with no between-slice invariant to violate. Expect occasional
 * longer pauses in exchange.
 *
 * 0 restores the game's shipped setting. */
#define PHI_GC_NON_INCREMENTAL 1

/* High-volume per-operation traces. These were invaluable for the black-screen /
 * boot-hang triage but are catastrophic for load speed once the game runs: every
 * data.unity3d read/lseek and most mprot calls fflush two lines to the SD card, so
 * a synchronous scene load (~1700 bundle reads) takes minutes instead of seconds
 * and looks like a hang. Keep them OFF for normal play; flip to 1 to re-trace. */
#define TRACE_BUNDLE_IO 0   /* per-read/lseek trace of globalgamemanagers */
#define TRACE_MPROT     0   /* per-mprotect commit trace */

/* Per-frame / per-asset traces that dominate the log once the game runs:
 * the doFrame proxy line (every frame), [io] open (every asset, ~3x), and
 * the clock-stall beacon. Off = a readable log and far less SD I/O; flip to
 * 1 only to re-trace JNI proxy dispatch or asset open order. */
#define LOG_VERBOSE 0

/* Mutex ownership tracker (self-deadlock / holder naming). It takes a global
 * lock on every pthread_mutex op, which serialises the engine's mutex
 * traffic and reorders acquisition -- fine for a one-off deadlock hunt, but
 * it must be OFF for normal runs or it changes timing enough to hang boot.
 * Flip to 1 only to re-diagnose a mutex deadlock. */
#define MTXOWN_ENABLE 0

/* Force glFinish() before eglSwapBuffers on the first N presents, to work
 * around / localise the first-present hang (mesa blocking in its flush/
 * fence phase). 0 disables. A small number (a few frames) is enough to get
 * past the initial present without serialising steady-state rendering. */
/* round 63: adaptive futex re-poll floor. 250us gives fast recovery of
 * silent-writer waits (the async-load bottleneck) while backoff keeps idle
 * threads cheap. Lower = faster loads but more wakeups. */
#define PHI_FUTEX_HOP_MIN 250000ULL
/* round 70: re-poll CEILING. Handoffs whose wake never arrives directly cost
 * one tick of this, so it sets the load speed. 16ms was the old value and is
 * why loading crawled; 1ms is ~16x faster. Lower = faster loads, more CPU. */
#define PHI_FUTEX_HOP_MAX 1000000ULL
/* round 64: vsync/Choreographer pulse period. 16ms == ~60fps cap; the load
 * is frame-gated so a shorter period renders (and loads) faster. Delta-time
 * is hooked so game speed is unchanged. */
#define PHI_VSYNC_PERIOD_NS 16000000ULL
#define PHI_SWAP_FINISH_N 8

/* ---- libil2cpp hook gates (see patches/patch_sources.py) -----------------
 * libil2cpp is GAME CODE: every offset into it is specific to one build of one
 * game. The PvZ core hardcodes hooks at PvZ's offsets. Both are OFF here because
 * neither was re-derived for Phigros; turning one on without re-deriving it
 * first will patch unrelated functions. Symptoms and method: PORTING sec 6.    */
#define PHI_HAVE_TIME_HOOKS 0   /* not derived for Phigros -- see nx_patch_phigros.h */
#define PHI_FORCE_SPLASH_FINISH 0  /* Fruit-Ninja-specific; not derived here */
/* round 68: async-load integration budget, ms per frame. Unity default is
 * 4ms (High would be 50). Bigger = faster scene loads, fewer frames during
 * loading. 0 disables the patch. */
#define PHI_PRELOAD_BUDGET_MS 4
/* round 75: NOP UpdatePreloading's early-exit branch so the main thread keeps
 * retrying SingleStep for the whole budget instead of yielding the frame the
 * moment the integrate queue is briefly empty. 0 disables. */
#define PHI_PRELOAD_NO_EARLY_EXIT 1
/* round 69: per-asset "[io] DATA open" trace. Costs an extra fstat plus a
 * log line for each of the ~2200 assets loaded, so keep it off by default. */
#define PHI_TRACE_DATA_IO 0
/* round 79: build+mount the Subway-Surfers-style asset pack. First boot
 * packs assets/bin/Data into one file; later boots mount it. 0 = off. */
/* Fold the loose assets/ tree into one file + index on first boot.
 *
 * ON for Phigros. This game ships 2958 loose files (2.79 GB): 2514 Addressables
 * bundles plus 441 files under bin/Data. Every one of those is an open() that
 * costs a FAT directory traversal on SD, and that cost is what makes an
 * unpacked first scene load take tens of minutes. The pack turns each open into
 * an index lookup.
 *
 * The build is non-destructive and verify-before-commit: it writes
 * assets.nxpack.tmp + .idx.tmp, loads them BACK and checks them, renames into
 * place, and only then deletes the loose tree. A failed or interrupted build
 * leaves your assets untouched.
 *
 * It needs ~2.8 GB free on the SD card while building, because the pack and the
 * loose tree briefly coexist. Check before the first boot. If space is short,
 * set this to 0 and the loader runs from loose files -- slower, but it works.
 *
 * FAT32 note: the pack is a single ~2.8 GB file, under FAT32's 4 GB per-file
 * limit but not by much. If you later add song packs, use exFAT. */
#define PHI_ASSET_PACK 1
#define PHI_BYPASS_UNITY_SPLASH 0  /* not derived for Phigros -- see nx_patch_phigros.h */
/* JNI approximation ledger (round 130). Records every JNI call answered by a
 * catch-all -- empty string, NULL object, 0, no-op -- deduped and counted, and
 * marks the ones Unity reads back via ExceptionCheck. Android raises at this
 * boundary and we cannot; this is the half of that which costs nothing. The
 * ledger is reprinted in full on any crash dump, so a fault log now carries
 * "here is everything we faked" instead of needing a separate run.
 * Set to 0 for absolute silence (DEBUG_LOG 0 already suppresses the output). */
#define PHI_JNI_LOUD 1

/* Poison freed newlib blocks with 0xDE (no quarantine, nothing retained).
 * Makes use-after-free deterministic instead of layout-dependent -- see the
 * comment in nx_alloc.c's nx_free_inner. Costs one memset per free; set to 0 to
 * restore the previous behaviour exactly. */
#define PHI_POISON_FREE 1


/* Diagnostic ONLY: call il2cpp_gc_disable() after init so no collection ever
 * runs. Answers "is the corruption premature reclamation?" in one session.
 * Memory grows unbounded -- never ship with this set to 1. */
#define PHI_GC_DISABLE 0

/* The on-device asset-pack payload verifier is GONE (round 142). It ran once,
 * printed "verify OK: payload matches header (b3e645de7a86ae28)", and that is
 * recorded -- it had no business costing a 255 MB blocking read on the boot
 * path. The same check now lives in tools/verify_pack.py, run on a PC. */

#define PHI_HAVE_GC_BRIDGE  1   /* DERIVED for Phigros: 2 pthread_kill callers, ack sem confirmed 3 ways */

extern int screen_width;
extern int screen_height;

/* ----------------------------- Language ----------------------------------
 * NOT INHERITED FROM PvZ -- re-derived for this game, and it works differently.
 *
 * PvZ's build read its locale as a STRING from an AndroidJavaClass, so its
 * config mapped 1/2 onto the literals "en"/"zh" returned by jni_fake's
 * getLanguage(). Phigros does not do that. Its managed code calls
 *
 *     UnityEngine.Application::get_systemLanguage()
 *
 * which returns Unity's SystemLanguage ENUM. The engine populates that at init
 * from the Java locale, so jni_fake's getLanguage() still feeds it -- but the
 * game compares against an enum, not against our string, so there is no
 * game-specific token to guess.
 *
 * Practical consequence: LANG_AUTO is the only value with confirmed meaning.
 * The overrides below simply force the locale jni_fake reports; verify on
 * hardware which SystemLanguage the engine derives before trusting them.
 *
 * (The binary also carries I18N.CJK/MidEast/Other/Rare/West and RTLTMPro, so
 * the title has broad language coverage including right-to-left. The ar-*
 * tokens visible in libil2cpp are mscorlib CULTURE TABLES from I18N.MidEast,
 * not a list of shipped game languages -- do not read them as one.)        */
#define LANG_AUTO 0   /* follow the Switch system language -- recommended */
#define LANG_EN   1   /* force en */
#define LANG_ZH   2   /* force zh -- retained from the base; UNVERIFIED here */

/* ORIENTATION -- UNRESOLVED, AND YOU MUST CHECK THIS BEFORE FIRST BOOT.
 *
 * PvZ removed the base's TATE/portrait path because that game is landscape-only.
 * We have NOT confirmed Phigros's orientation: its managed code contains no
 * ScreenOrientation manipulation and no landscape/portrait strings, so the APK's
 * AndroidManifest.xml (android:screenOrientation) is authoritative. Check it.
 *
 *   landscape -> nothing to do; this build matches PvZ and is correct as-is.
 *   portrait  -> you must restore the Zookeeper TATE compositor-rotation path,
 *                AND revisit nx_pointer, which maps the touch panel to render
 *                space with a straight stretch and applies no rotation. Aiming
 *                will be wrong otherwise -- silently, which is the worst kind.
 *
 * The `portrait` knob is retired here only because it is retired upstream; it
 * is NOT a statement that this game is landscape.                          */
typedef struct {
  int handheld_res;   /* render height in handheld mode: 720 or 1080 */
  int docked_res;     /* render height when docked:      720 or 1080 */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
