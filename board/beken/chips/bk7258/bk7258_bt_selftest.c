/****************************************************************************
 * BK7258 raw Bluetooth HCI transport self-test.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include <nuttx/clock.h>
#include <nuttx/net/bluetooth.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <nuttx/wireless/bluetooth/bt_buf.h>
#include <nuttx/wireless/bluetooth/bt_hci.h>

#include "bk7258_bt.h"

#define BT_SELFTEST_TIMEOUT MSEC2TICK(5000)

struct bt_selftest_case
{
  uint16_t opcode;
  const uint8_t *parameters;
  uint8_t parameter_length;
  const char *name;
};

static sem_t g_selftest_sem;
static uint16_t g_selftest_opcode;
static int g_selftest_status;

static uint16_t selftest_get_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static int selftest_receive(enum bt_buf_type_e type, const uint8_t *data,
                            size_t length, void *arg)
{
  uint16_t opcode;
  int status;

  (void)arg;

  if (type != BT_EVT || data == NULL || length < 6)
    {
      return OK;
    }

  if (data[0] == BT_HCI_EVT_CMD_COMPLETE && data[1] >= 4)
    {
      opcode = selftest_get_le16(&data[3]);
      status = data[5] == 0 ? OK : -EREMOTEIO;
    }
  else if (data[0] == BT_HCI_EVT_CMD_STATUS && data[1] == 4)
    {
      opcode = selftest_get_le16(&data[4]);
      status = data[2] == 0 ? OK : -EREMOTEIO;
    }
  else
    {
      return OK;
    }

  if (opcode == g_selftest_opcode)
    {
      g_selftest_status = status;
      nxsem_post(&g_selftest_sem);
    }

  return OK;
}

static int selftest_wait_free(uint32_t target)
{
  clock_t deadline = clock_systime_ticks() + BT_SELFTEST_TIMEOUT;

  for (;;)
    {
      struct bk7258_bt_stats stats;

      bk7258_bt_transport_get_stats(&stats);
      if (stats.tx_free >= target)
        {
          return OK;
        }

      if ((int32_t)(clock_systime_ticks() - deadline) >= 0)
        {
          return -ETIMEDOUT;
        }

      nxsig_usleep(1000);
    }
}

static int selftest_command(const struct bt_selftest_case *test)
{
  uint8_t command[3 + 8];
  struct bk7258_bt_stats stats;
  int ret;

  command[0] = test->opcode & 0xffu;
  command[1] = test->opcode >> 8;
  command[2] = test->parameter_length;
  memcpy(&command[3], test->parameters, test->parameter_length);

  bk7258_bt_transport_get_stats(&stats);
  g_selftest_opcode = test->opcode;
  g_selftest_status = -EINPROGRESS;
  nxsem_reset(&g_selftest_sem, 0);

  ret = bk7258_bt_transport_send(BT_CMD, command,
                                 3 + test->parameter_length);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_tickwait_uninterruptible(&g_selftest_sem,
                                       BT_SELFTEST_TIMEOUT);
  if (ret < 0)
    {
      return ret;
    }

  if (g_selftest_status < 0)
    {
      return g_selftest_status;
    }

  ret = selftest_wait_free(stats.tx_free + 1);
  printf("bt-selftest: %-28s opcode=0x%04x result=%d\n",
         test->name, test->opcode, ret);
  return ret;
}

int bk7258_bt_raw_selftest_run(void)
{
  static const uint8_t event_mask[8] =
  {
    0x9c, 0xe8, 0x04, 0x02, 0x00, 0x80, 0x00, 0x20
  };
  static const uint8_t le_event_mask[8] =
  {
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  static const uint8_t host_buffer_size[7] =
  {
    BLUETOOTH_MAX_FRAMELEN - sizeof(struct bt_hci_acl_hdr_s), 0x00,
    0x00, CONFIG_BLUETOOTH_BUFFER_PREALLOC, 0x00, 0x00, 0x00
  };
  static const uint8_t flow_control[1] = {1};
  static const struct bt_selftest_case tests[] =
  {
    {BT_HCI_OP_RESET, NULL, 0, "HCI Reset"},
    {BT_HCI_OP_READ_LOCAL_FEATURES, NULL, 0, "Read Local Features"},
    {BT_HCI_OP_READ_LOCAL_VERSION_INFO, NULL, 0, "Read Local Version"},
    {BT_HCI_OP_READ_BD_ADDR, NULL, 0, "Read BD_ADDR"},
    {BT_HCI_OP_LE_READ_LOCAL_FEATURES, NULL, 0, "LE Read Local Features"},
    {BT_HCI_OP_LE_READ_BUFFER_SIZE, NULL, 0, "LE Read Buffer Size"},
    {BT_HCI_OP_SET_EVENT_MASK, event_mask, sizeof(event_mask),
     "Set Event Mask"},
    {BT_HCI_OP_LE_SET_EVENT_MASK, le_event_mask, sizeof(le_event_mask),
     "LE Set Event Mask"},
    {BT_HCI_OP_HOST_BUFFER_SIZE, host_buffer_size, sizeof(host_buffer_size),
     "Host Buffer Size"},
    {BT_HCI_OP_SET_CTL_TO_HOST_FLOW, flow_control, sizeof(flow_control),
     "Controller To Host Flow"},
  };
  unsigned int i;
  int ret;

  nxsem_init(&g_selftest_sem, 0, 0);
  bk7258_bt_transport_set_receiver(selftest_receive, NULL);

  ret = bk7258_bt_transport_open();
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < nitems(tests); i++)
    {
      ret = selftest_command(&tests[i]);
      if (ret < 0)
        {
          bk7258_bt_transport_dump_stats();
          return ret;
        }
    }

  bk7258_bt_transport_dump_stats();
  printf("bt-selftest: B1/B2 raw HCI sequence passed\n");
  return OK;
}
