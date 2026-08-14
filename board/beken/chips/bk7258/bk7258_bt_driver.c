/****************************************************************************
 * BK7258 NuttX Bluetooth lower-half.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/net/bluetooth.h>
#include <nuttx/wireless/bluetooth/bt_buf.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>

#include "bk7258_bt.h"

static int bk7258_bt_open(FAR struct bt_driver_s *btdev)
{
  (void)btdev;
  return bk7258_bt_transport_open();
}

static int bk7258_bt_send(FAR struct bt_driver_s *btdev,
                          enum bt_buf_type_e type,
                          FAR void *data, size_t length)
{
  (void)btdev;
  return bk7258_bt_transport_send(type, data, length);
}

static void bk7258_bt_close(FAR struct bt_driver_s *btdev)
{
  (void)btdev;
  bk7258_bt_transport_close();
}

static int bk7258_bt_receive(enum bt_buf_type_e type,
                             const uint8_t *data, size_t length,
                             void *arg)
{
  FAR struct bt_driver_s *btdev = arg;

  if (btdev == NULL || data == NULL || length == 0 ||
      length > BLUETOOTH_MAX_FRAMELEN || btdev->receive == NULL)
    {
      return -EINVAL;
    }

  if (type != BT_EVT && type != BT_ACL_IN)
    {
      return -EINVAL;
    }

  return bt_netdev_receive(btdev, type, (FAR void *)data, length);
}

static struct bt_driver_s g_bk7258_bt_driver =
{
  .head_reserve = 0,
  .open         = bk7258_bt_open,
  .send         = bk7258_bt_send,
  .close        = bk7258_bt_close,
  .ioctl        = NULL,
  .priv         = NULL,
};

int bk7258_bt_driver_register(void)
{
  int ret;

  bk7258_bt_transport_set_receiver(bk7258_bt_receive,
                                   &g_bk7258_bt_driver);
  ret = bt_driver_register(&g_bk7258_bt_driver);
  if (ret < 0)
    {
      bk7258_bt_transport_set_receiver(NULL, NULL);
    }

  return ret;
}
