/* BombSquad Explodinary for Nintendo Switch.
 *
 * Loads the arm64-v8a libmain.so from the player's own APK into a minimal
 * Android environment and drives the lifecycle the Java activity normally
 * would: JNI_OnLoad, surface creation, the init-cycle handshake, then a
 * frame loop.
 *
 * Thread split follows the Android app: one thread owns engine start-up and
 * the lifecycle callbacks, while the thread holding the GL context presents
 * frames.
 */

#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bridge.h"
#include "config.h"
#include "error.h"
#include "gl_shim.h"
#include "guest.h"
#include "imports.h"
#include "input.h"
#include "io_shim.h"
#include "jni_env.h"
#include "opensles.h"
#include "patch.h"
#include "net_shim.h"
#include "pipe_shim.h"
#include "port_config.h"
#include "pthread_shim.h"
#include "setup.h"
#include "shims.h"
#include "so_util.h"
#include "sys_shim.h"
#include "text.h"
#include "util.h"
#include "video.h"

/* The guest image is staged in this reservation carved out of the heap, then
 * donated to the kernel as code memory. */
static void *g_so_region;
static size_t g_so_region_size;

/* ------------------------------------------------------------ heap layout */

void __libnx_initheap(void) {
  void *base;
  size_t size = 0;

  if (envHasHeapOverride()) {
    base = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    u64 available = 0, used = 0;
    svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (available > used + 0x200000) size = (available - used - 0x200000) & ~0x1FFFFFull;
    if (!size) size = 0x2000000 * 8;
    if (R_FAILED(svcSetHeapSize(&base, size)))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 4) so_zone = size / 4;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)base;
  fake_heap_end = (char *)base + (size - so_zone);

  g_so_region = (void *)ALIGN_MEM((uintptr_t)base + (size - so_zone), 0x1000);
  g_so_region_size = so_zone - 0x1000;
}

/* ------------------------------------------------------- service start-up */

static void require_syscalls(void) {
  if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78) ||
      !envIsSyscallHinted(0x73) || envGetOwnProcessHandle() == INVALID_HANDLE) {
    fatal_error(
        "This build needs code-memory access.\n\n"
        "  Launch it as a full application: hold R while starting a game\n"
        "  from the console's home menu, then pick BombSquad: Explodinary Rebombed in hbmenu.");
  }
}

static void require_memory(void) {
  u64 available = 0, used = 0;
  svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  trace("memory: %llu MB total, %llu MB used at startup",
        (unsigned long long)(available / (1024 * 1024)),
        (unsigned long long)(used / (1024 * 1024)));

  /* Album-applet mode leaves far too little for the engine plus Python. */
  if (available < 512ull * 1024ull * 1024ull) {
    fatal_error(
        "Only %llu MB of memory is available.\n\n"
        "  Launch with a game override: hold R while starting a game from\n"
        "  the home menu, then pick BombSquad: Explodinary Rebombed in hbmenu.",
        (unsigned long long)(available / (1024 * 1024)));
  }
}

/* --------------------------------------------------------- guest loading */

static void load_guest(void) {
  startup_status_update("Loading the game library");

  const int result = so_load(&guest_module, SO_PATH, g_so_region, g_so_region_size);
  if (result < 0) {
    if (result == -5)
      fatal_error("%s is not an arm64-v8a library.\n\n"
                  "  Extract lib/arm64-v8a from your APK, not another ABI.",
                  SO_PATH);
    if (result == -3)
      fatal_error("%s does not fit in the reserved %u MB code region.", SO_PATH,
                  (unsigned)(g_so_region_size / (1024 * 1024)));
    fatal_error("Could not load %s (error %d).", SO_PATH, result);
  }

  startup_status_update("Relocating the game library");
  so_relocate(&guest_module);
  patch_verify_build_early();

  startup_status_update("Binding the Android environment");
  so_resolve(&guest_module, dynlib_functions, (int)dynlib_function_count);

  startup_status_update("Mapping executable memory");
  so_finalize(&guest_module);
  so_flush_caches(&guest_module);

  patch_verify_build();
  patch_apply();

  startup_status_update("Running static initializers");
  /* Constructors run guest code, so this thread needs its bionic TLS block
   * before the first one executes. */
  bionic_thread_adopt();
  so_execute_init_array(&guest_module);
  so_free_temp(&guest_module);

  trace("guest module ready at %p", guest_module.load_virtbase);
}

