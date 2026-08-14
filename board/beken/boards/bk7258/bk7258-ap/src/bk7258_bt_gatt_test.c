/****************************************************************************
 * BK7258 Bluetooth GATT acceptance fixture.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <nuttx/wireless/bluetooth/bt_core.h>
#include <nuttx/wireless/bluetooth/bt_gatt.h>
#include <nuttx/wireless/bluetooth/bt_uuid.h>

#define BK7258_BT_TEST_SERVICE_HANDLE  0x0100
#define BK7258_BT_TEST_RW_CHRC_HANDLE  0x0101
#define BK7258_BT_TEST_RW_VALUE_HANDLE 0x0102
#define BK7258_BT_TEST_N_CHRC_HANDLE   0x0103
#define BK7258_BT_TEST_N_VALUE_HANDLE  0x0104
#define BK7258_BT_TEST_CCC_HANDLE      0x0105
#define BK7258_BT_TEST_VALUE_LENGTH    20

static uint8_t g_test_value[BK7258_BT_TEST_VALUE_LENGTH] =
{
  'V', 'e', 'l', 'a', 'S', 'i', 'g', 'h', 't', 's'
};
static uint8_t g_notify_value;
static bool g_notify_enabled;
static struct work_s g_notify_work;
static struct bt_gatt_ccc_cfg_s
  g_notify_ccc[CONFIG_BLUETOOTH_MAX_CONN];

static void test_notify_worker(void *arg);

static struct bt_uuid_s g_gap_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = BT_UUID_GAP,
};

static struct bt_uuid_s g_name_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = BT_UUID_GAP_DEVICE_NAME,
};

static struct bt_uuid_s g_appearance_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = BT_UUID_GAP_APPEARANCE,
};

static struct bt_uuid_s g_service_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = 0xfff0,
};

static struct bt_uuid_s g_rw_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = 0xfff1,
};

static struct bt_uuid_s g_notify_uuid =
{
  .type = BT_UUID_16,
  .u.u16 = 0xfff2,
};

static struct bt_gatt_chrc_s g_name_chrc =
{
  .uuid = &g_name_uuid,
  .value_handle = 0x0004,
  .properties = BT_GATT_CHRC_READ,
};

static struct bt_gatt_chrc_s g_appearance_chrc =
{
  .uuid = &g_appearance_uuid,
  .value_handle = 0x0006,
  .properties = BT_GATT_CHRC_READ,
};

static struct bt_gatt_chrc_s g_rw_chrc =
{
  .uuid = &g_rw_uuid,
  .value_handle = BK7258_BT_TEST_RW_VALUE_HANDLE,
  .properties = BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
                BT_GATT_CHRC_WRITE_WITHOUT_RESP,
};

static struct bt_gatt_chrc_s g_notify_chrc =
{
  .uuid = &g_notify_uuid,
  .value_handle = BK7258_BT_TEST_N_VALUE_HANDLE,
  .properties = BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
};

static int test_read_name(FAR struct bt_conn_s *conn,
                          FAR const struct bt_gatt_attr_s *attr,
                          FAR void *buf, uint8_t len, uint16_t offset)
{
  const char *name = attr->user_data;

  return bt_gatt_attr_read(conn, attr, buf, len, offset, name,
                           strlen(name));
}

static int test_read_appearance(FAR struct bt_conn_s *conn,
                                FAR const struct bt_gatt_attr_s *attr,
                                FAR void *buf, uint8_t len, uint16_t offset)
{
  uint16_t appearance = BT_HOST2LE16(CONFIG_DEVICE_APPEARANCE);

  return bt_gatt_attr_read(conn, attr, buf, len, offset, &appearance,
                           sizeof(appearance));
}

static int test_read_value(FAR struct bt_conn_s *conn,
                           FAR const struct bt_gatt_attr_s *attr,
                           FAR void *buf, uint8_t len, uint16_t offset)
{
  return bt_gatt_attr_read(conn, attr, buf, len, offset, g_test_value,
                           sizeof(g_test_value));
}

static int test_write_value(FAR struct bt_conn_s *conn,
                            FAR const struct bt_gatt_attr_s *attr,
                            FAR const void *buf, uint8_t len,
                            uint16_t offset)
{
  printf("bt-gatt-test: write len=%u offset=%u\n", len, offset);

  if (offset > sizeof(g_test_value) ||
      len > sizeof(g_test_value) - offset)
    {
      return -EINVAL;
    }

  memcpy(&g_test_value[offset], buf, len);
  return len;
}

static int test_read_notify(FAR struct bt_conn_s *conn,
                            FAR const struct bt_gatt_attr_s *attr,
                            FAR void *buf, uint8_t len, uint16_t offset)
{
  return bt_gatt_attr_read(conn, attr, buf, len, offset, &g_notify_value,
                           sizeof(g_notify_value));
}

static void test_ccc_changed(uint16_t value)
{
  printf("bt-gatt-test: ccc=0x%04x\n", value);
  g_notify_enabled = value == BT_GATT_CCC_NOTIFY;
  if (!g_notify_enabled)
    {
      (void)work_cancel(LPWORK, &g_notify_work);
    }
  else
    {
      (void)work_queue(LPWORK, &g_notify_work,
                       test_notify_worker, NULL, 0);
    }
}

static void test_notify_worker(void *arg)
{
  (void)arg;

  if (g_notify_enabled)
    {
      g_notify_value++;
      printf("bt-gatt-test: notify value=%u\n", g_notify_value);
      bt_gatt_notify(BK7258_BT_TEST_N_VALUE_HANDLE,
                     &g_notify_value, sizeof(g_notify_value));
      (void)work_queue(LPWORK, &g_notify_work, test_notify_worker, NULL,
                       MSEC2TICK(1000));
    }
}

static const struct bt_gatt_attr_s g_test_attrs[] =
{
  BT_GATT_PRIMARY_SERVICE(0x0001, &g_gap_uuid),
  BT_GATT_CHARACTERISTIC(0x0003, &g_name_chrc),
  BT_GATT_DESCRIPTOR(0x0004, &g_name_uuid, BT_GATT_PERM_READ,
                     test_read_name, NULL, (FAR void *)CONFIG_DEVICE_NAME),
  BT_GATT_CHARACTERISTIC(0x0005, &g_appearance_chrc),
  BT_GATT_DESCRIPTOR(0x0006, &g_appearance_uuid, BT_GATT_PERM_READ,
                     test_read_appearance, NULL, NULL),
  BT_GATT_PRIMARY_SERVICE(BK7258_BT_TEST_SERVICE_HANDLE,
                          &g_service_uuid),
  BT_GATT_CHARACTERISTIC(BK7258_BT_TEST_RW_CHRC_HANDLE, &g_rw_chrc),
  BT_GATT_DESCRIPTOR(BK7258_BT_TEST_RW_VALUE_HANDLE, &g_rw_uuid,
                     BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                     test_read_value, test_write_value, g_test_value),
  BT_GATT_CHARACTERISTIC(BK7258_BT_TEST_N_CHRC_HANDLE, &g_notify_chrc),
  BT_GATT_DESCRIPTOR(BK7258_BT_TEST_N_VALUE_HANDLE, &g_notify_uuid,
                     BT_GATT_PERM_READ, test_read_notify, NULL,
                     &g_notify_value),
  BT_GATT_CCC(BK7258_BT_TEST_CCC_HANDLE, BK7258_BT_TEST_N_VALUE_HANDLE,
              g_notify_ccc, test_ccc_changed),
};

int bk7258_bt_gatt_test_initialize(void)
{
  g_notify_enabled = false;
  g_notify_value = 0;
  memset(g_notify_ccc, 0, sizeof(g_notify_ccc));
  bt_gatt_register(g_test_attrs, nitems(g_test_attrs));
  return OK;
}
