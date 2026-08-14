/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/board.h>
#include <nuttx/clock.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <syslog.h>
#include <nuttx/timers/pwm.h>

#include <arch/board/board.h>

#define BK7258_KEY_POLL_US       5000
#define BK7258_KEY_DEBOUNCE_COUNT 6
#define BK7258_MOTOR_FREQUENCY   1000
#define BK7258_MOTOR_DUTY        ((ub16_t)((7u * 65536u) / 10u))
#define BK7258_PM_CLK_CTRL_CMD   2
#define BK7258_PM_CLK_ID_PWM_1   3
#define BK7258_PM_CLK_POWER_UP   1

struct pwm_lowerhalf_s *bk7258_pwminitialize(void);
int bk7258_mailbox_send_pwc(uint8_t command, uint32_t p1, uint32_t p2,
                            uint32_t p3);
int bk7258_mailbox_wait_pwc(unsigned int timeout_ms);

#ifdef CONFIG_BK7258_POWER_KEY_MOTOR
static sem_t g_motor_ready_sem;
static volatile int g_motor_worker_result;

static int bk7258_motor_button_worker(int argc, char **argv)
{
  struct pwm_info_s info =
  {
    .frequency = BK7258_MOTOR_FREQUENCY,
    .duty = BK7258_MOTOR_DUTY,
  };
  bool candidate = false;
  bool pressed = false;
  unsigned int samples = 0;
  int ret;
  int fd = -1;
  /* clock_t request_start; */

  (void)argc;
  (void)argv;
  /* request_start = clock_systime_ticks(); */
  ret = bk7258_mailbox_send_pwc(BK7258_PM_CLK_CTRL_CMD,
                                BK7258_PM_CLK_ID_PWM_1,
                                BK7258_PM_CLK_POWER_UP, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "power-key motor: PWM clock request failed: %d\n", ret);
      goto ready;
    }

  ret = bk7258_mailbox_wait_pwc(200);
  /* syslog(LOG_INFO, "power-key motor: PWM clock transport wait=%d elapsed=%lu ms\n",
         ret, (unsigned long)TICK2MSEC(clock_systime_ticks() - request_start)); */
  if (ret < 0)
    {
      syslog(LOG_ERR, "power-key motor: PWM clock ACK failed: %d\n", ret);
      goto ready;
    }

  nxsig_usleep(20000);
  fd = open("/dev/pwm0", O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "power-key motor: open pwm0 failed: %d\n", ret);
      goto ready;
    }

  if (ioctl(fd, PWMIOC_SETCHARACTERISTICS,
            (unsigned long)(uintptr_t)&info) < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "power-key motor: set PWM characteristics failed: %d\n",
             ret);
      close(fd);
      goto ready;
    }

  ret = OK;

ready:
  g_motor_worker_result = ret;
  nxsem_post(&g_motor_ready_sem);
  if (ret < 0)
    {
      return ret;
    }

  /* printf("power-key motor worker ready, PWM parameters applied\n"); */

  for (;;)
    {
      bool sample = (board_buttons() & BUTTON_POWER_BIT) != 0;

      if (sample != candidate)
        {
          candidate = sample;
          samples = 1;
        }
      else if (samples < BK7258_KEY_DEBOUNCE_COUNT)
        {
          samples++;
        }

      if (samples == BK7258_KEY_DEBOUNCE_COUNT && pressed != candidate)
        {
          int cmd = candidate ? PWMIOC_START : PWMIOC_STOP;
          /* clock_t edge_start = clock_systime_ticks(); */

          if (ioctl(fd, cmd, 0) < 0)
            {
              syslog(LOG_ERR, "power-key motor: PWM cmd=%d failed: %d\n",
                     cmd, -errno);
              close(fd);
              return -errno;
            }

          pressed = candidate;
          /* syslog(LOG_INFO,
                 "power-key motor: button=%u, pwm cmd=%d applied elapsed=%lu ms\n",
                 candidate, cmd,
                 (unsigned long)TICK2MSEC(clock_systime_ticks() - edge_start)); */
        }

      nxsig_usleep(BK7258_KEY_POLL_US);
    }
}
#endif

int bk7258_motor_setup(void)
{
  static bool initialized;
  struct pwm_lowerhalf_s *pwm;
  int ret;

  if (initialized)
    {
      return OK;
    }

  pwm = bk7258_pwminitialize();
  if (pwm == NULL)
    {
      return -ENODEV;
    }

  ret = pwm_register("/dev/pwm0", pwm);
  if (ret >= 0)
    {
      initialized = true;
    }

  return ret;
}

int bk7258_power_key_motor_start(void)
{
#ifdef CONFIG_BK7258_POWER_KEY_MOTOR
  int pid;

  nxsem_init(&g_motor_ready_sem, 0, 0);
  g_motor_worker_result = -EINPROGRESS;
  pid = kthread_create("power-key-motor", 100, 1536,
                       bk7258_motor_button_worker, NULL);
  if (pid < 0)
    {
      return pid;
    }
  if (nxsem_tickwait_uninterruptible(&g_motor_ready_sem,
                                     MSEC2TICK(500)) < 0)
    {
      return -ETIMEDOUT;
    }

  return g_motor_worker_result;
#else
  return OK;
#endif
}