/* --------------------------------------------------------- engine startup */

typedef int (*jni_on_load_fn)(void *vm, void *reserved);

static Thread g_lifecycle_thread;
static volatile int g_engine_ready;

/* ------------------------------------------------------- suspend / resume
 *
 * The engine insists that suspending, resuming and going active or inactive
 * all happen on the thread it considers its main one -- the lifecycle thread
 * here, not the one running the frame loop. So the frame loop, which is what
 * actually sees the console's focus changing, only records what it wants and
 * the lifecycle thread carries it out. */

static volatile int g_want_active = 1;
static volatile int g_want_running = 1;
static volatile int g_restart_requested;

/* Written only by the lifecycle thread. The frame loop reads the running one
 * to know whether there is any point asking for a frame. */
static volatile int g_engine_running = 1;
static int g_engine_active = 1;

static bool engine_running(void) {
  return __atomic_load_n(&g_engine_running, __ATOMIC_ACQUIRE) != 0;
}

static void engine_stop(void) {
  if (!g_engine_running) return;
  /* Inactive first: stopping waits for the engine to finish telling itself
   * it went inactive, and complains if that never happened. */
  if (g_engine_active) {
    guest_set_active(false);
    g_engine_active = 0;
  }
  /* And the frame loop stands down before the engine parks its threads
   * rather than after -- a draw once the logic thread is parked just sits
   * out the engine's frame timeout, a whole second per frame, during exactly
   * the window the stop is waiting to finish in. */
  __atomic_store_n(&g_engine_running, 0, __ATOMIC_RELEASE);
  guest_set_running(false);
}

static void engine_start(void) {
  if (g_engine_running) return;
  guest_set_running(true);
  __atomic_store_n(&g_engine_running, 1, __ATOMIC_RELEASE);
}

static void apply_app_state(void) {
  const int want_running = __atomic_load_n(&g_want_running, __ATOMIC_ACQUIRE);
  const int want_active = __atomic_load_n(&g_want_active, __ATOMIC_ACQUIRE);

  if (!want_running) {
    if (g_engine_running) {
      engine_stop();
      trace("engine suspended");
    }
    return;
  }

  if (!g_engine_running) {
    engine_start();
    trace("engine resumed");
  }

  /* Waking from a console sleep, nothing was ever told to stop -- the whole
   * process was simply frozen -- but every socket it held died with the
   * network stack, and coming out of a stop is the only thing that makes the
   * engine open a fresh listener. So it gets put through one. */
  if (__atomic_exchange_n(&g_restart_requested, 0, __ATOMIC_ACQ_REL)) {
    trace("cycling the engine so it rebuilds its sockets");
    engine_stop();
    engine_start();
    /* And a nudge for anything waiting on the network, which the engine
     * treats as a change rather than a state. */
    guest_net_avail_changed(false);
    guest_net_avail_changed(true);
  }

  if (want_active != g_engine_active) {
    guest_set_active(want_active != 0);
    g_engine_active = want_active;
  }
}

/* Engine start-up and the lifecycle callbacks all run here, matching the
 * Android app's non-render thread. The engine drives its own start-up: it
 * asks for init cycles until it reports that it is done. */
static void lifecycle_entry(void *arg) {
  (void)arg;
  bionic_thread_adopt();

  trace("nativeInit call");
  guest_init();
  trace("nativeInit returned");

  bool activated = false;
  while (!activated) {
    while (!bridge_init_complete() && bridge_take_init_cycle_request()) {
      trace("nativeInitCycle call");
      guest_init_cycle();
      trace("nativeInitCycle returned");
    }

    if (bridge_init_complete()) {
      /* Controllers are announced before the engine goes live so the first
       * frame already has its input devices. */
      guest_enable_input();
      input_register_pads();

      guest_set_running(true);
      guest_set_active(true);
      guest_net_avail_changed(true);
      activated = true;
      __atomic_store_n(&g_engine_ready, 1, __ATOMIC_RELEASE);
      trace("engine activated");
      break;
    }
    svcSleepThread(1000000ull); /* 1 ms */
  }

  /* This thread stays on as the engine's main thread, replaying the calls
   * the Java UI thread would normally run. It blocks on a condition
   * variable rather than polling: it shares a core with the render loop at
   * the same priority, so a spin here costs frames. */
  while (1) {
    apply_app_state();

    char id[24];
    if (!bridge_wait_main_thread_call(id, sizeof id, 50)) continue;

    const u64 started = armGetSystemTick();
    guest_handle_command2("MAIN_THREAD_CALL", id);
    const u64 elapsed_ms =
        (armGetSystemTick() - started) * 1000ull / armGetSystemTickFreq();

    /* Only the first few, plus anything slow enough to cost frames. */
    static unsigned traced;
    if (traced < 16 || elapsed_ms >= 50) {
      traced++;
      trace("main-thread call %s took %llu ms", id,
            (unsigned long long)elapsed_ms);
    }
  }
}

