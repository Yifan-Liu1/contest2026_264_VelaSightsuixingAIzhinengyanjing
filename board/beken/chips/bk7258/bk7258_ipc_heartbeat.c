/****************************************************************************
 * BK7258 Armino-compatible HW_CTRL power-up and heartbeat service.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>

#include "hardware/bk7258_mbox.h"
#define IPC_CPU1_POWER_UP_INDICATION   1u
#define IPC_CPU1_HEART_BEAT_INDICATION 2u
#define IPC_ACK_STATE_COMPLETE         2u
#define IPC_RESPONSE_TIMEOUT_MS        600u
#define IPC_HEARTBEAT_INTERVAL         MSEC2TICK(2000)
#define IPC_HEARTBEAT_RETRY            MSEC2TICK(100)
#define IPC_HEARTBEAT_POLL_US           10000u
#define IPC_HEARTBEAT_PRIORITY          110

static mutex_t g_ipc_lock = NXMUTEX_INITIALIZER;
static volatile int g_ipc_ack_result;
static volatile int g_ipc_start_result;
static volatile int g_heartbeat_result;
static volatile bool g_heartbeat_enabled;
static volatile bool g_heartbeat_pending;
static volatile uint32_t g_heartbeat_completions;
static clock_t g_next_heartbeat;
static uint32_t g_heartbeat_failures;
static uint32_t g_heartbeat_submissions;
static uint32_t g_heartbeat_reported;
static bool g_ipc_started;

static int ipc_ack_result(const struct bk7258_mb_wire_message *ack,
                          int result)
{
  if (result == OK &&
      (ack == NULL || ack->reserved != IPC_ACK_STATE_COMPLETE))
    {
      result = -EREMOTEIO;
    }

  return result;
}

static void ipc_tx_complete(const struct bk7258_mb_wire_message *ack,
                            int result, void *arg)
{
  g_ipc_ack_result = ipc_ack_result(ack, result);
  if ((uintptr_t)arg == IPC_CPU1_POWER_UP_INDICATION &&
      g_ipc_ack_result == OK)
    {
      g_heartbeat_result = OK;
      g_next_heartbeat = clock_systime_ticks() + IPC_HEARTBEAT_INTERVAL;
      g_heartbeat_enabled = true;
    }
}

static void ipc_heartbeat_complete(
  const struct bk7258_mb_wire_message *ack, int result, void *arg)
{
  (void)arg;
  g_heartbeat_result = ipc_ack_result(ack, result);
  g_heartbeat_completions++;
  g_heartbeat_pending = false;
}

static int ipc_send(uint8_t command, uint32_t parameter)
{
  struct bk7258_mb_wire_message message;
  volatile uint32_t *payload =
    (volatile uint32_t *)(uintptr_t)BK7258_IPC_TX_ADDRESS;
  int ret;

  ret = nxmutex_lock(&g_ipc_lock);
  if (ret < 0)
    {
      return ret;
    }

  *payload = parameter;
  __asm__ volatile("dmb sy" ::: "memory");

  memset(&message, 0, sizeof(message));
  message.header = command;
  message.payload_address = BK7258_IPC_TX_ADDRESS;
  message.payload_length =
    command == IPC_CPU1_HEART_BEAT_INDICATION ? sizeof(*payload) : 0;
  g_ipc_ack_result = -EINPROGRESS;

  ret = bk7258_mailbox_send_wire(BK7258_MB_CHAN_HW_CTRL_TX, &message,
                                  ipc_tx_complete,
                                  (void *)(uintptr_t)command);
  if (ret == OK)
    {
      ret = bk7258_mailbox_wait_hw_control(IPC_RESPONSE_TIMEOUT_MS);
      if (ret == OK)
        {
          ret = g_ipc_ack_result;
        }
    }

  nxmutex_unlock(&g_ipc_lock);
  return ret > 0 ? -EIO : ret;
}

static int ipc_heartbeat_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  for (;;)
    {
      nxsig_usleep(IPC_HEARTBEAT_POLL_US);
      bk7258_ipc_heartbeat_poll();
    }

  return OK;
}

void bk7258_ipc_heartbeat_poll(void)
{
  struct bk7258_mb_wire_message message;
  volatile uint32_t *payload =
    (volatile uint32_t *)(uintptr_t)BK7258_IPC_TX_ADDRESS;
  clock_t now;
  int ret;

  if (!g_heartbeat_enabled || g_heartbeat_pending)
    {
      return;
    }

  if (g_heartbeat_completions != g_heartbeat_reported)
    {
      g_heartbeat_reported = g_heartbeat_completions;

      /* Only the first ACK is worth a line: it is the evidence that the
       * HW_CTRL round trip works at all.  The next ones say nothing new and
       * they used to print straight into the freshly-drawn NSH prompt
       * ("nsh> IPC: heartbeat queued count=1 ...") for the first six seconds
       * of every boot, which reads like a broken console.  A heartbeat that
       * stops being ACKed is not silent: the failure path below reports it
       * through syslog(LOG_ERR), and the consequence is loud anyway -- the
       * CP resets the part after CONFIG_INT_WDT_PERIOD_MS.
       */

      if (g_heartbeat_reported == 1u)
        {
          printf("IPC: heartbeat ACK result=%d, interval=%u ms\n",
                 g_heartbeat_result,
                 (unsigned int)TICK2MSEC(IPC_HEARTBEAT_INTERVAL));
        }
    }

  if (g_heartbeat_result < 0)
    {
      g_heartbeat_failures++;
      if ((g_heartbeat_failures & (g_heartbeat_failures - 1u)) == 0)
        {
          syslog(LOG_ERR, "IPC heartbeat failed: %d count=%lu\n",
                 g_heartbeat_result,
                 (unsigned long)g_heartbeat_failures);
        }

      g_heartbeat_result = OK;
    }

  now = clock_systime_ticks();
  if ((sclock_t)(now - g_next_heartbeat) < 0)
    {
      return;
    }

  *payload = 0;
  __asm__ volatile("dmb sy" ::: "memory");
  memset(&message, 0, sizeof(message));
  message.header = IPC_CPU1_HEART_BEAT_INDICATION;
  message.payload_address = BK7258_IPC_TX_ADDRESS;
  message.payload_length = sizeof(*payload);
  g_heartbeat_result = -EINPROGRESS;
  g_heartbeat_pending = true;
  ret = bk7258_mailbox_send_wire(BK7258_MB_CHAN_HW_CTRL_TX, &message,
                                 ipc_heartbeat_complete, NULL);
  if (ret == OK)
    {
      g_heartbeat_submissions++;
      g_next_heartbeat = now + IPC_HEARTBEAT_INTERVAL;
    }
  else
    {
      g_heartbeat_pending = false;
      g_heartbeat_result = ret;
      g_next_heartbeat = now + IPC_HEARTBEAT_RETRY;
    }
}

