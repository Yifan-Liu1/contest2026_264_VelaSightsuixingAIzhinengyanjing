/****************************************************************************
 * BK7258 Bluetooth transport and NuttX lower-half interfaces.
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_BK7258_BT_H
#define __VENDOR_BEKEN_CHIPS_BK7258_BK7258_BT_H

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

enum bt_buf_type_e;

typedef int (*bk7258_bt_receive_t)(enum bt_buf_type_e type,
                                   const uint8_t *data, size_t length,
                                   void *arg);

struct bk7258_bt_stats
{
  uint32_t tx_enqueued;
  uint32_t tx_commands;
  uint32_t tx_acl;
  uint32_t tx_ack;
  uint32_t tx_ack_error;
  uint32_t tx_free;
  uint32_t tx_unknown_free;
  uint32_t tx_quarantined;
  uint32_t rx_enqueued;
  uint32_t rx_events;
  uint32_t rx_acl;
  uint32_t rx_delivered;
  uint32_t rx_rejected;
  uint32_t rx_duplicate;
  uint32_t rx_free_ack;
  uint32_t rx_free_quarantined;
  uint32_t queue_full;
  uint32_t last_remote_pointer;
  uint8_t last_remote_type;
};

int bk7258_bt_transport_initialize(void);
void bk7258_bt_transport_set_receiver(bk7258_bt_receive_t receive,
                                       void *arg);
int bk7258_bt_transport_open(void);
int bk7258_bt_transport_send(enum bt_buf_type_e type,
                             const void *data, size_t length);
void bk7258_bt_transport_close(void);
void bk7258_bt_transport_get_stats(struct bk7258_bt_stats *stats);
void bk7258_bt_transport_dump_stats(void);

int bk7258_bt_driver_register(void);

#ifdef CONFIG_BK7258_BT_RAW_SELFTEST
int bk7258_bt_raw_selftest_run(void);
#endif

#endif
