/****************************************************************************
 * BK7258 Bluetooth HCI-over-Mailbox wire ABI.
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_BT_IPC_H
#define __VENDOR_BEKEN_CHIPS_BK7258_HARDWARE_BK7258_BT_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/bk7258_mbox.h"

#define BK7258_BT_HCI_COMMAND_PKT   0x01u
#define BK7258_BT_HCI_ACL_DATA_PKT  0x02u
#define BK7258_BT_HCI_SCO_DATA_PKT  0x03u
#define BK7258_BT_HCI_EVENT_PKT     0x04u
#define BK7258_BT_HCI_FREE_PKT      0x0au

#define BK7258_BT_VENDOR_OPCODE     0xfefeu
#define BK7258_BT_VENDOR_EVENT      0xfeu
#define BK7258_BT_VENDOR_INIT       0x0001u
#define BK7258_BT_VENDOR_DEINIT     0x0002u

struct bk7258_bt_descriptor
{
  uint32_t header;
  uint8_t packet_type;
  uint8_t pointer[4];
  uint8_t reserved[7];
} __attribute__((packed));

union bk7258_mb_frame
{
  struct bk7258_mb_wire_message generic;
  struct bk7258_bt_descriptor bt;
  uint8_t raw[BK7258_MB_MESSAGE_SIZE];
};

_Static_assert(sizeof(struct bk7258_bt_descriptor) ==
               BK7258_MB_MESSAGE_SIZE, "BT descriptor size");
_Static_assert(sizeof(union bk7258_mb_frame) == BK7258_MB_MESSAGE_SIZE,
               "mailbox frame size");
_Static_assert(offsetof(struct bk7258_bt_descriptor, packet_type) == 4,
               "BT packet type offset");
_Static_assert(offsetof(struct bk7258_bt_descriptor, pointer) == 5,
               "BT pointer offset");

static inline uint32_t
bk7258_bt_descriptor_pointer(const struct bk7258_bt_descriptor *descriptor)
{
  uint32_t pointer;

  memcpy(&pointer, descriptor->pointer, sizeof(pointer));
  return pointer;
}

static inline void
bk7258_bt_descriptor_set_pointer(struct bk7258_bt_descriptor *descriptor,
                                 uint32_t pointer)
{
  memcpy(descriptor->pointer, &pointer, sizeof(pointer));
}

#endif
