/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <nuttx/board.h>
#include <nuttx/kthread.h>
#include <nuttx/fs/fs.h>
#include <nuttx/semaphore.h>
#include <nuttx/video/fb.h>

#include <arch/board/board.h>

#include "arm_internal.h"

#include "bk7258_psram.h"
#include "bk7258_ramdisk.h"
#include "bk7258_gc9d01_fb.h"
#include "hardware/bk7258_mbox.h"
#include "bk7258_wifi.h"
#include "bk7258_status_screen.h"
#include "bk7258_net_autostart.h"

int weak_function velasight_autostart(void)
{
  return -ENOSYS;
}

#ifdef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
static sem_t g_velasight_wifi_ready = SEM_INITIALIZER(0);
static int g_velasight_wifi_result = -EINPROGRESS;
static bool g_velasight_display_revealed;

int bk7258_wifi_wait_ready(void)
{
  int ret = nxsem_wait_uninterruptible(&g_velasight_wifi_ready);

  if (ret < 0)
    return ret;

  return g_velasight_wifi_result;
}

void bk7258_display_reveal(void)
{
  if (!g_velasight_display_revealed)
    {
      bk7258_gc9d01_backlight(true);
      g_velasight_display_revealed = true;
      printf("velasight: LVGL first frames ready, backlight on\n");
    }
}
#endif

#ifdef CONFIG_BK7258_TRNG
#  include "bk7258_trng.h"
#endif

#ifdef CONFIG_BK7258_BLUETOOTH
#  include "bk7258_bt.h"
#endif

#ifdef CONFIG_BK7258_BT_GATT_TEST
int bk7258_bt_gatt_test_initialize(void);
#endif

#ifdef CONFIG_BK7258_AUDIO
#  include "bk7258_audio_bringup.h"
#endif

#ifdef CONFIG_BK7258_JPEG_ENC
#  include "bk7258_jpeg_enc.h"
#endif

#define BK7258_LINK_WAIT_MS 8000u

int bk7258_pwc_start(void);
int bk7258_motor_setup(void);
int bk7258_power_key_motor_start(void);
int bk7258_gc9d01_test(int argc, char **argv);
int bk7258_camera_initialize(void);

#ifdef CONFIG_BK7258_BLUETOOTH
/****************************************************************************
 * Name: bk7258_bt_bringup
 *
 * Description:
 *   Bluetooth transport, host registration and (optionally) the GATT
 *   fixture.  Deliberately NOT called from bk7258_bringup(): it only needs
 *   the mailbox and PWC, which bk7258_bringup() has already finished, and
 *   everything it does happens before the panels would otherwise light up.
 *   Registering the host cost the boot greeting its head start, and a radio
 *   that comes up a moment later is not something a user can see, whereas a
 *   screen that stays dark is.  Called from board_late_initialize() after
 *   the display instead.
 *
 *   Best-effort by the same argument: a Bluetooth failure used to abort
 *   bring-up and take the console, camera and panels down with it.
 *
 ****************************************************************************/

static void bk7258_bt_bringup(void)
{
  int ret;

  ret = bk7258_bt_transport_initialize();
  if (ret < 0)
    {
      printf("Bluetooth transport initialization failed, error=%d\n", ret);
      return;
    }

#  ifdef CONFIG_BK7258_BT_RAW_SELFTEST
  ret = bk7258_bt_raw_selftest_run();
  if (ret < 0)
    {
      printf("Bluetooth raw self-test failed, error=%d\n", ret);
    }
#  else
  ret = bk7258_bt_driver_register();
  if (ret < 0)
    {
      printf("Bluetooth Host registration failed, error=%d\n", ret);
      bk7258_bt_transport_dump_stats();
      bk7258_mailbox_dump_stats();
      return;
    }

#  ifdef CONFIG_BK7258_BT_GATT_TEST
  ret = bk7258_bt_gatt_test_initialize();
  if (ret < 0)
    {
      printf("Bluetooth GATT fixture failed, error=%d\n", ret);
    }
#  endif
#  endif
}
#endif