int bk7258_ipc_heartbeat_start(void)
{
  int pid;
  int ret;

  if (g_ipc_started)
    {
      return g_ipc_start_result;
    }

  g_ipc_start_result = -EINPROGRESS;
  g_heartbeat_enabled = false;
  g_heartbeat_pending = false;
  g_heartbeat_completions = 0;
  g_heartbeat_failures = 0;
  g_heartbeat_submissions = 0;
  g_heartbeat_reported = 0;
  ret = ipc_send(IPC_CPU1_POWER_UP_INDICATION, 0);
  g_ipc_start_result = ret < 0 ? ret : OK;

  if (g_ipc_start_result == OK)
    {
      pid = kthread_create("ipc-heartbeat", IPC_HEARTBEAT_PRIORITY, 1536,
                           ipc_heartbeat_worker, NULL);
      if (pid < 0)
        {
          g_ipc_start_result = pid;
        }
    }

  g_ipc_started = g_ipc_start_result == OK;

  if (g_ipc_start_result == OK)
    {
      printf("IPC: HW_CTRL power-up acknowledged by CP, heartbeat armed "
             "now=%llu deadline=%llu\n",
             (unsigned long long)clock_systime_ticks(),
             (unsigned long long)g_next_heartbeat);
    }

  return g_ipc_start_result;
}
