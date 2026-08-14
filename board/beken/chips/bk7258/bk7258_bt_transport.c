/****************************************************************************
 * BK7258 Bluetooth HCI-over-Mailbox transport.
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
#include <nuttx/net/bluetooth.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/wireless/bluetooth/bt_buf.h>
#include <nuttx/wireless/bluetooth/bt_hci.h>

#include "bk7258_bt.h"
#include "hardware/bk7258_bt_ipc.h"
#include "hardware/bk7258_mbox.h"

#define BT_TX_PRIORITY             106
#define BT_RX_PRIORITY             104
#define BT_WORKER_STACK            3072
#define BT_RETRY_TICKS             MSEC2TICK(2)
#define BT_VENDOR_TIMEOUT          MSEC2TICK(5000)
#define BT_AP_RAM_START            0x28010000u
#define BT_AP_RAM_END              0x28064000u
#define BT_COMMAND_RESERVE         2u

enum bt_pool_state
{
  BT_POOL_FREE = 0,
  BT_POOL_BUILDING,
  BT_POOL_QUEUED,
  BT_POOL_SENT,
  BT_POOL_ACKED,
  BT_POOL_FREE_SEEN,
  BT_POOL_QUARANTINED
};

enum bt_remote_state
{
  BT_REMOTE_FREE = 0,
  BT_REMOTE_QUEUED,
  BT_REMOTE_FREE_QUEUED,
  BT_REMOTE_FREE_SENT,
  BT_REMOTE_QUARANTINED
};

enum bt_tx_kind
{
  BT_TX_NONE = 0,
  BT_TX_PAYLOAD,
  BT_TX_REMOTE_FREE
};

struct bt_pool_entry
{
  uint8_t data[BLUETOOTH_MAX_FRAMELEN] __attribute__((aligned(4)));
  clock_t enqueued;
  uint32_t generation;
  uint16_t length;
  uint8_t packet_type;
  uint8_t state;
};

struct bt_remote_entry
{
  uint32_t pointer;
  uint8_t packet_type;
  uint8_t state;
};

struct bt_index_queue
{
  uint8_t entries[CONFIG_BK7258_BT_TX_QUEUE];
  uint8_t head;
  uint8_t count;
};

struct bt_control_queue
{
  uint8_t entries[CONFIG_BK7258_BT_TX_BUFFERS];
  uint8_t head;
  uint8_t count;
};

struct bt_rx_queue
{
  uint8_t entries[CONFIG_BK7258_BT_RX_QUEUE];
  uint8_t head;
  uint8_t count;
};

struct bt_current_tx
{
  uint32_t generation;
  uint8_t kind;
  uint8_t index;
  bool valid;
  bool accepted;
};

static struct bt_pool_entry
  g_tx_pool[CONFIG_BK7258_BT_TX_BUFFERS] __attribute__((aligned(32)));
static struct bt_remote_entry g_remote[CONFIG_BK7258_BT_RX_QUEUE];
static struct bt_index_queue g_normal_queue;
static struct bt_control_queue g_control_queue;
static struct bt_rx_queue g_rx_queue;
static struct bt_rx_queue g_free_queue;
static struct bt_current_tx g_current;
static struct bk7258_bt_stats g_stats;
static spinlock_t g_lock = SP_UNLOCKED;
static sem_t g_tx_sem;
static sem_t g_rx_sem;
static sem_t g_vendor_sem;
static sem_t g_pool_sem;
static bk7258_bt_receive_t g_receive;
static void *g_receive_arg;
static uint16_t g_vendor_waiting;
static int g_vendor_status;
static bool g_initialized;
static bool g_workers_created;
static bool g_host_enabled;

_Static_assert(CONFIG_BK7258_BT_TX_BUFFERS <= UINT8_MAX,
               "BT pool index width");
_Static_assert(CONFIG_BK7258_BT_TX_QUEUE <= UINT8_MAX,
               "BT TX queue index width");
_Static_assert(CONFIG_BK7258_BT_RX_QUEUE <= UINT8_MAX,
               "BT RX queue index width");

static uint16_t bt_get_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void bt_put_le16(uint8_t *data, uint16_t value)
{
  data[0] = value & 0xffu;
  data[1] = value >> 8;
}

static void bt_deliver_event(const uint8_t *data, size_t length)
{
  bk7258_bt_receive_t receive;
  void *arg;
  irqstate_t flags;

  flags = spin_lock_irqsave(&g_lock);
  receive = g_receive;
  arg = g_receive_arg;
  spin_unlock_irqrestore(&g_lock, flags);

  if (receive != NULL)
    {
      (void)receive(BT_EVT, data, length, arg);
    }
}

static void bt_inject_command_complete(uint16_t opcode)
{
  uint8_t event[] =
  {
    0x0e, 0x04, 0x01,
    (uint8_t)(opcode & 0xff), (uint8_t)(opcode >> 8), 0x00
  };

  bt_deliver_event(event, sizeof(event));
}

static void bt_inject_acl_complete(uint16_t handle)
{
  uint8_t event[] =
  {
    0x13, 0x05, 0x01,
    (uint8_t)(handle & 0xff), (uint8_t)(handle >> 8),
    0x01, 0x00
  };

  bt_deliver_event(event, sizeof(event));
}

static bool bt_pointer_range(uint32_t pointer, size_t length,
                             uint32_t start, uint32_t end)
{
  return pointer >= start && pointer < end && length <= end - pointer;
}

static bool bt_pool_pointer_valid(const void *pointer, size_t length)
{
  uintptr_t address = (uintptr_t)pointer;

  return address <= UINT32_MAX &&
         bt_pointer_range((uint32_t)address, length,
                          BT_AP_RAM_START, BT_AP_RAM_END);
}

static int bt_queue_push(uint8_t *entries, uint8_t *head, uint8_t *count,
                         unsigned int capacity, uint8_t value)
{
  unsigned int tail;

  if (*count >= capacity)
    {
      return -ENOBUFS;
    }

  tail = (*head + *count) % capacity;
  entries[tail] = value;
  (*count)++;
  return OK;
}

static int bt_queue_pop(uint8_t *entries, uint8_t *head, uint8_t *count,
                        unsigned int capacity, uint8_t *value)
{
  if (*count == 0)
    {
      return -ENOENT;
    }

  *value = entries[*head];
  *head = (*head + 1u) % capacity;
  (*count)--;
  return OK;
}

static void bt_release_pool_locked(unsigned int index)
{
  struct bt_pool_entry *entry = &g_tx_pool[index];

  entry->state = BT_POOL_FREE;
  entry->length = 0;
  entry->packet_type = 0;
  nxsem_post(&g_pool_sem);
}

static int bt_alloc_pool_locked(bool reserved, unsigned int *index)
{
  unsigned int free_count = 0;
  unsigned int candidate = CONFIG_BK7258_BT_TX_BUFFERS;
  unsigned int i;

  for (i = 0; i < CONFIG_BK7258_BT_TX_BUFFERS; i++)
    {
      if (g_tx_pool[i].state == BT_POOL_FREE)
        {
          free_count++;
          if (candidate == CONFIG_BK7258_BT_TX_BUFFERS)
            {
              candidate = i;
            }
        }
    }

  if (candidate == CONFIG_BK7258_BT_TX_BUFFERS ||
      (!reserved && free_count <= BT_COMMAND_RESERVE))
    {
      return -ENOBUFS;
    }

  g_tx_pool[candidate].state = BT_POOL_BUILDING;
  g_tx_pool[candidate].generation++;
  *index = candidate;
  return OK;
}

static void bt_handle_local_free(uint32_t pointer)
{
  irqstate_t flags;
  unsigned int i;

  flags = spin_lock_irqsave(&g_lock);
  for (i = 0; i < CONFIG_BK7258_BT_TX_BUFFERS; i++)
    {
      struct bt_pool_entry *entry = &g_tx_pool[i];

      if ((uint32_t)(uintptr_t)entry->data != pointer)
        {
          continue;
        }

      if (entry->state == BT_POOL_SENT)
        {
          entry->state = BT_POOL_FREE_SEEN;
          g_stats.tx_free++;
          spin_unlock_irqrestore(&g_lock, flags);
          return;
        }

      if (entry->state == BT_POOL_ACKED)
        {
          g_stats.tx_free++;
          bt_release_pool_locked(i);
          spin_unlock_irqrestore(&g_lock, flags);
          return;
        }

      g_stats.tx_unknown_free++;
      spin_unlock_irqrestore(&g_lock, flags);
      return;
    }

  g_stats.tx_unknown_free++;
  spin_unlock_irqrestore(&g_lock, flags);
}

static int bt_mailbox_rx(const struct bk7258_mb_wire_message *message,
                         uint8_t *ack_flags, void *arg)
{
  const union bk7258_mb_frame *frame = (const union bk7258_mb_frame *)message;
  uint32_t pointer = bk7258_bt_descriptor_pointer(&frame->bt);
  irqstate_t flags;
  unsigned int i;
  unsigned int remote_index = CONFIG_BK7258_BT_RX_QUEUE;
  int ret;

  (void)ack_flags;
  (void)arg;

  if (frame->bt.packet_type == BK7258_BT_HCI_FREE_PKT)
    {
      bt_handle_local_free(pointer);
      nxsem_post(&g_tx_sem);
      return OK;
    }

  if (frame->bt.packet_type != BK7258_BT_HCI_EVENT_PKT &&
      frame->bt.packet_type != BK7258_BT_HCI_ACL_DATA_PKT)
    {
      g_stats.rx_rejected++;
      g_stats.last_remote_pointer = pointer;
      g_stats.last_remote_type = frame->bt.packet_type;
      return -EPROTO;
    }

  if ((pointer & 3u) != 0 ||
      !bt_pointer_range(pointer, 2, BK7258_CP_RAM_START,
                        BK7258_CP_RAM_END))
    {
      g_stats.rx_rejected++;
      g_stats.last_remote_pointer = pointer;
      g_stats.last_remote_type = frame->bt.packet_type;
      return -EFAULT;
    }

  flags = spin_lock_irqsave(&g_lock);
  for (i = 0; i < CONFIG_BK7258_BT_RX_QUEUE; i++)
    {
      if (g_remote[i].state != BT_REMOTE_FREE &&
          g_remote[i].pointer == pointer)
        {
          g_stats.rx_duplicate++;
          spin_unlock_irqrestore(&g_lock, flags);
          return -EALREADY;
        }

      if (remote_index == CONFIG_BK7258_BT_RX_QUEUE &&
          g_remote[i].state == BT_REMOTE_FREE)
        {
          remote_index = i;
        }
    }

  if (remote_index == CONFIG_BK7258_BT_RX_QUEUE)
    {
      g_stats.queue_full++;
      spin_unlock_irqrestore(&g_lock, flags);
      return -EAGAIN;
    }

  ret = bt_queue_push(g_rx_queue.entries, &g_rx_queue.head,
                      &g_rx_queue.count, CONFIG_BK7258_BT_RX_QUEUE,
                      remote_index);
  if (ret < 0)
    {
      g_stats.queue_full++;
      spin_unlock_irqrestore(&g_lock, flags);
      return -EAGAIN;
    }

  g_remote[remote_index].pointer = pointer;
  g_remote[remote_index].packet_type = frame->bt.packet_type;
  g_remote[remote_index].state = BT_REMOTE_QUEUED;
  g_stats.rx_enqueued++;
  spin_unlock_irqrestore(&g_lock, flags);
  nxsem_post(&g_rx_sem);
  return OK;
}

static int bt_enqueue_pool(const void *data, size_t length,
                           uint8_t packet_type, bool priority, bool reserved,
                           unsigned int *pool_index,
                           uint32_t *generation)
{
  struct bt_pool_entry *entry;
  irqstate_t flags;
  unsigned int index;
  int ret;

  flags = spin_lock_irqsave(&g_lock);
  ret = bt_alloc_pool_locked(reserved, &index);
  spin_unlock_irqrestore(&g_lock, flags);
  if (ret < 0)
    {
      return ret;
    }

  entry = &g_tx_pool[index];
  if (!bt_pool_pointer_valid(entry->data, length))
    {
      flags = spin_lock_irqsave(&g_lock);
      bt_release_pool_locked(index);
      spin_unlock_irqrestore(&g_lock, flags);
      return -EFAULT;
    }

  memcpy(entry->data, data, length);
  __asm__ volatile("dmb sy" ::: "memory");

  flags = spin_lock_irqsave(&g_lock);
  entry->length = length;
  entry->packet_type = packet_type;
  entry->enqueued = clock_systime_ticks();
  entry->state = BT_POOL_QUEUED;

  if (priority)
    {
      ret = bt_queue_push(g_control_queue.entries, &g_control_queue.head,
                          &g_control_queue.count,
                          CONFIG_BK7258_BT_TX_BUFFERS, index);
    }
  else
    {
      ret = bt_queue_push(g_normal_queue.entries, &g_normal_queue.head,
                          &g_normal_queue.count,
                          CONFIG_BK7258_BT_TX_QUEUE, index);
    }

  if (ret < 0)
    {
      bt_release_pool_locked(index);
      g_stats.queue_full++;
      spin_unlock_irqrestore(&g_lock, flags);
      return ret;
    }

  g_stats.tx_enqueued++;
  if (packet_type == BK7258_BT_HCI_COMMAND_PKT)
    {
      g_stats.tx_commands++;
    }
  else if (packet_type == BK7258_BT_HCI_ACL_DATA_PKT)
    {
      g_stats.tx_acl++;
    }
  if (pool_index != NULL)
    {
      *pool_index = index;
    }

  if (generation != NULL)
    {
      *generation = entry->generation;
    }

  spin_unlock_irqrestore(&g_lock, flags);
  nxsem_post(&g_tx_sem);
  return OK;
}

static void bt_tx_complete(const struct bk7258_mb_wire_message *ack,
                           int result, void *arg)
{
  irqstate_t flags;
  struct bt_current_tx current;
  bool acl_complete = false;
  uint16_t acl_handle = 0;

  (void)ack;
  (void)arg;

  flags = spin_lock_irqsave(&g_lock);
  current = g_current;
  memset(&g_current, 0, sizeof(g_current));

  if (!current.valid || !current.accepted)
    {
      spin_unlock_irqrestore(&g_lock, flags);
      return;
    }

  if (current.kind == BT_TX_PAYLOAD &&
      current.index < CONFIG_BK7258_BT_TX_BUFFERS)
    {
      struct bt_pool_entry *entry = &g_tx_pool[current.index];

      if (entry->generation == current.generation)
        {
          if (result == OK)
            {
              g_stats.tx_ack++;
              if (entry->packet_type == BK7258_BT_HCI_ACL_DATA_PKT &&
                  entry->length >= 4)
                {
                  acl_handle = bt_get_le16(&entry->data[0]) & 0x0fffu;
                  acl_complete = true;
                }
              if (entry->state == BT_POOL_FREE_SEEN)
                {
                  bt_release_pool_locked(current.index);
                }
              else if (entry->state == BT_POOL_SENT)
                {
                  entry->state = BT_POOL_ACKED;
                }
            }
          else if (entry->state == BT_POOL_FREE_SEEN)
            {
              bt_release_pool_locked(current.index);
            }
          else
            {
              entry->state = BT_POOL_QUARANTINED;
              g_stats.tx_ack_error++;
              g_stats.tx_quarantined++;
            }
        }
    }
  else if (current.kind == BT_TX_REMOTE_FREE &&
           current.index < CONFIG_BK7258_BT_RX_QUEUE)
    {
      struct bt_remote_entry *remote = &g_remote[current.index];

      if (result == OK && remote->state == BT_REMOTE_FREE_SENT)
        {
          memset(remote, 0, sizeof(*remote));
          g_stats.rx_free_ack++;
        }
      else if (remote->state != BT_REMOTE_FREE)
        {
          remote->state = BT_REMOTE_QUARANTINED;
          g_stats.rx_free_quarantined++;
        }
    }

  spin_unlock_irqrestore(&g_lock, flags);

  if (acl_complete)
    {
      bt_inject_acl_complete(acl_handle);
    }

  nxsem_post(&g_tx_sem);
}

static bool bt_select_current_locked(void)
{
  uint8_t index;

  if (g_current.valid)
    {
      return true;
    }

  if (bt_queue_pop(g_free_queue.entries, &g_free_queue.head,
                   &g_free_queue.count, CONFIG_BK7258_BT_RX_QUEUE,
                   &index) == OK)
    {
      g_current.kind = BT_TX_REMOTE_FREE;
      g_current.index = index;
      g_current.valid = true;
      g_remote[index].state = BT_REMOTE_FREE_SENT;
      return true;
    }

  if (bt_queue_pop(g_control_queue.entries, &g_control_queue.head,
                   &g_control_queue.count, CONFIG_BK7258_BT_TX_BUFFERS,
                   &index) == OK ||
      bt_queue_pop(g_normal_queue.entries, &g_normal_queue.head,
                   &g_normal_queue.count, CONFIG_BK7258_BT_TX_QUEUE,
                   &index) == OK)
    {
      g_current.kind = BT_TX_PAYLOAD;
      g_current.index = index;
      g_current.generation = g_tx_pool[index].generation;
      g_current.valid = true;
      g_tx_pool[index].state = BT_POOL_SENT;
      return true;
    }

  return false;
}

static int bt_dispatch_current(void)
{
  union bk7258_mb_frame frame;
  struct bt_current_tx current;
  uint32_t pointer;
  irqstate_t flags;
  int ret;

  flags = spin_lock_irqsave(&g_lock);
  if (!bt_select_current_locked() || g_current.accepted)
    {
      spin_unlock_irqrestore(&g_lock, flags);
      return OK;
    }

  g_current.accepted = true;
  current = g_current;
  if (current.kind == BT_TX_PAYLOAD)
    {
      pointer = (uint32_t)(uintptr_t)g_tx_pool[current.index].data;
    }
  else
    {
      pointer = g_remote[current.index].pointer;
    }

  memset(&frame, 0, sizeof(frame));
  frame.bt.packet_type = current.kind == BT_TX_REMOTE_FREE ?
                         BK7258_BT_HCI_FREE_PKT :
                         g_tx_pool[current.index].packet_type;
  bk7258_bt_descriptor_set_pointer(&frame.bt, pointer);
  spin_unlock_irqrestore(&g_lock, flags);

  __asm__ volatile("dmb sy" ::: "memory");
  ret = bk7258_mailbox_send_raw(BK7258_MB_CHAN_BT_TX, frame.raw,
                                bt_tx_complete, NULL);

  flags = spin_lock_irqsave(&g_lock);
  if (ret != OK && g_current.valid &&
      g_current.kind == current.kind && g_current.index == current.index &&
      g_current.generation == current.generation)
    {
      if (ret != -EBUSY && ret != -EAGAIN)
        {
          if (current.kind == BT_TX_PAYLOAD)
            {
              g_tx_pool[current.index].state = BT_POOL_QUARANTINED;
              g_stats.tx_quarantined++;
            }
          else
            {
              g_remote[current.index].state = BT_REMOTE_QUARANTINED;
              g_stats.rx_free_quarantined++;
            }

          memset(&g_current, 0, sizeof(g_current));
        }
      else
        {
          g_current.accepted = false;
        }
    }

  spin_unlock_irqrestore(&g_lock, flags);
  return ret;
}

static int bt_tx_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  for (;;)
    {
      int ret;

      nxsem_wait_uninterruptible(&g_tx_sem);
      nxsem_reset(&g_tx_sem, 0);
      ret = bt_dispatch_current();
      if (ret == -EBUSY || ret == -EAGAIN)
        {
          nxsig_usleep(TICK2USEC(BT_RETRY_TICKS));
          nxsem_post(&g_tx_sem);
        }
    }

  return OK;
}

static bool bt_vendor_event(const uint8_t *data, size_t length)
{
  irqstate_t flags;
  uint16_t subopcode;
  bool vendor;

  if (length != 5 || data[0] != BK7258_BT_VENDOR_EVENT || data[1] != 3)
    {
      return false;
    }

  subopcode = bt_get_le16(&data[2]);
  if (subopcode != BK7258_BT_VENDOR_INIT &&
      subopcode != BK7258_BT_VENDOR_DEINIT)
    {
      return false;
    }

  flags = spin_lock_irqsave(&g_lock);
  vendor = true;
  if (g_vendor_waiting == subopcode)
    {
      g_vendor_status = data[4] == 0 ? OK : -EREMOTEIO;
      g_vendor_waiting = 0;
      nxsem_post(&g_vendor_sem);
    }
  else
    {
      syslog(LOG_WARNING, "bluetooth: stray vendor event subop=%04x\n",
             subopcode);
    }

  spin_unlock_irqrestore(&g_lock, flags);
  return vendor;
}

static int bt_read_remote(unsigned int index, uint8_t *data, size_t *length)
{
  const uint8_t *remote;
  uint32_t pointer = g_remote[index].pointer;
  size_t total;

  __asm__ volatile("dmb sy" ::: "memory");
  remote = (const uint8_t *)(uintptr_t)pointer;

  if (g_remote[index].packet_type == BK7258_BT_HCI_EVENT_PKT)
    {
      uint8_t header[2];

      memcpy(header, remote, sizeof(header));
      total = sizeof(header) + header[1];
    }
  else
    {
      uint8_t header[4];

      if (!bt_pointer_range(pointer, sizeof(header), BK7258_CP_RAM_START,
                            BK7258_CP_RAM_END))
        {
          return -EFAULT;
        }

      memcpy(header, remote, sizeof(header));
      total = sizeof(header) + bt_get_le16(&header[2]);
    }

  if (total > BLUETOOTH_MAX_FRAMELEN ||
      !bt_pointer_range(pointer, total, BK7258_CP_RAM_START,
                        BK7258_CP_RAM_END))
    {
      return -EMSGSIZE;
    }

  memcpy(data, remote, total);

  if (g_remote[index].packet_type == BK7258_BT_HCI_ACL_DATA_PKT &&
      total >= 4)
    {
      uint16_t handle = bt_get_le16(data);

      if ((handle >> 12) == 0)
        {
          bt_put_le16(data, handle | 0x2000);
        }
    }

  *length = total;
  return OK;
}

static void bt_queue_remote_free(unsigned int index)
{
  irqstate_t flags;
  int ret;

  flags = spin_lock_irqsave(&g_lock);
  g_remote[index].state = BT_REMOTE_FREE_QUEUED;
  ret = bt_queue_push(g_free_queue.entries, &g_free_queue.head,
                      &g_free_queue.count, CONFIG_BK7258_BT_RX_QUEUE,
                      index);
  if (ret < 0)
    {
      g_remote[index].state = BT_REMOTE_QUARANTINED;
      g_stats.rx_free_quarantined++;
    }

  spin_unlock_irqrestore(&g_lock, flags);
  nxsem_post(&g_tx_sem);
}

static void bt_process_remote(unsigned int index)
{
  uint8_t data[BLUETOOTH_MAX_FRAMELEN];
  enum bt_buf_type_e type;
  size_t length;
  int ret;

  ret = bt_read_remote(index, data, &length);
  if (ret < 0)
    {
      irqstate_t flags = spin_lock_irqsave(&g_lock);

      g_stats.rx_rejected++;
      g_remote[index].state = BT_REMOTE_QUARANTINED;
      g_stats.rx_free_quarantined++;
      spin_unlock_irqrestore(&g_lock, flags);
      return;
    }

  if (g_remote[index].packet_type == BK7258_BT_HCI_EVENT_PKT &&
      bt_vendor_event(data, length))
    {
      bt_queue_remote_free(index);
      return;
    }

  type = g_remote[index].packet_type == BK7258_BT_HCI_EVENT_PKT ?
         BT_EVT : BT_ACL_IN;
  if (type == BT_EVT)
    {
      g_stats.rx_events++;
      if (data[0] == 0x3eu || data[0] == 0x05u)
        {
          printf("bt-ipc: event=%02x sub/status=%02x len=%lu\n",
                 data[0], length >= 3 ? data[2] : 0,
                 (unsigned long)length);
        }
    }
  else
    {
      g_stats.rx_acl++;
      printf("bt-ipc: rx-acl len=%lu\n", (unsigned long)length);
    }
  if (g_receive != NULL && (g_host_enabled || type == BT_EVT))
    {
      ret = g_receive(type, data, length, g_receive_arg);
      if (ret >= 0)
        {
          g_stats.rx_delivered++;
        }
      else
        {
          g_stats.rx_rejected++;
        }
    }
  else
    {
      g_stats.rx_rejected++;
    }

  if (type == BT_EVT && (data[0] == 0x05u || data[0] == 0x3eu))
    {
      bk7258_bt_transport_dump_stats();
    }

  bt_queue_remote_free(index);
}

static int bt_rx_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  for (;;)
    {
      uint8_t index;
      irqstate_t flags;
      int ret;

      nxsem_wait_uninterruptible(&g_rx_sem);
      for (;;)
        {
          flags = spin_lock_irqsave(&g_lock);
          ret = bt_queue_pop(g_rx_queue.entries, &g_rx_queue.head,
                             &g_rx_queue.count, CONFIG_BK7258_BT_RX_QUEUE,
                             &index);
          spin_unlock_irqrestore(&g_lock, flags);
          if (ret < 0)
            {
              break;
            }

          bt_process_remote(index);
          bk7258_mbox_kick_rx();
        }

      nxsem_reset(&g_rx_sem, 0);
    }

  return OK;
}

static int bt_wait_pool(unsigned int index, uint32_t generation,
                        clock_t timeout)
{
  clock_t deadline = clock_systime_ticks() + timeout;

  for (;;)
    {
      irqstate_t flags = spin_lock_irqsave(&g_lock);
      bool released = g_tx_pool[index].generation == generation &&
                      g_tx_pool[index].state == BT_POOL_FREE;
      clock_t now = clock_systime_ticks();

      spin_unlock_irqrestore(&g_lock, flags);
      if (released)
        {
          return OK;
        }

      if ((int32_t)(now - deadline) >= 0)
        {
          return -ETIMEDOUT;
        }

      (void)nxsem_tickwait_uninterruptible(&g_pool_sem, deadline - now);
    }
}

static int bt_vendor_control(uint16_t subopcode)
{
  uint8_t command[5];
  unsigned int index;
  uint32_t generation;
  irqstate_t flags;
  int ret;

  bt_put_le16(command, BK7258_BT_VENDOR_OPCODE);
  command[2] = 2;
  command[3] = subopcode >> 8;
  command[4] = subopcode & 0xffu;

  nxsem_reset(&g_vendor_sem, 0);
  flags = spin_lock_irqsave(&g_lock);
  g_vendor_waiting = subopcode;
  g_vendor_status = -EINPROGRESS;
  spin_unlock_irqrestore(&g_lock, flags);

  ret = bt_enqueue_pool(command, sizeof(command),
                        BK7258_BT_HCI_COMMAND_PKT, true, true,
                        &index, &generation);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&g_lock);
      g_vendor_waiting = 0;
      spin_unlock_irqrestore(&g_lock, flags);
      return ret;
    }

  ret = nxsem_tickwait_uninterruptible(&g_vendor_sem, BT_VENDOR_TIMEOUT);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&g_lock);
  ret = g_vendor_status;
  spin_unlock_irqrestore(&g_lock, flags);
  if (ret < 0)
    {
      return ret;
    }

  return bt_wait_pool(index, generation, BT_VENDOR_TIMEOUT);
}

int bk7258_bt_transport_initialize(void)
{
  int ret;

  if (g_initialized)
    {
      return OK;
    }

  memset(g_tx_pool, 0, sizeof(g_tx_pool));
  memset(g_remote, 0, sizeof(g_remote));
  memset(&g_normal_queue, 0, sizeof(g_normal_queue));
  memset(&g_control_queue, 0, sizeof(g_control_queue));
  memset(&g_rx_queue, 0, sizeof(g_rx_queue));
  memset(&g_free_queue, 0, sizeof(g_free_queue));
  memset(&g_current, 0, sizeof(g_current));
  memset(&g_stats, 0, sizeof(g_stats));
  nxsem_init(&g_tx_sem, 0, 0);
  nxsem_init(&g_rx_sem, 0, 0);
  nxsem_init(&g_vendor_sem, 0, 0);
  nxsem_init(&g_pool_sem, 0, 0);

  ret = bk7258_mailbox_register_rx(BK7258_MB_CHAN_BT_RX,
                                   bt_mailbox_rx, NULL);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_workers_created)
    {
      ret = kthread_create("bt-ipc-tx", BT_TX_PRIORITY, BT_WORKER_STACK,
                           bt_tx_worker, NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = kthread_create("bt-ipc-rx", BT_RX_PRIORITY, BT_WORKER_STACK,
                           bt_rx_worker, NULL);
      if (ret < 0)
        {
          return ret;
        }

      g_workers_created = true;
    }

  g_initialized = true;
  return OK;
}

void bk7258_bt_transport_set_receiver(bk7258_bt_receive_t receive, void *arg)
{
  irqstate_t flags = spin_lock_irqsave(&g_lock);

  g_receive = receive;
  g_receive_arg = arg;
  spin_unlock_irqrestore(&g_lock, flags);
}

int bk7258_bt_transport_open(void)
{
  int ret;

  if (!g_initialized || !bk7258_mailbox_link_ready())
    {
      return -ENOLINK;
    }

  ret = bt_vendor_control(BK7258_BT_VENDOR_INIT);
  if (ret == OK)
    {
      irqstate_t flags = spin_lock_irqsave(&g_lock);

      g_host_enabled = true;
      spin_unlock_irqrestore(&g_lock, flags);
    }

  return ret;
}

int bk7258_bt_transport_send(enum bt_buf_type_e type,
                             const void *data, size_t length)
{
  uint8_t packet_type;
  irqstate_t flags;
  bool enabled;
  int ret;

  if (data == NULL || length > BLUETOOTH_MAX_FRAMELEN)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_lock);
  enabled = g_host_enabled;
  spin_unlock_irqrestore(&g_lock, flags);
  if (!enabled)
    {
      return -ESHUTDOWN;
    }

  if (type == BT_CMD)
    {
      const uint8_t *command = data;
      uint16_t opcode;

      if (length < 3 || command[2] != length - 3)
        {
          return -EMSGSIZE;
        }

      packet_type = BK7258_BT_HCI_COMMAND_PKT;
      opcode = bt_get_le16(command);
      printf("bt-ipc: tx-cmd opcode=%04x len=%lu\n",
             (unsigned int)opcode, (unsigned long)length);

      if (opcode == BT_HCI_OP_HOST_BUFFER_SIZE)
        {
          bt_inject_command_complete(opcode);
          return (int)length;
        }

      if (opcode == BT_HCI_OP_HOST_NUM_COMPLETED_PACKETS)
        {
          return (int)length;
        }

    }
  else if (type == BT_ACL_OUT)
    {
      const uint8_t *acl = data;

      if (length < 4 || bt_get_le16(&acl[2]) != length - 4)
        {
          return -EMSGSIZE;
        }

      packet_type = BK7258_BT_HCI_ACL_DATA_PKT;
      printf("bt-ipc: tx-acl len=%lu\n", (unsigned long)length);
    }
  else
    {
      return -EINVAL;
    }

  ret = bt_enqueue_pool(data, length, packet_type, false, type == BT_CMD,
                        NULL, NULL);
  return ret < 0 ? ret : (int)length;
}

void bk7258_bt_transport_close(void)
{
  irqstate_t flags = spin_lock_irqsave(&g_lock);

  g_host_enabled = false;
  spin_unlock_irqrestore(&g_lock, flags);
  (void)bt_vendor_control(BK7258_BT_VENDOR_DEINIT);
}

void bk7258_bt_transport_get_stats(struct bk7258_bt_stats *stats)
{
  irqstate_t flags;

  if (stats == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(&g_lock);
  *stats = g_stats;
  spin_unlock_irqrestore(&g_lock, flags);
}

void bk7258_bt_transport_dump_stats(void)
{
  struct bk7258_bt_stats stats;

  bk7258_bt_transport_get_stats(&stats);
  printf("bt-ipc: tx=%lu cmd=%lu acl=%lu ack=%lu ackerr=%lu free=%lu "
         "unknown_free=%lu quarantine=%lu rx=%lu evt=%lu acl=%lu "
         "delivered=%lu rejected=%lu duplicate=%lu "
         "free_ack=%lu free_quarantine=%lu queue_full=%lu "
         "last_remote=%08lx/%02x\n",
         (unsigned long)stats.tx_enqueued,
         (unsigned long)stats.tx_commands,
         (unsigned long)stats.tx_acl,
         (unsigned long)stats.tx_ack,
         (unsigned long)stats.tx_ack_error,
         (unsigned long)stats.tx_free,
         (unsigned long)stats.tx_unknown_free,
         (unsigned long)stats.tx_quarantined,
         (unsigned long)stats.rx_enqueued,
         (unsigned long)stats.rx_events,
         (unsigned long)stats.rx_acl,
         (unsigned long)stats.rx_delivered,
         (unsigned long)stats.rx_rejected,
         (unsigned long)stats.rx_duplicate,
         (unsigned long)stats.rx_free_ack,
         (unsigned long)stats.rx_free_quarantined,
         (unsigned long)stats.queue_full,
         (unsigned long)stats.last_remote_pointer,
         stats.last_remote_type);
}