#ifdef CONFIG_BK7258_SDIO
int bk7258_mmcsd_initialize(void);
int bk7258_mmcsd_schedule(unsigned int delay_ms);
#endif

int bk7258_bringup(void)
{
  uint32_t button_count;
  int ret;

  button_count = board_button_initialize();
#ifdef CONFIG_FS_PROCFS
  /* Mount procfs first so that ps, free and the other informational NSH
   * commands work even if a later bring-up step returns early. A mount
   * failure is reported but never blocks bring-up: procfs is not required
   * by the console data path.
   */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      printf("failed to mount procfs at /proc, error=%d\n", ret);
    }
#endif

  ret = bk7258_motor_setup();
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_mailbox_wait_link_ready(BK7258_LINK_WAIT_MS);
  if (ret < 0)
    {
      printf("mailbox UART0 STATE probe failed, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      bk7258_mbox_uart_dump_stats();
      return ret;
    }

  printf("mailbox UART0 link ready\n");

  ret = bk7258_ipc_heartbeat_start();
  if (ret < 0)
    {
      printf("HW_CTRL power-up service failed, error=%d\n", ret);
      bk7258_mailbox_dump_stats();
      return ret;
    }

  ret = bk7258_pwc_start();
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_BK7258_SDIO
  ret = bk7258_mmcsd_schedule(CONFIG_BK7258_SDIO_AUTOINIT_DELAY_MS);
  if (ret < 0)
    {
      printf("SD-NAND delayed initialization schedule failed, error=%d\n",
             ret);
    }
#endif
#if defined(CONFIG_BK7258_WIFI) && \
    !defined(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  ret = bk7258_wifi_initialize();
  if (ret < 0)
    {
      return ret;
    }
#endif

  printf("button GPIOs configured, count=%lu\n",
         (unsigned long)button_count);
  printf("PWM device registered at /dev/pwm0\n");
  printf("mailbox transport, PWC and CPU1 ready handshake complete\n");
  printf("heartbeat worker started, interval=2000 ms\n");
#ifdef CONFIG_BK7258_PSRAM
  if (bk7258_psram_is_online())
    {
      printf("PSRAM online, base=0x60000000 size=0x01000000 "
             "RW/XN/non-cacheable\n");

      /* PSRAM-backed /dev/ram0: the only storage on this board big enough
       * to hold a captured frame as a file (tmpfs lives in the ~300KB
       * kernel heap, one 640x480 YUYV frame is 614400 bytes).
       */

      (void)bk7258_ramdisk_initialize();
      /* printf("PSRAM CP heap 0x60700000..0x6071ffff reserved\n"); */
      /* bk7258_psram_dump(); */
    }
  else
    {
      printf("PSRAM unavailable; media services must remain disabled\n");
    }
#endif

#ifdef CONFIG_BK7258_TRNG
  /* Seed the kernel entropy pool from the hardware TRNG.  Best-effort: a
   * probe failure leaves the pool with interrupt timing only, which is
   * weaker but still functional, and must not stop bring-up.  /dev/random
   * itself was registered earlier, by drivers_initialize().
   */

  ret = bk7258_trng_initialize();
  if (ret < 0)
    {
      printf("trng: initialize failed, error=%d "
             "(entropy pool falls back to IRQ timing)\n", ret);
    }
#endif

  ret = bk7258_power_key_motor_start();
  if (ret < 0)
    {
      return ret;
    }

  return 0;
}

/****************************************************************************
 * Name: nand_config_loader
 *
 * Description:
 *   Injects the stored LLM settings into the agent's config file and joins
 *   the stored Wi-Fi network.
 *
 *   A task rather than part of bring-up because association blocks on the CP,
 *   and blocking bring-up stops the mailbox from being serviced: the
 *   heartbeat lapses and the CP's 8-second watchdog resets the chip.  Here a
 *   slow answer only delays this one task.
 *
 *   The store is memory-only, so on a cold boot there is nothing to inject
 *   and nothing to join; both calls are no-ops until something is set with
   *   the provisioning web page.
 *
 ****************************************************************************/

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
static int nand_config_loader(int argc, FAR char *argv[])
{
  UNUSED(argc);
  UNUSED(argv);

  /* The provisioning file is the sole product configuration source.  KVDB is
   * deprecated and intentionally not initialized or read here. */
  bk7258_nand_seed_agent_config();

#ifdef CONFIG_BK7258_WIFI
#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  /* Whether or not the load worked: credentials typed during this boot are
   * just as usable as credentials read back from flash.
   *
   * Association, DHCP and the console service, in that order and from this
   * task -- see bk7258_net_autostart().  It replaced a bare
    * old key-value apply call here: associating without asking for an
   * address left the interface RUNNING at NuttX's default 10.0.0.2, which
   * looks configured and routes nowhere.
   */

  bk7258_net_autostart();
#endif
#endif

  return 0;
}
#endif

/****************************************************************************
 * Name: bk7258_report_cache
 *
 * Description:
 *   Reports whether this core implements instruction and data caches, and is
 *   left in at boot so the next person does not have to guess.
 *
 *   The AP executes in place from flash at 0x02150000.  NuttX did not enable
 *   the instruction cache until commit "perf(bk7258): enable the AP
 *   instruction cache", and every instruction fetch was a flash access: the
 *   expression renderer cost ~455ms per frame for work that takes 0.09ms on a
 *   host, a 51200-byte word-at-a-time copy ran at 0.8MB/s, and raising the
 *   core to 480MHz changed none of it because the core was never the limit.
 *   The same frame now costs 37ms.
 *
 *   Turning it on needed two independent fixes, and the first one alone looks
 *   like "the cache does nothing": CONFIG_ARMV8M_ICACHE depends on the hidden
 *   ARMV8M_HAVE_ICACHE, which this chip's Kconfig did not select, so asking
 *   for it in a defconfig was silently dropped; and selecting it only compiles
 *   up_enable_icache() in -- every other ARMv8-M chip in the tree calls that
 *   from its own start code, this one did not.  Both are fixed now (see the
 *   chip Kconfig and bk7258_start.c).
 *
 *   Only the I-Cache is on.  Instruction fetch is read-only and needs no
 *   maintenance against the camera, panel and mailbox DMA that shares AP RAM,
 *   which the MPU deliberately keeps non-cacheable.
 *
 *   CLIDR/CTR exist in ARMv8-M whether or not a cache is fitted (they read
 *   zero when it is not), so reading them is safe and settles the question:
 *   this part reports CLIDR=0x09200003, i.e. both caches are implemented.
 *
 ****************************************************************************/

static void bk7258_report_cache(void)
{
  uint32_t clidr = getreg32(0xe000ed78);   /* SCB->CLIDR */
  uint32_t ctr   = getreg32(0xe000ed7c);   /* SCB->CTR   */
  unsigned int l1 = clidr & 7;

  printf("cache: CLIDR=0x%08" PRIx32 " CTR=0x%08" PRIx32
         " L1=%s%s%s\n", clidr, ctr,
         (l1 & 1) != 0 ? "I" : "",
         (l1 & 2) != 0 ? "D" : "",
         l1 == 0 ? "none (uncached XIP from flash)" :
         (l1 == 3 ? " (separate I and D)" : ""));
}

void board_late_initialize(void)
{
  int ret = bk7258_bringup();

  bk7258_report_cache();

  if (ret < 0)
    {
      printf("board bring-up stopped, error=%d\n", ret);
      return;
    }

  printf("board bring-up initialization completed\n");

  /* GC9D01 QSPI panel bring-up smoke test.  board_app_finalinitialize()
   * (BOARDIOC_FINALINIT) is never invoked in this minimal, apps-less NSH
   * configuration (no CONFIG_BOARDCTL_FINALINIT, no apps/nshlib start-up
   * script caller), so the test entry point is invoked here instead, from
   * the unconditionally-called CONFIG_BOARD_LATE_INITIALIZE hook.  See
   * docs/superpowers/plans/2026-07-29-gc9d01-lcd-bringup.md Task 3 Step 3.
   */
#ifdef CONFIG_BK7258_GC9D01_FB
  /* GC9D01 framebuffers, one per populated panel: fb_register() calls
   * up_fbinitialize(), which does the rail/reset/bus bring-up and the init
   * sequence and allocates a 51200-byte RGB565 framebuffer, then publishes
   * /dev/fb<display>.
   *
   * Both panels are registered unconditionally.  GC9D01 has no readable ID
   * register, so an unpopulated footprint cannot be distinguished from a
   * working panel -- every command still "completes".  Registering /dev/fb1
   * on a single-panel board therefore costs one unused 51200-byte buffer
   * and nothing else; refusing to register it would need board-variant
   * configuration that this board provides no way to detect.
   */

  {
    int display;
    int registered = 0;

    /* Both panels through reset and the command table in one pass, before
     * fb_register() asks for either of them.  The sequence is nearly all
     * waiting (20ms rail, 230ms reset pulse, 120ms after Sleep Out) and the
     * waits are the panels', not the bus's, so sharing them across the two
     * panels takes ~350ms off the time to the first pixel.  fb_register()
     * still calls up_fbinitialize() -> bk7258_gc9d01_panel_init(), which
     * finds its panel already up and returns.
     */

    (void)bk7258_gc9d01_panels_init((1 << GC9D01_NDISPLAYS) - 1);

    for (display = 0; display < GC9D01_NDISPLAYS; display++)
      {
        ret = fb_register(display, 0);
        if (ret < 0)
          {
            printf("fb_register(%d) failed: %d\n", display, ret);
            continue;
          }

        registered |= 1 << display;
        printf("fb: /dev/fb%d registered\n", display);
      }

    /* Non-product boot greeting, written out rather than simply appearing.
     *
     * The panel has no readable ID register, so "the init sequence was sent"
     * has never been evidence that it worked; something has to be drawn at
     * boot or a dead panel looks exactly like a working one.
     *
     * Both panels are revealed together, one stroke segment per step, so the
     * word is written left to right.  The step count is the whole cost: the
     * panel takes no partial update, so every step pushes a full 51200-byte
     * frame to each panel at ~25ms, and the stroke rendering adds to that.
     * 20 steps measured 1677ms and was by itself most of the delay between
     * reset and a readable screen; 8 steps measured 408ms on the same board
     * and still reads as writing rather than appearing.  Keep it at 8 unless
     * there is a measurement to justify more.
     *
     * The pen-stroke version of this lives in the 'hello' shell command,
     * which has a proper stroke font.  It cannot be reused here: it is an
     * application, and this configuration builds neither exec_builtin() nor
     * posix_spawn(), so bring-up has no way to start it.
     */

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
    if (registered != 0)
      {
        (void)bk7258_gc9d01_fb_hello_animate(registered, 8);
      }
#else
    /* Keep the backlight dark until both panel GRAMs contain a known frame.
     * This reuses the same full-frame fill and push path that the historical
     * greeting animation used before drawing its first stroke.  LVGL starts
     * only after this temporary black frame is visible and then owns every
     * subsequent update. */

    for (display = 0; display < GC9D01_NDISPLAYS; display++)
      {
        if ((registered & (1 << display)) != 0)
          {
            ret = bk7258_gc9d01_fb_fill(display, 0x0000u);
            if (ret < 0)
              {
                printf("gc9d01_fb[%d]: boot black fill failed: %d\n",
                       display, ret);
              }
          }
      }

    if (registered == (1 << GC9D01_NDISPLAYS) - 1)
      {
        ret = velasight_autostart();
        if (ret < 0)
          {
            printf("velasight autostart failed: %d\n", ret);
          }
      }
    else
      {
        printf("velasight not started: both framebuffers are required\n");
      }
#endif
  }
#endif

#if defined(CONFIG_BK7258_WIFI) && \
    defined(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  /* VelaSight has already submitted its first LVGL frame.  Network setup is
   * deliberately later so Wi-Fi cannot delay or suppress display ownership.
   */

  ret = bk7258_wifi_initialize();
  g_velasight_wifi_result = ret;
  nxsem_post(&g_velasight_wifi_ready);
  if (ret < 0)
    {
      printf("Wi-Fi initialization failed, error=%d; UI remains active\n", ret);
    }
#endif

  /* GC2145 camera: registered as a standard V4L2 /dev/video0 node here
   * (imgdata+imgsensor framework, see bk7258_camera_bringup.c), superseding
   * the older bare board_late_initialize() smoke-test call this line used
   * to make directly.  The old bk7258_gc2145_test() bare entry point is
   * kept as an independent NSH command (see bk7258_appinit.c) for
   * side-by-side diagnostic comparison, per design discussion in
   * docs/superpowers/plans/2026-07-30-gc2145-camera-bringup.md -- it is
   * intentionally NOT called from here anymore, so a bug in this new V4L2
   * driver cannot be masked by (or confused with) the old direct-call
   * path still running unconditionally at boot. */
  (void)bk7258_camera_initialize();

#ifdef CONFIG_BK7258_AUDIO
  /* Internal-DAC audio.  This has to come after bk7258_bringup() above,
   * not from inside it, for the same reason the camera does: the AUD block
   * needs the AUDP power domain and the AUD module clock, and both are
   * requested from CP1 over the PWC mailbox channel that bk7258_pwc_start()
   * brings up.  Registering the audio devices is best-effort -- a failure
   * here must not take the console down with it.
   */

  (void)bk7258_audio_initialize();
#endif

#ifdef CONFIG_BK7258_JPEG_ENC
  /* After the camera: both want PSRAM, and registering the encoder second
   * keeps /dev/video0 belonging to capture and /dev/video1 to this codec no
   * matter how the framework numbers them internally.  Best-effort, like the
   * others -- a codec that fails to register must not take the console down.
   */

  (void)bk7258_jpeg_enc_initialize();
#endif

  /* Configuration that survives a reset.  Deliberately after the display and
   * the camera: it talks to the CP's flash service over the mailbox and can
   * wait up to a second if that service does not answer, which is not
   * something the boot screen should queue behind.  A failure here is
   * reported and ignored -- the board still runs, it just forgets settings on
   * reset, which is what it did before this existed.
   */

  /* Leave the panels showing the product's own idle state rather than the
   * boot greeting.  From here on the screen is event driven: it is repainted
   * when a state or a result changes and at no other time, which is what the
   * spec requires ("屏幕仅事件触发刷新，不运行实时 Camera Preview").
   */

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  (void)bk7258_status_screen_state(BK7258_STATUS_IDLE);
#endif

#ifdef CONFIG_BK7258_BLUETOOTH
  /* Last, so that nothing a user can see waits on the radio.  See
   * bk7258_bt_bringup() above for why this is not part of bk7258_bringup().
   */

  bk7258_bt_bringup();
#endif

  /* Legacy non-product configurations may still use the old network startup. */

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT
  if (kthread_create("nand_cfg", SCHED_PRIORITY_DEFAULT, 3072,
                     nand_config_loader, NULL) < 0)
    {
      printf("bk7258_bringup: nand config loader not started; settings will not "
             "survive a reset\n");
    }
#endif
}