static bool engine_ready(void) {
  return __atomic_load_n(&g_engine_ready, __ATOMIC_ACQUIRE) != 0;
}

/* The console will run the CPU at its top clock on request, and drops the
 * GPU to its minimum in exchange. That is the right trade while an APK is
 * being unpacked or the engine is starting: both are bound by the processor
 * and the card, and what little is on screen is a progress line. It is the
 * wrong trade the moment the game starts drawing for real, so it is given
 * back as soon as the engine says it is up. */
static bool g_cpu_boosted;

static void set_cpu_boost(bool boosted) {
  static bool complained;
  if (boosted == g_cpu_boosted) return;

  const Result rc = appletSetCpuBoostMode(
      boosted ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
  if (R_FAILED(rc)) {
    /* Not every way of launching allows this, and it is only ever a speed-up,
     * so a refusal is worth one line and nothing more. */
    if (!complained) {
      complained = true;
      trace("cpu boost unavailable (0x%x); loading at the standard clock", rc);
    }
    return;
  }
  g_cpu_boosted = boosted;
  trace("cpu boost %s", boosted ? "on for loading" : "off");
}

/* Loading is not finished when the engine reports itself up: it still has a
 * Python library to byte-compile and asset caches to fill, and that work runs
 * on the thread the renderer waits for. So the clock is not handed back at
 * that point; it is held until a full window comes in at a healthy frame
 * rate, which is what actually says loading is over. Handing it back at
 * engine-ready and waiting for a bad window to ask for it again cost a five
 * second detection gap, and the worst stall lands inside it.
 *
 * If the frame rate collapses again later, the same reading applies: a window
 * this far below the refresh rate means the processor is the bottleneck, not
 * the graphics clock -- boost mode's low clock is nowhere near being why a
 * frame took a second -- so the clock goes back up until it recovers.
 *
 * The budget is what keeps that from being a trap: if the reading is ever
 * wrong, the most it can cost is this much time at a low graphics clock,
 * once, and then never again this session. */
#define BOOST_RESCUE_BELOW 90    /* frames in five seconds; about 18 a second */
/* Deliberately well short of the refresh rate. The graphics clock boost mode
 * leaves is low enough that the full rate is out of reach while it is on, so
 * asking for the full rate before letting go meant never letting go: the
 * budget ran out every time instead. This is comfortably above the stall it
 * exists to end and comfortably below what the low clock can still manage. */
#define BOOST_RELEASE_ABOVE 200  /* about 40 a second: the stall is over */
/* Covers the whole of a first-launch load and still bounds the damage if the
 * reading is ever wrong: this much time at a low graphics clock, once, and
 * never again that session. */
#define BOOST_RESCUE_BUDGET_MS 90000

/* ------------------------------------------------------------- frame loop */

/* Only a real suspend should reach this. The game does stall for seconds at
 * a time on its own -- a level load, or the SD card going away to think --
 * and misreading one of those as a sleep would cycle the engine underneath a
 * player who was in the middle of something. Nobody puts the console down
 * for less than this, and an actual sleep runs to minutes. */
#define SUSPENDED_GAP_MS 20000

/* How long to let the console find its network again before rebuilding
 * sockets anyway. */
#define NETWORK_RETURN_WAIT_MS 15000

static void run_frame_loop(void) {
  /* Keep the console awake while the engine loads assets and nobody has
   * touched a controller yet. */
  bool keeping_awake = true;
  appletSetMediaPlaybackState(true);

  bool focused = true;
  u64 last_iteration = armGetSystemTick();
  bool waiting_for_network = false;
  u64 resume_started = 0;
  u64 frames = 0;
  u64 frames_at_report = 0;
  u64 last_report = armGetSystemTick();
  u64 rescue_budget_ms = BOOST_RESCUE_BUDGET_MS;
  /* Which phase the wall-clock time goes into, so a stall names itself. */
  u64 input_ticks = 0, draw_ticks = 0, swap_ticks = 0;

  while (appletMainLoop()) {
    const u64 frequency = armGetSystemTickFreq();
    const u64 iteration = armGetSystemTick();
    const u64 gap_ms = (iteration - last_iteration) * 1000ull / frequency;
    last_iteration = iteration;

    if (engine_ready()) {
      /* Pressing HOME takes the app out of focus. Stopping the engine there,
       * the way the Android build does on pause, keeps it from resuming
       * mid-explosion -- and it is also what makes it rebuild its sockets on
       * the way back. */
      const bool was_focused = focused;
      const bool now_focused = appletGetFocusState() == AppletFocusState_InFocus;
      if (now_focused != focused) {
        focused = now_focused;
        __atomic_store_n(&g_want_active, focused ? 1 : 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_want_running, focused ? 1 : 0, __ATOMIC_RELEASE);
        trace("focus %s", focused ? "gained" : "lost");
      }

      /* The console can freeze the whole process between two iterations --
       * sleeping is the usual reason -- and nothing announces it afterwards.
       * Only the clock shows it happened, and everything the network stack
       * was holding is gone by then. */
      if (was_focused && focused && !waiting_for_network &&
          gap_ms >= SUSPENDED_GAP_MS) {
        trace("%llu ms passed between frames; the console was suspended",
              (unsigned long long)gap_ms);
        waiting_for_network = true;
        resume_started = iteration;
      }

      if (waiting_for_network) {
        const u64 waited_ms = (iteration - resume_started) * 1000ull / frequency;
        /* Rebuilding sockets before the console has rejoined its network
         * would only fail, so it gets a chance to come back first. */
        if (net_have_address() || waited_ms >= NETWORK_RETURN_WAIT_MS) {
          waiting_for_network = false;
          __atomic_store_n(&g_restart_requested, 1, __ATOMIC_RELEASE);
        }
      }
    }

    /* There is nothing to draw while the engine is stopped, and asking it to
     * render anyway just waits out its frame timeout. */
    if (!engine_running()) {
      svcSleepThread(16000000ull);
      if (bridge_quit_requested()) break;
      continue;
    }

    const u64 phase_start = armGetSystemTick();
    input_poll();
    const u64 after_input = armGetSystemTick();

    if (video_take_resize()) {
      guest_surface_changed(video_width(), video_height());
      trace("surface resized to %dx%d", video_width(), video_height());
    } else if (bridge_take_surface_refresh()) {
      guest_surface_changed(video_width(), video_height());
    }

    guest_draw_frame();
    const u64 after_draw = armGetSystemTick();
    if (!frames) trace("first frame drawn");
    frames++;
    video_swap();
    const u64 after_swap = armGetSystemTick();

    input_ticks += after_input - phase_start;
    draw_ticks += after_draw - after_input;
    swap_ticks += after_swap - after_draw;

    /* A black screen is ambiguous without this: it separates "stuck before
     * the first frame" from "running but drawing nothing". */
    const u64 now = armGetSystemTick();
    if (now - last_report >= armGetSystemTickFreq() * 5) {
      const u64 to_ms = armGetSystemTickFreq() / 1000;
      const u64 window_frames = frames - frames_at_report;
      const u64 window_ms = (now - last_report) / to_ms;
      trace("frame %llu (%llu in 5s, engine %s) input=%llums draw=%llums swap=%llums",
            (unsigned long long)frames,
            (unsigned long long)window_frames,
            engine_ready() ? "ready" : "still starting",
            (unsigned long long)(input_ticks / to_ms),
            (unsigned long long)(draw_ticks / to_ms),
            (unsigned long long)(swap_ticks / to_ms));

      /* The budget only starts running once the engine is up. Before that
       * there is no frame rate worth reading and nothing to decide. */
      if (g_cpu_boosted && engine_ready()) {
        rescue_budget_ms -= window_ms < rescue_budget_ms ? window_ms
                                                         : rescue_budget_ms;
      }
      if (engine_ready()) {
        if (g_cpu_boosted &&
            (window_frames >= BOOST_RELEASE_ABOVE || rescue_budget_ms == 0)) {
          set_cpu_boost(false);
        } else if (!g_cpu_boosted && rescue_budget_ms > 0 &&
                   window_frames < BOOST_RESCUE_BELOW) {
          trace("only %llu frames in %llums; giving loading the clock back",
                (unsigned long long)window_frames,
                (unsigned long long)window_ms);
          set_cpu_boost(true);
        }
      }

      frames_at_report = frames;
      input_ticks = draw_ticks = swap_ticks = 0;
      last_report = now;
    }

    if (bridge_quit_requested() || input_exit_requested()) break;

    if (keeping_awake && engine_ready()) {
      appletSetMediaPlaybackState(false);
      keeping_awake = false;
    }
  }
  if (keeping_awake) appletSetMediaPlaybackState(false);
  set_cpu_boost(false);
}

/* Socket calls travel over a small pool of service sessions, and one call
 * occupies a session for its whole duration. The engine keeps sockets busy on
 * several threads at once -- the UDP listener, the network writer, and
 * whatever CPython is doing for the account and the server browser -- so the
 * default of three leaves them taking turns. */
static void socket_init(void) {
  SocketInitConfig config = *socketGetDefaultInitConfig();
  config.num_bsd_sessions = 8;
  if (R_SUCCEEDED(socketInitialize(&config))) {
    trace("sockets up with %u sessions", config.num_bsd_sessions);
    return;
  }
  /* The console decides how many sessions it will hand out, so an ambitious
   * request has to be able to fall back rather than leave the game offline. */
  trace("could not open %u socket sessions; falling back to the default",
        config.num_bsd_sessions);
  if (R_FAILED(socketInitializeDefault())) trace("socket init failed");
}

/* ------------------------------------------------------------------- main */

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  trace_reset();
  trace("explodinary starting");
  startup_status_begin("Starting up");

  /* Everything from here to the engine reporting itself up is loading. */
  set_cpu_boost(true);

  require_syscalls();
  require_memory();

  /* The time service has to be up before sys_shim_init measures the
   * console's UTC offset from it. */
  setsysInitialize();
  timeInitialize();

  io_shim_init();
  pipe_shim_init();
  shims_init();
  sys_shim_init();
  shims_sync_timezone();
  setup_prepare_data();
  port_config_load();
  /* After both: the settings say whether to keep the archive, and the data
   * it produced has been checked by now. */
  setup_remove_installed_apk();

  /* Sockets have to be up before the engine's first init cycle, which opens
   * its UDP listener. */
  socket_init();

  if (!text_init())
    trace("dynamic text rendering unavailable; atlas fonts still work");

  /* The engine reads these before its first frame. */
  bionic_setenv("BA_UI_SCALE", port_config_ui_scale_name(), 1);
  bionic_setenv("BA_DEVICE_NAME", port_config()->device_name, 1);
  /* Oboe would otherwise reach for AAudio; this port implements OpenSL ES. */
  bionic_setenv("BA_OBOE_USE_OPENSLES", "1", 1);
  if (!port_config()->udp_listener) {
    bionic_setenv("BA_NO_UDP_LISTENER", "1", 1);
    trace("UDP listener disabled by config");
  }

  jni_init();
  load_guest();
  guest_resolve_entrypoints();

  startup_status_update("Starting the engine");
  jni_on_load_fn JNI_OnLoad = (jni_on_load_fn)so_symbol(&guest_module, "JNI_OnLoad");
  if (!JNI_OnLoad) fatal_error("This libmain.so has no JNI_OnLoad.");
  const int jni_version = JNI_OnLoad(&jni_vm, NULL);
  if (jni_version != JNI_VERSION_1_4 && jni_version != JNI_VERSION_1_6)
    fatal_error("JNI_OnLoad failed (returned 0x%08x).", jni_version);
  trace("JNI_OnLoad returned 0x%08x", jni_version);

  startup_status_update("Handing the screen to the engine");
  startup_status_end();

  video_init();
  error_screen_taken();
  gl_shim_init();
  input_init();

  /* One step below the render thread's priority: engine start-up work
   * should never come at the cost of a frame. */
  if (R_FAILED(threadCreate(&g_lifecycle_thread, lifecycle_entry, NULL, NULL,
                            1024 * 1024, 0x2D, -2)) ||
      R_FAILED(threadStart(&g_lifecycle_thread))) {
    fatal_error("Could not start the engine lifecycle thread.");
  }

  guest_surface_created();
  guest_surface_changed(video_width(), video_height());

  run_frame_loop();

  trace("shutting down");
  opensles_shutdown();
  video_shutdown();
  socketExit();
  return 0;
}
