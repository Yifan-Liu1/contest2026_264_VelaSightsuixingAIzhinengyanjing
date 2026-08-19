/****************************************************************************
 * BK7258 AP-side Wi-Fi controller-interface driver.
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_BK7258_WIFI

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kthread.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/wireless/wireless.h>

#include <net/if_arp.h>

#include "bk7258_driver.h"
#include "hardware/bk7258_mbox.h"
#include "hardware/bk7258_wifi_ipc.h"
#include "bk7258_wifi.h"

#define WIFI_NODE_QUEUE          16u
#define WIFI_RECYCLE_QUEUE       16u
#define WIFI_PACKET_QUEUE        4u
#define WIFI_COMMAND_TIMEOUT     MSEC2TICK(2000)
#define WIFI_ROLE_STOP_TIMEOUT   MSEC2TICK(3000)
#define WIFI_WORK_INTERVAL       MSEC2TICK(20)
#define WIFI_PBUF_TYPE_RAM       0x80u
#define WIFI_EVENT_DATA_OFFSET   12u
#define WIFI_STA_STATUS_CONNECTED 3u
#if defined(CONFIG_WIRELESS_WAPI_SCAN_MAX_DATA) && \
    CONFIG_WIRELESS_WAPI_SCAN_MAX_DATA < 4096
#  define WIFI_SCAN_CACHE_LIMIT CONFIG_WIRELESS_WAPI_SCAN_MAX_DATA
#else
#  define WIFI_SCAN_CACHE_LIMIT 4096
#endif

struct wifi_command_slot
{
  uint8_t bytes[BK7258_WIFI_CMD_SLOT_SIZE] __attribute__((aligned(32)));
  bool owned;
  bool transport_pending;
  uint16_t command;
  uint16_t sequence;
  uint32_t generation;
};

struct wifi_tx_slot
{
  struct bk7258_wifi_pbuf pbuf;
  uint8_t headroom[BK7258_WIFI_TX_HEADROOM];
  uint8_t frame[BK7258_WIFI_MAX_FRAME];
  netpkt_t *packet;
  bool active;
  bool transport_pending;
  bool transport_rejected;
  uint8_t vif;
  uint32_t role_epoch;
} __attribute__((aligned(32)));

struct wifi_rx_packet
{
  uint8_t frame[BK7258_WIFI_MAX_FRAME];
  uint16_t length;
  uint32_t role_epoch;
};

enum wifi_role
{
  WIFI_ROLE_NONE = 0,
  WIFI_ROLE_STA,
  WIFI_ROLE_SOFTAP
};

enum wifi_role_state
{
  WIFI_ROLE_IDLE = 0,
  WIFI_ROLE_STARTING,
  WIFI_ROLE_ACTIVE,
  WIFI_ROLE_STOPPING
};

struct wifi_role_config
{
  char ssid[33];
  char password[64];
  int32_t wpa_version;
  int32_t cipher;
  uint8_t channel;
};

struct wifi_pending_node
{
  struct bk7258_wifi_ipc_node node;
  uint32_t generation;
  uint32_t role_epoch;
  uint8_t recycle_index;
  bool recycle_reserved;
};

enum wifi_recycle_state
{
  WIFI_RECYCLE_FREE = 0,
  WIFI_RECYCLE_RESERVED,
  WIFI_RECYCLE_QUEUED,
  WIFI_RECYCLE_SENDING,
  WIFI_RECYCLE_DISCARD
};

struct wifi_recycle_entry
{
  struct bk7258_wifi_ipc_node node;
  uint32_t generation;
  enum wifi_recycle_state state;
};

struct wifi_net_notifications
{
  netpkt_t *tx_packets[CONFIG_BK7258_WIFI_TX_SLOTS];
  uint8_t tx_count;
  bool rxready;
};

struct wifi_driver
{
  struct netdev_lowerhalf_s lower;
  mutex_t command_lock;
  mutex_t packet_lock;
  mutex_t scan_lock;
  sem_t command_sem;
  sem_t role_sem;
  sem_t work_sem;
  sem_t ready_sem;
  struct wifi_pending_node nodes[WIFI_NODE_QUEUE];
  struct wifi_recycle_entry recycle[WIFI_RECYCLE_QUEUE];
  struct wifi_rx_packet packets[WIFI_PACKET_QUEUE];
  struct wifi_command_slot command[BK7258_WIFI_CMD_SLOTS];
  struct wifi_tx_slot tx[CONFIG_BK7258_WIFI_TX_SLOTS];
  uint8_t node_head;
  uint8_t node_tail;
  uint8_t node_count;
  uint8_t recycle_head;
  uint8_t recycle_tail;
  uint8_t recycle_count;
  uint8_t packet_head;
  uint8_t packet_tail;
  uint8_t packet_count;
  uint16_t next_sequence;
  uint16_t waiting_id;
  uint16_t waiting_sequence;
  int command_result;
  uint16_t command_length;
  uint8_t command_data[256];
  uint8_t *scan_cache;
  uint16_t scan_cache_length;
  struct wifi_role_config sta_config;
  struct wifi_role_config ap_config;
  enum wifi_role_state role_state;
  bool admin_up;
  enum wifi_role configured_role;
  enum wifi_role active_role;
  bool ap_started;
  uint8_t ap_client_count;
  uint8_t sta_mac[6];
  uint8_t ap_mac[6];
  uint32_t role_epoch;
  bool tx_gate;
  bool initialized;
  bool carrier;
  bool carrier_notified;
  bool scan_running;
  bool scan_cached;
  uint32_t reset_generation;
};

static struct wifi_driver g_wifi;
static volatile int g_worker_result;
static unsigned int g_wifi_rx_trace;
static unsigned int g_wifi_tx_trace;

_Static_assert(sizeof(g_wifi.command_data) >=
               sizeof(struct bk7258_wifi_scan_page_response),
               "command response buffer must hold one scan page");
_Static_assert(sizeof(struct bk7258_wifi_scan_page_response) <= 256,
               "scan page must fit the short controller event");
_Static_assert(WIFI_SCAN_CACHE_LIMIT > 0 && WIFI_SCAN_CACHE_LIMIT <= 4096,
               "scan cache must be bounded to 4096 bytes");

static bool wifi_cp_range(uint32_t address, size_t length)
{
  return address >= BK7258_CP_RAM_START &&
         address <= BK7258_CP_RAM_END &&
         length <= (size_t)(BK7258_CP_RAM_END - address);
}

static bool wifi_cp_pointer(uint32_t address, size_t length)
{
  return (address & 3u) == 0 && wifi_cp_range(address, length);
}

static int wifi_send_node(uint8_t mailbox_channel, uint8_t node_channel,
                          uint32_t head, uint32_t tail, uint8_t count,
                          bk7258_mb_tx_complete_t callback, void *arg)
{
  struct bk7258_mb_wire_message message;
  struct bk7258_wifi_ipc_node node;

  memset(&node, 0, sizeof(node));
  node.head = head;
  node.tail = tail;
  node.channel = node_channel;
  node.num = count;
  memcpy(&message, &node, sizeof(message));
  __asm__ volatile("dmb sy" ::: "memory");
  return bk7258_mailbox_send_wire(mailbox_channel, &message, callback, arg);
}

static int wifi_send_node_async(uint8_t mailbox_channel,
                                const struct bk7258_wifi_ipc_node *node,
                                bk7258_mb_tx_complete_t callback, void *arg)
{
  struct bk7258_mb_wire_message message;

  memcpy(&message, node, sizeof(message));
  __asm__ volatile("dmb sy" ::: "memory");
  return bk7258_mailbox_send_wire(mailbox_channel, &message, callback, arg);
}

static struct wifi_command_slot *wifi_command_alloc(void)
{
  unsigned int i;

  for (i = 0; i < BK7258_WIFI_CMD_SLOTS; i++)
    {
      uint32_t *pattern = (uint32_t *)g_wifi.command[i].bytes;

      if (!g_wifi.command[i].owned &&
          !g_wifi.command[i].transport_pending &&
          *pattern == BK7258_WIFI_CMD_PATTERN_FREE)
        {
          g_wifi.command[i].owned = true;
          *pattern = BK7258_WIFI_CMD_PATTERN_BUSY;
          return &g_wifi.command[i];
        }
    }

  return NULL;
}

static void wifi_command_reap(void)
{
  unsigned int i;

  for (i = 0; i < BK7258_WIFI_CMD_SLOTS; i++)
    {
      uint32_t pattern = *(uint32_t *)g_wifi.command[i].bytes;

      if (g_wifi.command[i].owned &&
          !g_wifi.command[i].transport_pending &&
          pattern == BK7258_WIFI_CMD_PATTERN_FREE)
        {
          g_wifi.command[i].owned = false;
        }
    }
}

static void wifi_command_transport_complete(
  const struct bk7258_mb_wire_message *ack, int result, void *arg)
{
  struct wifi_command_slot *slot = arg;
  bool wake = false;
  irqstate_t flags;

  (void)ack;
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (slot->transport_pending)
    {
      slot->transport_pending = false;
      if (result == -EREMOTEIO && slot->owned)
        {
          *(uint32_t *)slot->bytes = BK7258_WIFI_CMD_PATTERN_FREE;
          slot->owned = false;
          if (g_wifi.waiting_id ==
                slot->command + BK7258_WIFI_CFM_OFFSET &&
              g_wifi.waiting_sequence == slot->sequence)
            {
              g_wifi.command_result = -EREMOTEIO;
              g_wifi.waiting_id = 0;
              wake = true;
            }
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (wake)
    {
      nxsem_post(&g_wifi.command_sem);
    }
}

static int wifi_command(uint16_t command, const void *payload,
                         uint16_t payload_length, void *result,
                         uint16_t result_size, uint16_t *result_length)
{
  struct wifi_command_slot *slot;
  struct bk7258_wifi_cpdu *cpdu;
  struct bk7258_wifi_msg_hdr *header;
  uint16_t sequence;
  int ret;

  if (payload_length > BK7258_WIFI_CMD_SLOT_SIZE - sizeof(uint32_t) -
                       sizeof(*cpdu) - sizeof(*header))
    {
      return -E2BIG;
    }

  if (!bk7258_mailbox_link_ready())
    {
      return -ENOLINK;
    }

  nxmutex_lock(&g_wifi.command_lock);
  nxmutex_lock(&g_wifi.packet_lock);
  wifi_command_reap();
  slot = wifi_command_alloc();
  if (slot == NULL)
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      nxmutex_unlock(&g_wifi.command_lock);
      return -ENOMEM;
    }

  cpdu = (struct bk7258_wifi_cpdu *)(slot->bytes + sizeof(uint32_t));
  header = (struct bk7258_wifi_msg_hdr *)(cpdu + 1);
  sequence = ++g_wifi.next_sequence;
  memset(cpdu, 0, sizeof(*cpdu) + sizeof(*header) + payload_length);
  cpdu->length = sizeof(*cpdu) + sizeof(*header) + payload_length;
  header->cmd_id = command;
  header->cmd_sn = sequence;
  header->length = payload_length;
  slot->command = command;
  slot->sequence = sequence;
  slot->generation = bk7258_mailbox_peer_reset_generation();
  slot->transport_pending = true;
  if (payload != NULL && payload_length != 0)
    {
      memcpy(header + 1, payload, payload_length);
    }

  while (nxsem_trywait(&g_wifi.command_sem) == OK)
    {
    }

  {
    irqstate_t flags = rspin_lock_irqsave(&g_bk7258_driver_lock);

    g_wifi.waiting_id = command + BK7258_WIFI_CFM_OFFSET;
    g_wifi.waiting_sequence = sequence;
    g_wifi.command_result = -ETIMEDOUT;
    g_wifi.command_length = 0;
    rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  }
  ret = wifi_send_node(BK7258_WIFI_CMD_TX_CHANNEL, 0,
                       (uint32_t)(uintptr_t)cpdu,
                       (uint32_t)(uintptr_t)cpdu, 1,
                       wifi_command_transport_complete, slot);
  if (ret < 0)
    {
      *(uint32_t *)slot->bytes = BK7258_WIFI_CMD_PATTERN_FREE;
      slot->owned = false;
      slot->transport_pending = false;
      {
        irqstate_t flags = rspin_lock_irqsave(&g_bk7258_driver_lock);

        if (g_wifi.waiting_id == command + BK7258_WIFI_CFM_OFFSET &&
            g_wifi.waiting_sequence == sequence)
          {
            g_wifi.waiting_id = 0;
          }
        rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      }
      nxmutex_unlock(&g_wifi.packet_lock);
      nxmutex_unlock(&g_wifi.command_lock);
      return ret;
    }
  nxmutex_unlock(&g_wifi.packet_lock);

  ret = nxsem_tickwait_uninterruptible(&g_wifi.command_sem,
                                       WIFI_COMMAND_TIMEOUT);
  {
    irqstate_t flags = rspin_lock_irqsave(&g_bk7258_driver_lock);

    if (g_wifi.waiting_id == command + BK7258_WIFI_CFM_OFFSET &&
        g_wifi.waiting_sequence == sequence)
      {
        g_wifi.waiting_id = 0;
      }
    if (ret >= 0)
      {
        ret = g_wifi.command_result;
      }
    rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  }

  if (ret >= 0 && result != NULL && result_size != 0)
    {
      uint16_t copy = g_wifi.command_length < result_size ?
                      g_wifi.command_length : result_size;
      memcpy(result, g_wifi.command_data, copy);
    }
  if (ret >= 0 && result_length != NULL)
    {
      *result_length = g_wifi.command_length;
    }

  nxmutex_lock(&g_wifi.packet_lock);
  wifi_command_reap();
  nxmutex_unlock(&g_wifi.packet_lock);
  nxmutex_unlock(&g_wifi.command_lock);
  return ret;
}

static int wifi_ap_status_query(
  struct bk7258_wifi_ap_status_response *response)
{
  uint16_t result_length = 0;
  int ret;

  memset(response, 0, sizeof(*response));
  ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_AP_STATUS, NULL, 0,
                     response, sizeof(*response), &result_length);
  if (ret < 0)
    {
      return ret;
    }
  if (result_length != sizeof(*response) ||
      response->reserved[0] != 0 || response->reserved[1] != 0 ||
      response->started > 1 ||
      response->client_count > BK7258_WIFI_AP_MAX_CLIENTS ||
      response->security > BK7258_WIFI_AP_SECURITY_WPA2)
    {
      return -EPROTO;
    }

  return response->status;
}

static int wifi_sta_status_connected(bool *connected)
{
  uint8_t status = 0;
  uint16_t result_length = 0;
  int ret;

  ret = wifi_command(BK7258_WIFI_CMD_GET_WLAN_STATUS, NULL, 0,
                     &status, sizeof(status), &result_length);
  if (ret < 0)
    {
      return ret;
    }
  if (result_length < sizeof(status))
    {
      return -EPROTO;
    }

  *connected = status == WIFI_STA_STATUS_CONNECTED;
  return OK;
}

static void wifi_set_carrier(bool carrier)
{
  bool wake = false;
  irqstate_t flags;

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.carrier != carrier)
    {
      g_wifi.carrier = carrier;
      wake = true;
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  if (wake)
    {
      nxsem_post(&g_wifi.work_sem);
    }
}

static void wifi_notify_carrier(void)
{
  bool carrier;
  irqstate_t flags;

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (!g_wifi.initialized || g_wifi.carrier_notified == g_wifi.carrier)
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return;
    }
  carrier = g_wifi.carrier;
  g_wifi.carrier_notified = carrier;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (carrier)
    {
      netdev_lower_carrier_on(&g_wifi.lower);
    }
  else
    {
      netdev_lower_carrier_off(&g_wifi.lower);
    }
}

static uint8_t wifi_role_vif(enum wifi_role role)
{
  return role == WIFI_ROLE_SOFTAP ? BK7258_WIFI_WIRE_VIF_SOFTAP :
                                    BK7258_WIFI_WIRE_VIF_STA;
}

static bool wifi_tx_busy(void)
{
  unsigned int i;

  for (i = 0; i < CONFIG_BK7258_WIFI_TX_SLOTS; i++)
    {
      if (g_wifi.tx[i].active || g_wifi.tx[i].transport_pending)
        {
          return true;
        }
    }

  return false;
}

static void wifi_role_deactivate(enum wifi_role role)
{
  bool deactivated = false;
  irqstate_t flags;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (role == WIFI_ROLE_NONE || g_wifi.active_role == role)
    {
      g_wifi.active_role = WIFI_ROLE_NONE;
      g_wifi.role_state = WIFI_ROLE_IDLE;
      g_wifi.tx_gate = false;
      g_wifi.ap_started = false;
      g_wifi.ap_client_count = 0;
      g_wifi.role_epoch++;
      g_wifi.packet_head = 0;
      g_wifi.packet_tail = 0;
      g_wifi.packet_count = 0;
      deactivated = true;
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  if (deactivated)
    {
      nxsem_post(&g_wifi.role_sem);
      wifi_set_carrier(false);
    }
}

static void wifi_sta_event(bool connected)
{
  bool carrier = false;
  bool handled = false;
  bool stopped = false;
  irqstate_t flags;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.active_role == WIFI_ROLE_STA)
    {
      handled = true;
      if (connected && g_wifi.role_state != WIFI_ROLE_STOPPING)
        {
          g_wifi.role_state = WIFI_ROLE_ACTIVE;
          g_wifi.tx_gate = g_wifi.admin_up;
          carrier = g_wifi.admin_up;
        }
      else if (!connected)
        {
          g_wifi.tx_gate = false;
          g_wifi.role_epoch++;
          g_wifi.packet_head = 0;
          g_wifi.packet_tail = 0;
          g_wifi.packet_count = 0;
          if (g_wifi.role_state == WIFI_ROLE_STOPPING)
            {
              g_wifi.active_role = WIFI_ROLE_NONE;
              g_wifi.role_state = WIFI_ROLE_IDLE;
              stopped = true;
            }
          else
            {
              /* CP auto-reconnect keeps the STA role alive after a transient
               * link loss.  The next connected event can restore carrier. */

              g_wifi.role_state = WIFI_ROLE_STARTING;
            }
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  if (stopped)
    {
      nxsem_post(&g_wifi.role_sem);
    }
  if (handled)
    {
      wifi_set_carrier(carrier);
    }
}

static void wifi_ap_start_event(bool started)
{
  bool carrier = false;
  bool handled = false;
  bool stopped = false;
  irqstate_t flags;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.active_role == WIFI_ROLE_SOFTAP)
    {
      handled = true;
      if (started && g_wifi.role_state != WIFI_ROLE_STOPPING)
        {
          g_wifi.role_state = WIFI_ROLE_ACTIVE;
          g_wifi.ap_started = true;
          g_wifi.ap_client_count = 0;
          g_wifi.tx_gate = g_wifi.admin_up;
          carrier = g_wifi.admin_up;
        }
      else if (!started)
        {
          g_wifi.active_role = WIFI_ROLE_NONE;
          g_wifi.role_state = WIFI_ROLE_IDLE;
          g_wifi.ap_started = false;
          g_wifi.ap_client_count = 0;
          g_wifi.tx_gate = false;
          g_wifi.role_epoch++;
          g_wifi.packet_head = 0;
          g_wifi.packet_tail = 0;
          g_wifi.packet_count = 0;
          stopped = true;
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  if (stopped)
    {
      nxsem_post(&g_wifi.role_sem);
    }
  if (handled)
    {
      wifi_set_carrier(carrier);
    }
}

static void wifi_ap_client_event(bool associated)
{
  irqstate_t flags;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.active_role == WIFI_ROLE_SOFTAP && g_wifi.ap_started &&
      g_wifi.role_state == WIFI_ROLE_ACTIVE)
    {
      if (associated &&
          g_wifi.ap_client_count < BK7258_WIFI_AP_MAX_CLIENTS)
        {
          g_wifi_rx_trace = 0;
          g_wifi_tx_trace = 0;
          g_wifi.ap_client_count++;
          printf("bk7258_wifi: AP client associated, count=%u\n",
                 g_wifi.ap_client_count);
        }
      else if (!associated && g_wifi.ap_client_count != 0)
        {
          g_wifi.ap_client_count--;
          printf("bk7258_wifi: AP client disassociated, count=%u\n",
                 g_wifi.ap_client_count);
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
}

static void wifi_scan_cache_clear(void)
{
  kmm_free(g_wifi.scan_cache);
  g_wifi.scan_cache = NULL;
  g_wifi.scan_cache_length = 0;
  g_wifi.scan_cached = false;
}

static void wifi_link_down_state(void)
{
  bool wake_command = false;
  irqstate_t flags;

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.waiting_id != 0)
    {
      g_wifi.command_result = -ECONNRESET;
      g_wifi.waiting_id = 0;
      wake_command = true;
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  if (wake_command)
    {
      nxsem_post(&g_wifi.command_sem);
    }

  wifi_role_deactivate(WIFI_ROLE_NONE);
  nxmutex_lock(&g_wifi.scan_lock);
  wifi_scan_cache_clear();
  g_wifi.scan_running = false;
  nxmutex_unlock(&g_wifi.scan_lock);
}

static bool wifi_sync_reset_generation(void)
{
  struct wifi_pending_node nodes[WIFI_NODE_QUEUE];
  uint32_t generation = bk7258_mailbox_peer_reset_generation();
  bool wake_command = false;
  unsigned int kept = 0;
  unsigned int i;
  irqstate_t flags;

  if (generation == g_wifi.reset_generation)
    {
      return false;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.waiting_id != 0)
    {
      g_wifi.command_result = -ECONNRESET;
      g_wifi.waiting_id = 0;
      wake_command = true;
    }

  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  if (wake_command)
    {
      nxsem_post(&g_wifi.command_sem);
    }

  wifi_role_deactivate(WIFI_ROLE_NONE);
  nxmutex_lock(&g_wifi.scan_lock);
  wifi_scan_cache_clear();
  g_wifi.scan_running = false;
  nxmutex_unlock(&g_wifi.scan_lock);

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  generation = bk7258_mailbox_peer_reset_generation();

  /* A peer generation change proves the old CP can no longer reference AP
   * command slots.  TX slots remain quarantined because DMA completion is
   * not proven by a CP reset.
   */

  for (i = 0; i < BK7258_WIFI_CMD_SLOTS; i++)
    {
      if (g_wifi.command[i].owned &&
          g_wifi.command[i].generation != generation)
        {
          *(uint32_t *)g_wifi.command[i].bytes =
            BK7258_WIFI_CMD_PATTERN_FREE;
          g_wifi.command[i].owned = false;
          g_wifi.command[i].transport_pending = false;
        }
    }
  g_wifi.reset_generation = generation;
  for (i = 0; i < g_wifi.node_count; i++)
    {
      unsigned int index = (g_wifi.node_head + i) % WIFI_NODE_QUEUE;

      if (g_wifi.nodes[index].generation == generation)
        {
          nodes[kept++] = g_wifi.nodes[index];
        }
    }
  memcpy(g_wifi.nodes, nodes, kept * sizeof(nodes[0]));
  g_wifi.node_head = 0;
  g_wifi.node_tail = kept % WIFI_NODE_QUEUE;
  g_wifi.node_count = kept;
  g_wifi.packet_head = 0;
  g_wifi.packet_tail = 0;
  g_wifi.packet_count = 0;
  for (i = 0; i < WIFI_RECYCLE_QUEUE; i++)
    {
      if (g_wifi.recycle[i].state != WIFI_RECYCLE_FREE &&
          g_wifi.recycle[i].generation != generation)
        {
          g_wifi.recycle[i].state = WIFI_RECYCLE_DISCARD;
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  nxsem_post(&g_wifi.work_sem);
  return true;
}

static int wifi_scan_cache_append(const void *data, size_t length)
{
  if (length > WIFI_SCAN_CACHE_LIMIT - g_wifi.scan_cache_length)
    {
      return -E2BIG;
    }

  memcpy(g_wifi.scan_cache + g_wifi.scan_cache_length, data, length);
  g_wifi.scan_cache_length += length;
  return OK;
}

static int wifi_scan_cache_event(uint16_t command, const void *data,
                                 size_t length)
{
  struct iw_event event;

  memset(&event, 0, sizeof(event));
  event.cmd = command;
  event.len = offsetof(struct iw_event, u) + length;
  if (length > sizeof(event.u))
    {
      return -E2BIG;
    }
  memcpy(&event.u, data, length);
  return wifi_scan_cache_append(&event, event.len);
}

static int wifi_scan_cache_build_record(
  const struct bk7258_wifi_scan_record *record)
{
  struct iw_event event;
  struct iw_quality quality;
  struct iw_freq frequency;
  uint16_t flags;
  size_t ssid_length;
  size_t padded_length;
  size_t event_length;
  int ret;

  memset(&event, 0, sizeof(event));
  event.cmd = SIOCGIWAP;
  event.len = IW_EV_LEN(ap_addr);
  event.u.ap_addr.sa_family = ARPHRD_ETHER;
  memcpy(event.u.ap_addr.sa_data, record->bssid, sizeof(record->bssid));
  ret = wifi_scan_cache_append(&event, event.len);
  if (ret < 0)
    {
      return ret;
    }

  ssid_length = strnlen(record->ssid, sizeof(record->ssid));
  padded_length = (ssid_length + 3u) & ~3u;
  memset(&event, 0, sizeof(event));
  event.cmd = SIOCGIWESSID;
  event.u.essid.flags = IW_ESSID_ON;
  event.u.essid.length = ssid_length;
  event.u.essid.pointer = (void *)(uintptr_t)sizeof(struct iw_point);
  event_length = offsetof(struct iw_event, u) + sizeof(struct iw_point) +
                 padded_length;
  event.len = event_length;
  ret = wifi_scan_cache_append(&event, offsetof(struct iw_event, u) +
                               sizeof(struct iw_point));
  if (ret < 0)
    {
      return ret;
    }
  ret = wifi_scan_cache_append(record->ssid, ssid_length);
  if (ret < 0)
    {
      return ret;
    }
  if (padded_length != ssid_length)
    {
      uint32_t padding = 0;
      ret = wifi_scan_cache_append(&padding, padded_length - ssid_length);
      if (ret < 0)
        {
          return ret;
        }
    }

  memset(&quality, 0, sizeof(quality));
  quality.level = (uint8_t)record->rssi;
  quality.updated = IW_QUAL_DBM;
  ret = wifi_scan_cache_event(IWEVQUAL, &quality, sizeof(quality));
  if (ret < 0)
    {
      return ret;
    }

  memset(&frequency, 0, sizeof(frequency));
  frequency.m = record->channel;
  ret = wifi_scan_cache_event(SIOCGIWFREQ, &frequency, sizeof(frequency));
  if (ret < 0)
    {
      return ret;
    }

  flags = record->security == 0 ? IW_ENCODE_DISABLED : IW_ENCODE_ENABLED;
  memset(&event, 0, sizeof(event));
  event.cmd = SIOCGIWENCODE;
  event.len = IW_EV_LEN(data);
  event.u.data.flags = flags;
  return wifi_scan_cache_append(&event, event.len);
}

static int wifi_scan_cache_fetch(void)
{
  struct bk7258_wifi_scan_page_request request;
  struct bk7258_wifi_scan_page_response response;
  uint16_t start = 0;
  uint16_t total = 0;
  uint16_t result_length;
  int ret;

  g_wifi.scan_cache = kmm_malloc(WIFI_SCAN_CACHE_LIMIT);
  if (g_wifi.scan_cache == NULL)
    {
      return -ENOMEM;
    }

  do
    {
      memset(&request, 0, sizeof(request));
      request.start = start;
      request.max_records = BK7258_WIFI_SCAN_PAGE_RECORDS;
      memset(&response, 0, sizeof(response));
      ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_SCAN_PAGE,
                         &request, sizeof(request), &response,
                         sizeof(response), &result_length);
      if (ret < 0)
        {
          printf("wifi scan page transport failed: %d\n", ret);
          wifi_scan_cache_clear();
          return ret;
        }
      if (result_length != sizeof(response) || response.status != 0 ||
          response.page.start != start ||
          response.page.count > BK7258_WIFI_SCAN_PAGE_RECORDS ||
          response.page.reserved != 0 ||
          response.page.total < start ||
          response.page.count > response.page.total - start)
        {
          printf("wifi scan page invalid: status=%ld start=%u count=%u "
                 "total=%u length=%u\n",
                 (long)response.status, response.page.start,
                 response.page.count, response.page.total, result_length);
          wifi_scan_cache_clear();
          return response.status != 0 ? response.status : -EPROTO;
        }

      if (start == 0)
        {
          total = response.page.total;
        }
      else if (response.page.total != total)
        {
          wifi_scan_cache_clear();
          return -EPROTO;
        }

      for (uint8_t i = 0; i < response.page.count; i++)
        {
          ret = wifi_scan_cache_build_record(&response.page.records[i]);
          if (ret < 0)
            {
              wifi_scan_cache_clear();
              return ret;
            }
        }

      start += response.page.count;
      if (response.page.more != (start < total) ||
          (response.page.count == 0 && start < total))
        {
          wifi_scan_cache_clear();
          return -EPROTO;
        }
    }
  while (start < total);

  g_wifi.scan_cached = true;
  return OK;
}

static void wifi_release_cp_command(uint32_t cpdu_address)
{
  uint32_t pattern_address = cpdu_address - sizeof(uint32_t);

  if (wifi_cp_pointer(pattern_address, sizeof(uint32_t)))
    {
      *(uint32_t *)(uintptr_t)pattern_address =
        BK7258_WIFI_CMD_PATTERN_FREE;
      __asm__ volatile("dmb sy" ::: "memory");
    }
}

static void wifi_handle_command(uint32_t cpdu_address,
                                uint32_t generation)
{
  struct bk7258_wifi_cpdu cpdu;
  struct bk7258_wifi_event_hdr event;
  uint8_t data[sizeof(g_wifi.command_data)];
  uint32_t event_address;
  uint16_t length;
  uint16_t copied;
  irqstate_t flags;

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (generation != bk7258_mailbox_peer_reset_generation() ||
      !wifi_cp_pointer(cpdu_address, sizeof(cpdu)))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return;
    }

  memcpy(&cpdu, (const void *)(uintptr_t)cpdu_address, sizeof(cpdu));
  event_address = cpdu_address + sizeof(cpdu);
  if (cpdu.length < sizeof(cpdu) + sizeof(event) ||
      !wifi_cp_range(event_address, sizeof(event)))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return;
    }

  memcpy(&event, (const void *)(uintptr_t)event_address, sizeof(event));
  length = event.length;
  if (length > cpdu.length - sizeof(cpdu) - sizeof(event) ||
      !wifi_cp_range(event_address + WIFI_EVENT_DATA_OFFSET, length))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return;
    }

  copied = length < sizeof(data) ? length : sizeof(data);
  if (copied != 0)
    {
      memcpy(data, (const void *)(uintptr_t)(event_address +
                                              WIFI_EVENT_DATA_OFFSET),
             copied);
    }
  wifi_release_cp_command(cpdu_address);
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (event.id >= BK7258_WIFI_CFM_OFFSET)
    {
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);

      if (event.id == g_wifi.waiting_id &&
          event.sequence == g_wifi.waiting_sequence)
        {
          if (copied != 0)
            {
              memcpy(g_wifi.command_data, data, copied);
            }
          g_wifi.command_length = copied;
          g_wifi.command_result = OK;
          g_wifi.waiting_id = 0;
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          nxsem_post(&g_wifi.command_sem);
        }
      else
        {
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
        }
    }
  else if (event.id == BK7258_WIFI_EVT_IPV4_IND)
    {
      wifi_sta_event(true);
    }
  else if (event.id == BK7258_WIFI_EVT_WIFI_EVENT_IND && copied >= 4)
    {
      uint16_t event_id;

      memcpy(&event_id, data, sizeof(event_id));
      if (event_id == BK7258_WIFI_EVENT_CONNECTED)
        {
          wifi_sta_event(true);
        }
      else if (event_id == BK7258_WIFI_EVENT_DISCONNECTED)
        {
          wifi_sta_event(false);
        }
    }
  else if (event.id == BK7258_WIFI_EVT_DISCONNECT_IND)
    {
      wifi_sta_event(false);
    }
  else if (event.id == BK7258_WIFI_EVT_START_AP_IND && copied >= 1)
    {
      wifi_ap_start_event(data[0] == 0);
    }
  else if (event.id == BK7258_WIFI_EVT_ASSOC_AP_IND && copied >= 6)
    {
      wifi_ap_client_event(true);
    }
  else if (event.id == BK7258_WIFI_EVT_DISASSOC_AP_IND && copied >= 6)
    {
      wifi_ap_client_event(false);
    }
  else if (event.id == BK7258_WIFI_EVT_STOP_AP_IND)
    {
      wifi_role_deactivate(WIFI_ROLE_SOFTAP);
    }
  else if (event.id == BK7258_WIFI_EVT_SCAN_WIFI_IND)
    {
      g_wifi.scan_running = false;
    }

}

static bool wifi_validate_command(uint32_t cpdu_address,
                                  uint32_t *next, uint32_t generation)
{
  struct bk7258_wifi_cpdu cpdu;
  struct bk7258_wifi_event_hdr event;
  uint32_t event_address;
  irqstate_t flags;

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (generation != bk7258_mailbox_peer_reset_generation() ||
      !wifi_cp_pointer(cpdu_address, sizeof(cpdu)))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return false;
    }

  memcpy(&cpdu, (const void *)(uintptr_t)cpdu_address, sizeof(cpdu));
  event_address = cpdu_address + sizeof(cpdu);
  if (cpdu.length < sizeof(cpdu) + sizeof(event) ||
      !wifi_cp_range(event_address, sizeof(event)))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return false;
    }

  memcpy(&event, (const void *)(uintptr_t)event_address, sizeof(event));
  if (event.length > cpdu.length - sizeof(cpdu) - sizeof(event) ||
      !wifi_cp_range(event_address + WIFI_EVENT_DATA_OFFSET, event.length))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return false;
    }

  *next = cpdu.next;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  return true;
}

static void wifi_handle_command_list(const struct bk7258_wifi_ipc_node *node,
                                     uint32_t generation)
{
  uint32_t addresses[BK7258_WIFI_MAX_LIST];
  uint32_t address = node->head;
  unsigned int count;

  if (node->channel != 0 || node->num == 0 ||
      node->num > BK7258_WIFI_MAX_LIST)
    {
      return;
    }

  for (count = 0; count < node->num; count++)
    {
      uint32_t next;
      unsigned int previous;

      if (!wifi_validate_command(address, &next, generation))
        {
          return;
        }
      for (previous = 0; previous < count; previous++)
        {
          if (addresses[previous] == address)
            {
              return;
            }
        }
      if ((count + 1 == node->num) != (address == node->tail))
        {
          return;
        }

      addresses[count] = address;
      address = next;
    }

  if (address != 0)
    {
      return;
    }

  for (count = 0; count < node->num; count++)
    {
      if (generation != bk7258_mailbox_peer_reset_generation())
        {
          return;
        }

      wifi_handle_command(addresses[count], generation);
    }
}

static struct wifi_tx_slot *wifi_tx_slot(uint32_t cpdu_address)
{
  unsigned int i;

  for (i = 0; i < CONFIG_BK7258_WIFI_TX_SLOTS; i++)
    {
      struct bk7258_wifi_cpdu *cpdu =
        (struct bk7258_wifi_cpdu *)g_wifi.tx[i].headroom;

      if (cpdu_address == (uint32_t)(uintptr_t)cpdu)
        {
          return &g_wifi.tx[i];
        }
    }

  return NULL;
}

static void wifi_tx_transport_complete(
  const struct bk7258_mb_wire_message *ack, int result, void *arg)
{
  struct wifi_tx_slot *tx = arg;
  bool wake = false;
  irqstate_t flags;

  (void)ack;
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (tx->transport_pending)
    {
      tx->transport_pending = false;
      if (result == -EREMOTEIO && tx->active)
        {
          tx->transport_rejected = true;
          wake = true;
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  if (wake)
    {
      nxsem_post(&g_wifi.work_sem);
    }
}

static void wifi_notify_network(struct wifi_net_notifications *notifications)
{
  unsigned int i;

  for (i = 0; i < notifications->tx_count; i++)
    {
      netpkt_free(&g_wifi.lower, notifications->tx_packets[i], NETPKT_TX);
      netdev_lower_txdone(&g_wifi.lower);
    }
  if (notifications->rxready)
    {
      netdev_lower_rxready(&g_wifi.lower);
    }
}

static void wifi_collect_rejected_tx(
  struct wifi_net_notifications *notifications)
{
  irqstate_t flags;
  unsigned int i;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  for (i = 0; i < CONFIG_BK7258_WIFI_TX_SLOTS; i++)
    {
      struct wifi_tx_slot *tx = &g_wifi.tx[i];

      if (tx->active && tx->transport_rejected)
        {
          notifications->tx_packets[notifications->tx_count++] = tx->packet;
          tx->packet = NULL;
          tx->active = false;
          tx->transport_rejected = false;
        }
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
}

static bool wifi_copy_rx_frame(uint32_t cpdu_address,
                               struct wifi_rx_packet *packet,
                               uint8_t *vif)
{
  struct bk7258_wifi_cpdu cpdu;
  struct bk7258_wifi_pbuf pbuf;
  uint32_t pbuf_address;

  if (!wifi_cp_pointer(cpdu_address, sizeof(cpdu)) ||
      cpdu_address < BK7258_CP_RAM_START + sizeof(pbuf))
    {
      return false;
    }

  memcpy(&cpdu, (const void *)(uintptr_t)cpdu_address, sizeof(cpdu));
  pbuf_address = cpdu_address - sizeof(pbuf);
  if (!wifi_cp_pointer(pbuf_address, sizeof(pbuf)))
    {
      return false;
    }

  memcpy(&pbuf, (const void *)(uintptr_t)pbuf_address, sizeof(pbuf));
  if ((cpdu.flags & 1u) != 0 || pbuf.next != 0 || pbuf.len == 0 ||
      pbuf.len > BK7258_WIFI_MAX_FRAME || pbuf.tot_len != pbuf.len ||
      pbuf.ref == 0 || pbuf.ref == UINT8_MAX ||
      !wifi_cp_pointer(pbuf.payload, pbuf.len))
    {
      return false;
    }

  if (packet != NULL)
    {
      memcpy(packet->frame, (const void *)(uintptr_t)pbuf.payload, pbuf.len);
      packet->length = pbuf.len;
    }
  if (vif != NULL)
    {
      *vif = bk7258_wifi_cpdu_get_vif(cpdu.flags);
    }
  return true;
}

static void wifi_recycle_append(struct bk7258_wifi_ipc_node *node,
                                uint32_t cpdu_address)
{
  struct bk7258_wifi_cpdu *cpdu =
    (struct bk7258_wifi_cpdu *)(uintptr_t)cpdu_address;
  uint32_t pbuf_address;
  struct bk7258_wifi_pbuf *pbuf;

  pbuf_address = cpdu_address - sizeof(*pbuf);
  pbuf = (struct bk7258_wifi_pbuf *)(uintptr_t)pbuf_address;

  /* Match wdrv_txdata_sender(): CP drops one reference before pbuf_free(). */

  pbuf->ref++;
  cpdu->flags |= 1u;
  cpdu->next = 0;
  if (node->tail != 0)
    {
      ((struct bk7258_wifi_cpdu *)(uintptr_t)node->tail)->next =
        cpdu_address;
    }
  else
    {
      node->head = cpdu_address;
    }

  node->tail = cpdu_address;
  node->num++;
}

static void wifi_recycle_complete(
  const struct bk7258_mb_wire_message *ack, int result, void *arg)
{
  struct wifi_recycle_entry *entry = arg;
  irqstate_t flags = rspin_lock_irqsave(&g_bk7258_driver_lock);

  (void)ack;
  if (entry->state == WIFI_RECYCLE_SENDING)
    {
      if (result == OK || entry->generation !=
          bk7258_mailbox_peer_reset_generation())
        {
          entry->state = WIFI_RECYCLE_DISCARD;
        }
      else
        {
          entry->state = WIFI_RECYCLE_QUEUED;
        }
    }

  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxsem_post(&g_wifi.work_sem);
}

static void wifi_flush_recycle(void)
{
  struct wifi_recycle_entry *entry;
  irqstate_t flags;
  int ret;

  for (;;)
    {
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
      if (g_wifi.recycle_count == 0)
        {
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          return;
        }

      entry = &g_wifi.recycle[g_wifi.recycle_head];
      if (entry->generation != g_wifi.reset_generation)
        {
          entry->state = WIFI_RECYCLE_DISCARD;
        }

      if (entry->state == WIFI_RECYCLE_DISCARD)
        {
          memset(entry, 0, sizeof(*entry));
          g_wifi.recycle_head = (g_wifi.recycle_head + 1) %
                                WIFI_RECYCLE_QUEUE;
          g_wifi.recycle_count--;
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          bk7258_mbox_kick_rx();
          continue;
        }

      if (entry->state != WIFI_RECYCLE_QUEUED ||
          !bk7258_mailbox_link_ready())
        {
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          return;
        }

      entry->state = WIFI_RECYCLE_SENDING;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      ret = wifi_send_node_async(BK7258_WIFI_DATA_TX_CHANNEL, &entry->node,
                                 wifi_recycle_complete, entry);
      if (ret < 0)
        {
          flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
          if (entry->state == WIFI_RECYCLE_SENDING)
            {
              entry->state = WIFI_RECYCLE_QUEUED;
            }
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
        }

      return;
    }
}

static void wifi_handle_data(const struct wifi_pending_node *pending,
                             struct wifi_net_notifications *notifications)
{
  const struct bk7258_wifi_ipc_node *node = &pending->node;
  uint32_t addresses[BK7258_WIFI_MAX_LIST];
  bool cp_owned[BK7258_WIFI_MAX_LIST];
  struct wifi_recycle_entry *recycle;
  uint32_t address = node->head;
  uint8_t packet_added = 0;
  unsigned int count;
  irqstate_t flags;

  if (!pending->recycle_reserved || node->channel != 2 ||
      node->num == 0 || node->num > BK7258_WIFI_MAX_LIST)
    {
      return;
    }

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  recycle = &g_wifi.recycle[pending->recycle_index];
  if (pending->generation != g_wifi.reset_generation ||
      pending->generation != bk7258_mailbox_peer_reset_generation())
    {
      recycle->state = WIFI_RECYCLE_DISCARD;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      return;
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  /* First pass validates the complete list without changing either core's
   * ownership.  A malformed list is left untouched.
   */

  for (count = 0; count < node->num; count++)
    {
      struct bk7258_wifi_cpdu cpdu;
      struct wifi_tx_slot *tx;
      uint32_t next;
      unsigned int previous;

      if (address == 0)
        {
          goto invalid;
        }

      for (previous = 0; previous < count; previous++)
        {
          if (addresses[previous] == address)
            {
              goto invalid;
            }
        }

      if ((count + 1 == node->num) != (address == node->tail))
        {
          goto invalid;
        }

      addresses[count] = address;
      tx = wifi_tx_slot(address);
      if (tx != NULL)
        {
          struct bk7258_wifi_cpdu *ap_cpdu =
            (struct bk7258_wifi_cpdu *)tx->headroom;

          if (!tx->active || (ap_cpdu->flags & 1u) == 0 ||
              bk7258_wifi_cpdu_get_vif(ap_cpdu->flags) != tx->vif)
            {
              goto invalid;
            }

          /* Completion is accepted by slot address even after role_epoch
           * changes.  A role cannot start while any TX slot is busy, and
           * rejecting a late completion here would leak the retained packet.
           */

          cp_owned[count] = false;
          next = ap_cpdu->next;
        }
      else
        {
          flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
          if (pending->generation !=
                bk7258_mailbox_peer_reset_generation() ||
              !wifi_cp_pointer(address, sizeof(cpdu)) ||
              !wifi_copy_rx_frame(address, NULL, NULL))
            {
              rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
              goto invalid;
            }

          memcpy(&cpdu, (const void *)(uintptr_t)address, sizeof(cpdu));
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          cp_owned[count] = true;
          next = cpdu.next;
        }

      address = next;
    }

  if (address != 0)
    {
      goto invalid;
    }

  for (count = 0; count < node->num; count++)
    {
      if (cp_owned[count] &&
          g_wifi.packet_count + packet_added < WIFI_PACKET_QUEUE)
        {
          unsigned int index = (g_wifi.packet_tail + packet_added) %
                               WIFI_PACKET_QUEUE;
          uint8_t vif;

          flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
          if (pending->generation ==
                bk7258_mailbox_peer_reset_generation() &&
              pending->role_epoch == g_wifi.role_epoch &&
              g_wifi.role_state == WIFI_ROLE_ACTIVE &&
              g_wifi.active_role != WIFI_ROLE_NONE &&
              wifi_copy_rx_frame(addresses[count], &g_wifi.packets[index],
                                 &vif) &&
              vif == wifi_role_vif(g_wifi.active_role))
            {
              if (g_wifi_rx_trace < 16)
                {
                  const uint8_t *frame = g_wifi.packets[index].frame;
                  uint16_t type = ((uint16_t)frame[12] << 8) | frame[13];

                  printf("bk7258_wifi: AP RX vif=%u len=%u eth=0x%04x\n",
                         vif, g_wifi.packets[index].length, type);
                  g_wifi_rx_trace++;
                }
              g_wifi.packets[index].role_epoch = pending->role_epoch;
              packet_added++;
            }
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
        }
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (pending->generation != bk7258_mailbox_peer_reset_generation())
    {
      recycle->state = WIFI_RECYCLE_DISCARD;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      return;
    }

  memset(&recycle->node, 0, sizeof(recycle->node));
  recycle->node.channel = 2;
  for (count = 0; count < node->num; count++)
    {
      if (cp_owned[count] &&
          recycle->node.num < BK7258_WIFI_MAX_LIST)
        {
          wifi_recycle_append(&recycle->node, addresses[count]);
        }
    }

  __asm__ volatile("dmb sy" ::: "memory");
  recycle->state = recycle->node.num == 0 ? WIFI_RECYCLE_DISCARD :
                                            WIFI_RECYCLE_QUEUED;
  g_wifi.packet_tail = (g_wifi.packet_tail + packet_added) %
                       WIFI_PACKET_QUEUE;
  g_wifi.packet_count += packet_added;

  for (count = 0; count < node->num; count++)
    {
      if (!cp_owned[count])
        {
          struct wifi_tx_slot *tx = wifi_tx_slot(addresses[count]);

          notifications->tx_packets[notifications->tx_count++] = tx->packet;
          tx->packet = NULL;
          tx->active = false;
          tx->transport_rejected = false;
        }
    }
  if (packet_added != 0)
    {
      notifications->rxready = true;
    }
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  nxsem_post(&g_wifi.work_sem);
  return;

invalid:
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  recycle->state = WIFI_RECYCLE_DISCARD;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
}


static int wifi_mailbox_rx(const struct bk7258_mb_wire_message *message,
                           uint8_t *ack_flags, void *arg)
{
  struct bk7258_wifi_ipc_node node;
  struct wifi_pending_node *pending;
  irqstate_t flags;

  (void)ack_flags;
  (void)arg;
  memcpy(&node, message, sizeof(node));
  if (node.num == 0 || node.num > BK7258_WIFI_MAX_LIST)
    {
      return -EINVAL;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (g_wifi.node_count == WIFI_NODE_QUEUE ||
      (node.channel == 2 &&
       g_wifi.recycle_count == WIFI_RECYCLE_QUEUE))
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      return -EAGAIN;
    }

  pending = &g_wifi.nodes[g_wifi.node_tail];
  memset(pending, 0, sizeof(*pending));
  pending->node = node;
  pending->generation = bk7258_mailbox_peer_reset_generation();
  pending->role_epoch = g_wifi.role_epoch;
  if (node.channel == 2)
    {
      struct wifi_recycle_entry *entry =
        &g_wifi.recycle[g_wifi.recycle_tail];

      memset(entry, 0, sizeof(*entry));
      entry->generation = pending->generation;
      entry->state = WIFI_RECYCLE_RESERVED;
      pending->recycle_index = g_wifi.recycle_tail;
      pending->recycle_reserved = true;
      g_wifi.recycle_tail = (g_wifi.recycle_tail + 1) %
                            WIFI_RECYCLE_QUEUE;
      g_wifi.recycle_count++;
    }

  g_wifi.node_tail = (g_wifi.node_tail + 1) % WIFI_NODE_QUEUE;
  g_wifi.node_count++;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxsem_post(&g_wifi.work_sem);
  return OK;
}

static void wifi_link_changed(enum bk7258_mb_link_state state, void *arg)
{
  (void)arg;
  if (state != BK7258_MB_LINK_READY)
    {
      wifi_link_down_state();
    }
  (void)wifi_sync_reset_generation();
}

static int wifi_worker(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  g_worker_result = OK;
  nxsem_post(&g_wifi.ready_sem);

  for (;;)
    {
      struct wifi_pending_node pending;
      struct wifi_net_notifications notifications;
      irqstate_t flags;

      (void)nxsem_tickwait_uninterruptible(&g_wifi.work_sem,
                                           WIFI_WORK_INTERVAL);
      memset(&notifications, 0, sizeof(notifications));
      (void)wifi_sync_reset_generation();
      wifi_collect_rejected_tx(&notifications);
      wifi_flush_recycle();
      for (;;)
        {
          if (bk7258_mailbox_peer_reset_generation() !=
              g_wifi.reset_generation)
            {
              (void)wifi_sync_reset_generation();
            }

          nxmutex_lock(&g_wifi.packet_lock);
          flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
          if (g_wifi.node_count == 0)
            {
              rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
              nxmutex_unlock(&g_wifi.packet_lock);
              break;
            }

          pending = g_wifi.nodes[g_wifi.node_head];
          g_wifi.node_head = (g_wifi.node_head + 1) % WIFI_NODE_QUEUE;
          g_wifi.node_count--;
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          nxmutex_unlock(&g_wifi.packet_lock);
          if (pending.generation != g_wifi.reset_generation)
            {
              if (pending.recycle_reserved)
                {
                  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
                  g_wifi.recycle[pending.recycle_index].state =
                    WIFI_RECYCLE_DISCARD;
                  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
                }
            }
          else if (pending.node.channel == 0)
            {
              wifi_handle_command_list(&pending.node,
                                       pending.generation);
            }
          else if (pending.node.channel == 2)
            {
              wifi_handle_data(&pending, &notifications);
            }
          bk7258_mbox_kick_rx();
        }

      wifi_flush_recycle();
      wifi_notify_network(&notifications);
      wifi_notify_carrier();
    }

  return OK;
}

static int wifi_ifup(struct netdev_lowerhalf_s *lower)
{
  bool carrier;
  irqstate_t flags;

  (void)lower;
  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_wifi.admin_up = true;
  carrier = g_wifi.role_state == WIFI_ROLE_ACTIVE &&
            (g_wifi.active_role == WIFI_ROLE_STA || g_wifi.ap_started);
  g_wifi.tx_gate = carrier;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  wifi_set_carrier(carrier);
  return OK;
}

static int wifi_stop_active_role(void)
{
  struct bk7258_wifi_ap_status_response ap_status;
  enum wifi_role role;
  enum wifi_role_state previous_state;
  int32_t status = 0;
  uint16_t result_length = 0;
  irqstate_t flags;
  int ret;

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  role = g_wifi.active_role;
  if (role == WIFI_ROLE_NONE)
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      wifi_set_carrier(false);
      return OK;
    }
  if (g_wifi.role_state == WIFI_ROLE_STOPPING)
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      return -EBUSY;
    }

  previous_state = g_wifi.role_state;
  g_wifi.role_state = WIFI_ROLE_STOPPING;
  g_wifi.tx_gate = false;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  wifi_set_carrier(false);
  while (nxsem_trywait(&g_wifi.role_sem) == OK)
    {
    }

  if (role == WIFI_ROLE_STA)
    {
      ret = wifi_command(BK7258_WIFI_CMD_DISCONNECT, NULL, 0,
                         NULL, 0, NULL);
    }
  else
    {
      ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_AP_STOP, NULL, 0,
                         &status, sizeof(status), &result_length);
      if (ret >= 0 && result_length != sizeof(status))
        {
          ret = -EPROTO;
        }
      else if (ret >= 0 && status != 0)
        {
          ret = status;
        }
      if (ret >= 0)
        {
          ret = wifi_ap_status_query(&ap_status);
          if (ret >= 0 && !ap_status.started)
            {
              wifi_role_deactivate(WIFI_ROLE_SOFTAP);
              return OK;
            }
        }
    }

  if (ret >= 0)
    {
      ret = nxsem_tickwait_uninterruptible(&g_wifi.role_sem,
                                            WIFI_ROLE_STOP_TIMEOUT);
    }
  if (ret == -ETIMEDOUT && role == WIFI_ROLE_STA)
    {
      bool connected;

      if (wifi_sta_status_connected(&connected) == OK && !connected)
        {
          wifi_role_deactivate(WIFI_ROLE_STA);
          return OK;
        }
    }
  if (ret < 0)
    {
      bool carrier;

      nxmutex_lock(&g_wifi.packet_lock);
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
      if (g_wifi.active_role == role &&
          g_wifi.role_state == WIFI_ROLE_STOPPING && ret != -ETIMEDOUT)
        {
          g_wifi.role_state = previous_state;
          g_wifi.tx_gate = previous_state == WIFI_ROLE_ACTIVE &&
                           g_wifi.admin_up &&
                           (role == WIFI_ROLE_STA || g_wifi.ap_started);
        }
      carrier = g_wifi.tx_gate;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      wifi_set_carrier(carrier);
    }

  return ret;
}

static int wifi_ifdown(struct netdev_lowerhalf_s *lower)
{
  irqstate_t flags;
  int ret;

  (void)lower;
  ret = wifi_stop_active_role();
  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_wifi.admin_up = false;
  g_wifi.tx_gate = false;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  wifi_set_carrier(false);
  return ret;
}

static int wifi_transmit(struct netdev_lowerhalf_s *lower, netpkt_t *packet)
{
  struct wifi_tx_slot *tx = NULL;
  struct bk7258_wifi_cpdu *cpdu;
  uint8_t vif;
  uint32_t role_epoch;
  unsigned int length;
  unsigned int i;
  int ret;

  nxmutex_lock(&g_wifi.packet_lock);
  length = netpkt_getdatalen(lower, packet);
  if (!g_wifi.carrier || !g_wifi.tx_gate ||
      g_wifi.role_state != WIFI_ROLE_ACTIVE ||
      g_wifi.active_role == WIFI_ROLE_NONE ||
      !bk7258_mailbox_link_ready())
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      return -ENETDOWN;
    }
  if (length == 0 || length > BK7258_WIFI_MAX_FRAME)
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      return -EMSGSIZE;
    }

  for (i = 0; i < CONFIG_BK7258_WIFI_TX_SLOTS; i++)
    {
      if (!g_wifi.tx[i].active && !g_wifi.tx[i].transport_pending)
        {
          tx = &g_wifi.tx[i];
          break;
        }
    }
  if (tx == NULL)
    {
      printf("bk7258_wifi: AP TX slots exhausted\n");
      nxmutex_unlock(&g_wifi.packet_lock);
      return -EBUSY;
    }

  memset(&tx->pbuf, 0, sizeof(tx->pbuf));
  cpdu = (struct bk7258_wifi_cpdu *)tx->headroom;
  memset(cpdu, 0, sizeof(*cpdu));
  vif = wifi_role_vif(g_wifi.active_role);
  role_epoch = g_wifi.role_epoch;
  tx->pbuf.payload = (uint32_t)(uintptr_t)tx->frame;
  tx->pbuf.tot_len = length;
  tx->pbuf.len = length;
  tx->pbuf.type_internal = WIFI_PBUF_TYPE_RAM;
  tx->pbuf.ref = 1;
  cpdu->length = sizeof(*cpdu) + length;
  cpdu->type_dst = 2u;
  cpdu->flags = bk7258_wifi_cpdu_set_vif(cpdu->flags, vif);
  ret = netpkt_copyout(lower, tx->frame, packet, length, 0);
  if (ret < 0)
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      return ret;
    }

  if (g_wifi.active_role == WIFI_ROLE_SOFTAP && g_wifi_tx_trace < 16)
    {
      uint16_t type = length >= 14 ?
                      ((uint16_t)tx->frame[12] << 8) | tx->frame[13] : 0;

      printf("bk7258_wifi: AP TX vif=%u len=%u eth=0x%04x\n",
             vif, length, type);
      g_wifi_tx_trace++;
    }

  tx->packet = packet;
  tx->active = true;
  tx->transport_pending = true;
  tx->transport_rejected = false;
  tx->vif = vif;
  tx->role_epoch = role_epoch;
  ret = wifi_send_node(BK7258_WIFI_DATA_TX_CHANNEL, 2,
                       (uint32_t)(uintptr_t)cpdu,
                       (uint32_t)(uintptr_t)cpdu, 1,
                       wifi_tx_transport_complete, tx);
  if (ret < 0)
    {
      tx->packet = NULL;
      tx->active = false;
      tx->transport_pending = false;
    }
  nxmutex_unlock(&g_wifi.packet_lock);
  return ret;
}

static netpkt_t *wifi_receive(struct netdev_lowerhalf_s *lower)
{
  struct wifi_rx_packet *rx;
  netpkt_t *packet;
  irqstate_t flags;

  packet = netpkt_alloc(lower, NETPKT_RX);
  if (packet == NULL)
    {
      return NULL;
    }

  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  while (g_wifi.packet_count != 0 &&
         g_wifi.packets[g_wifi.packet_head].role_epoch != g_wifi.role_epoch)
    {
      g_wifi.packet_head = (g_wifi.packet_head + 1) % WIFI_PACKET_QUEUE;
      g_wifi.packet_count--;
    }
  if (g_wifi.packet_count == 0 ||
      g_wifi.active_role == WIFI_ROLE_NONE ||
      g_wifi.role_state != WIFI_ROLE_ACTIVE)
    {
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
      netpkt_free(lower, packet, NETPKT_RX);
      return NULL;
    }
  rx = &g_wifi.packets[g_wifi.packet_head];
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);

  if (netpkt_copyin(lower, packet, rx->frame, rx->length, 0) < 0)
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      netpkt_free(lower, packet, NETPKT_RX);
      return NULL;
    }

  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  g_wifi.packet_head = (g_wifi.packet_head + 1) % WIFI_PACKET_QUEUE;
  g_wifi.packet_count--;
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  return packet;
}

#ifdef CONFIG_NETDEV_IOCTL
static int wifi_ioctl(struct netdev_lowerhalf_s *lower, int command,
                      unsigned long arg)
{
  (void)lower;
  (void)command;
  (void)arg;
  return -ENOTTY;
}
#endif

#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
static int wifi_connect(struct netdev_lowerhalf_s *lower)
{
  struct bk7258_wifi_ap_status_response ap_status;
  uint8_t payload[33 + 64];
  struct bk7258_wifi_ap_start_request request;
  struct wifi_role_config config;
  enum wifi_role role;
  int32_t status = 0;
  uint16_t result_length = 0;
  size_t ssid_length;
  size_t password_length;
  irqstate_t flags;
  int ret;

  (void)lower;
  nxmutex_lock(&g_wifi.packet_lock);
  flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
  if (!g_wifi.admin_up)
    {
      ret = -ENETDOWN;
      goto connect_unlock;
    }
  if (g_wifi.active_role != WIFI_ROLE_NONE ||
      g_wifi.role_state != WIFI_ROLE_IDLE || wifi_tx_busy())
    {
      ret = -EBUSY;
      goto connect_unlock;
    }

  role = g_wifi.configured_role;
  config = role == WIFI_ROLE_STA ? g_wifi.sta_config : g_wifi.ap_config;
  if (role == WIFI_ROLE_STA)
    {
      ssid_length = strnlen(config.ssid, sizeof(config.ssid));
      password_length = strnlen(config.password, sizeof(config.password));
    }
  else
    {
      ssid_length = strnlen(config.ssid, sizeof(config.ssid));
      password_length = strnlen(config.password, sizeof(config.password));
      if (ssid_length == 0 || config.channel < 1 || config.channel > 14 ||
          (config.wpa_version == IW_AUTH_WPA_VERSION_DISABLED &&
           (config.cipher != IW_AUTH_CIPHER_NONE ||
            password_length != 0)) ||
          (config.wpa_version == IW_AUTH_WPA_VERSION_WPA2 &&
           (config.cipher != IW_AUTH_CIPHER_CCMP ||
            password_length < 8 || password_length > 63)))
        {
          ret = -EINVAL;
          goto connect_unlock;
        }
    }

  g_wifi.active_role = role;
  g_wifi.role_state = WIFI_ROLE_STARTING;
  g_wifi.ap_started = false;
  g_wifi.ap_client_count = 0;
  g_wifi.tx_gate = false;
  g_wifi.role_epoch++;
  memcpy(g_wifi.lower.netdev.d_mac.ether.ether_addr_octet,
         role == WIFI_ROLE_STA ? g_wifi.sta_mac : g_wifi.ap_mac,
         sizeof(g_wifi.sta_mac));
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);

  if (role == WIFI_ROLE_STA)
    {
      memset(payload, 0, sizeof(payload));
      memcpy(payload, config.ssid, ssid_length);
      memcpy(payload + 33, config.password, password_length);
      ret = wifi_command(BK7258_WIFI_CMD_CONNECT, payload, sizeof(payload),
                         NULL, 0, NULL);
    }
  else
    {
      memset(&request, 0, sizeof(request));
      request.version = BK7258_WIFI_AP_ABI_VERSION;
      request.channel = config.channel;
      request.security = config.wpa_version ==
                         IW_AUTH_WPA_VERSION_DISABLED ?
                         BK7258_WIFI_AP_SECURITY_OPEN :
                         BK7258_WIFI_AP_SECURITY_WPA2;
      request.max_clients = BK7258_WIFI_AP_MAX_CLIENTS;
      request.ssid_length = ssid_length;
      request.password_length = password_length;
      memcpy(request.ssid, config.ssid, ssid_length);
      memcpy(request.password, config.password, password_length);
      ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_AP_START, &request,
                         sizeof(request), &status, sizeof(status),
                         &result_length);
      if (ret >= 0 && result_length != sizeof(status))
        {
          ret = -EPROTO;
        }
      else if (ret >= 0 && status != 0)
        {
          ret = status;
        }
      if (ret >= 0)
        {
          ret = wifi_ap_status_query(&ap_status);
          if (ret >= 0 && !ap_status.started)
            {
              ret = -EIO;
            }
          else if (ret >= 0)
            {
              memcpy(g_wifi.ap_mac, ap_status.mac, sizeof(g_wifi.ap_mac));
              memcpy(g_wifi.lower.netdev.d_mac.ether.ether_addr_octet,
                     ap_status.mac, sizeof(ap_status.mac));
              wifi_ap_start_event(true);
            }
        }
    }

  if (ret < 0)
    {
      wifi_role_deactivate(role);
    }
  return ret;

connect_unlock:
  rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
  nxmutex_unlock(&g_wifi.packet_lock);
  return ret;
}

static int wifi_disconnect(struct netdev_lowerhalf_s *lower)
{
  (void)lower;
  return wifi_stop_active_role();
}

static int wifi_essid(struct netdev_lowerhalf_s *lower,
                       struct iwreq *request, bool set)
{
  struct wifi_role_config *config;
  size_t length;
  int ret = OK;

  (void)lower;
  if (request->u.essid.pointer == NULL)
    {
      return -EINVAL;
    }
  nxmutex_lock(&g_wifi.packet_lock);
  config = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
           &g_wifi.ap_config : &g_wifi.sta_config;
  if (set)
    {
      length = request->u.essid.length;
      if (length >= sizeof(config->ssid))
        {
          ret = -E2BIG;
          goto essid_unlock;
        }
      memcpy(config->ssid, request->u.essid.pointer, length);
      config->ssid[length] = '\0';
    }
  else
    {
      length = strlen(config->ssid) + 1;
      if (request->u.essid.length < length)
        {
          request->u.essid.length = length;
          ret = -E2BIG;
          goto essid_unlock;
        }
      memcpy(request->u.essid.pointer, config->ssid, length);
      request->u.essid.length = length;
      request->u.essid.flags = IW_ESSID_ON;
    }

essid_unlock:
  nxmutex_unlock(&g_wifi.packet_lock);
  return ret;
}

static int wifi_passwd(struct netdev_lowerhalf_s *lower,
                       struct iwreq *request, bool set)
{
  struct iw_encode_ext *ext;
  struct wifi_role_config *config;
  int ret = OK;

  (void)lower;
  nxmutex_lock(&g_wifi.packet_lock);
  config = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
           &g_wifi.ap_config : &g_wifi.sta_config;
  if (!set)
    {
      request->u.encoding.length = 0;
      goto passwd_unlock;
    }
  if ((request->u.encoding.flags & IW_ENCODE_DISABLED) != 0)
    {
      config->password[0] = '\0';
      config->cipher = IW_AUTH_CIPHER_NONE;
      config->wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
      goto passwd_unlock;
    }
  ext = request->u.encoding.pointer;
  if (ext == NULL || ext->key_len >= sizeof(config->password) ||
      (ext->alg != IW_ENCODE_ALG_NONE && ext->alg != IW_ENCODE_ALG_CCMP) ||
      (ext->alg == IW_ENCODE_ALG_NONE && ext->key_len != 0))
    {
      ret = -EINVAL;
      goto passwd_unlock;
    }
  memcpy(config->password, ext->key, ext->key_len);
  config->password[ext->key_len] = '\0';
  config->cipher = ext->alg == IW_ENCODE_ALG_NONE ?
                   IW_AUTH_CIPHER_NONE : IW_AUTH_CIPHER_CCMP;
  if (ext->alg == IW_ENCODE_ALG_CCMP)
    {
      config->wpa_version = IW_AUTH_WPA_VERSION_WPA2;
    }

passwd_unlock:
  nxmutex_unlock(&g_wifi.packet_lock);
  return ret;
}

static int wifi_country(struct netdev_lowerhalf_s *lower,
                        struct iwreq *request, bool set)
{
  struct bk7258_wifi_country country;
  struct bk7258_wifi_country_response response;
  uint16_t result_length = 0;
  int ret;

  (void)lower;
  if (request == NULL || request->u.data.pointer == NULL)
    {
      return -EINVAL;
    }

  if (set)
    {
      if (request->u.data.length != 2)
        {
          return -EINVAL;
        }
      memset(&country, 0, sizeof(country));
      memcpy(country.cc, request->u.data.pointer, 2);
      country.cc[2] = '\0';
      country.start_channel = 1;
      country.channel_count = country.cc[0] == 'J' && country.cc[1] == 'P' ?
                              14 : 13;
      country.policy = 1;
      ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_COUNTRY_SET, &country,
                         sizeof(country), &response.status,
                         sizeof(response.status), &result_length);
      if (ret < 0)
        {
          return ret;
        }
      if (result_length != sizeof(response.status))
        {
          return -EPROTO;
        }
      return response.status;
    }

  if (request->u.data.length < 2)
    {
      request->u.data.length = 2;
      return -E2BIG;
    }
  memset(&response, 0, sizeof(response));
  ret = wifi_command(BK7258_WIFI_CMD_OPENVELA_COUNTRY_GET, NULL, 0,
                     &response, sizeof(response), &result_length);
  if (ret < 0)
    {
      return ret;
    }
  if (result_length != sizeof(response))
    {
      return -EPROTO;
    }
  if (response.status != 0)
    {
      return response.status;
    }
  memcpy(request->u.data.pointer, response.country.cc, 2);
  request->u.data.length = 2;
  return OK;
}

static int wifi_mode(struct netdev_lowerhalf_s *lower,
                     struct iwreq *request, bool set)
{
  enum wifi_role role;
  irqstate_t flags;

  (void)lower;
  if (set)
    {
      if (request->u.mode != IW_MODE_INFRA &&
          request->u.mode != IW_MODE_MASTER)
        {
          return -EINVAL;
        }
      role = request->u.mode == IW_MODE_MASTER ? WIFI_ROLE_SOFTAP :
                                                WIFI_ROLE_STA;
      nxmutex_lock(&g_wifi.packet_lock);
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
      if (g_wifi.active_role != WIFI_ROLE_NONE)
        {
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          nxmutex_unlock(&g_wifi.packet_lock);
          return -EBUSY;
        }
      g_wifi.configured_role = role;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
    }
  else
    {
      nxmutex_lock(&g_wifi.packet_lock);
      request->u.mode = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
                        IW_MODE_MASTER : IW_MODE_INFRA;
      nxmutex_unlock(&g_wifi.packet_lock);
    }
  return OK;
}

static int wifi_auth(struct netdev_lowerhalf_s *lower,
                     struct iwreq *request, bool set)
{
  struct wifi_role_config *config;
  int32_t value;
  int ret = OK;

  (void)lower;
  nxmutex_lock(&g_wifi.packet_lock);
  config = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
           &g_wifi.ap_config : &g_wifi.sta_config;
  if (set)
    {
      value = request->u.param.value;
      switch (request->u.param.flags & IW_AUTH_INDEX)
        {
          case IW_AUTH_WPA_VERSION:
            if (value != IW_AUTH_WPA_VERSION_DISABLED &&
                value != IW_AUTH_WPA_VERSION_WPA2)
              {
                ret = -EOPNOTSUPP;
                goto auth_unlock;
              }
            config->wpa_version = value;
            if (value == IW_AUTH_WPA_VERSION_DISABLED)
              {
                config->cipher = IW_AUTH_CIPHER_NONE;
                config->password[0] = '\0';
              }
            break;

          case IW_AUTH_CIPHER_PAIRWISE:
          case IW_AUTH_CIPHER_GROUP:
            if (value != IW_AUTH_CIPHER_NONE &&
                value != IW_AUTH_CIPHER_CCMP)
              {
                ret = -EOPNOTSUPP;
                goto auth_unlock;
              }
            config->cipher = value;
            break;

          default:
            ret = -EOPNOTSUPP;
            goto auth_unlock;
        }
    }
  else
    {
      switch (request->u.param.flags & IW_AUTH_INDEX)
        {
          case IW_AUTH_WPA_VERSION:
            request->u.param.value = config->wpa_version;
            break;

          case IW_AUTH_CIPHER_PAIRWISE:
          case IW_AUTH_CIPHER_GROUP:
            request->u.param.value = config->cipher;
            break;

          default:
            ret = -EOPNOTSUPP;
            goto auth_unlock;
        }
    }

auth_unlock:
  nxmutex_unlock(&g_wifi.packet_lock);
  return ret;
}

static int wifi_freq(struct netdev_lowerhalf_s *lower,
                     struct iwreq *request, bool set)
{
  struct wifi_role_config *config;
  uint8_t channel;
  irqstate_t flags;

  (void)lower;
  if (set)
    {
      if (request->u.freq.e != 0 || request->u.freq.m < 1 ||
          request->u.freq.m > 14)
        {
          return -EINVAL;
        }
      channel = request->u.freq.m;
      nxmutex_lock(&g_wifi.packet_lock);
      config = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
               &g_wifi.ap_config : &g_wifi.sta_config;
      flags = rspin_lock_irqsave(&g_bk7258_driver_lock);
      if (g_wifi.active_role == WIFI_ROLE_SOFTAP &&
          g_wifi.role_state != WIFI_ROLE_IDLE &&
          channel != g_wifi.ap_config.channel)
        {
          rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
          nxmutex_unlock(&g_wifi.packet_lock);
          return -EBUSY;
        }
      config->channel = channel;
      rspin_unlock_irqrestore(&g_bk7258_driver_lock, flags);
      nxmutex_unlock(&g_wifi.packet_lock);
    }
  else
    {
      nxmutex_lock(&g_wifi.packet_lock);
      config = g_wifi.configured_role == WIFI_ROLE_SOFTAP ?
               &g_wifi.ap_config : &g_wifi.sta_config;
      request->u.freq.m = config->channel;
      request->u.freq.e = 0;
      request->u.freq.i = 0;
      request->u.freq.flags = IW_FREQ_FIXED;
      nxmutex_unlock(&g_wifi.packet_lock);
    }
  return OK;
}

static int wifi_bssid(struct netdev_lowerhalf_s *lower,
                      struct iwreq *request, bool set)
{
  struct bk7258_wifi_ap_status_response response;
  int ret;

  (void)lower;
  if (set)
    {
      return -EOPNOTSUPP;
    }
  ret = wifi_ap_status_query(&response);
  if (ret < 0)
    {
      return ret;
    }
  memset(&request->u.ap_addr, 0, sizeof(request->u.ap_addr));
  request->u.ap_addr.sa_family = ARPHRD_ETHER;
  memcpy(request->u.ap_addr.sa_data, response.mac, sizeof(response.mac));
  return OK;
}

static int wifi_scan(struct netdev_lowerhalf_s *lower,
                      struct iwreq *request, bool set)
{
  uint8_t ssid[33];
  int32_t status;
  uint16_t result_length;
  int ret;

  (void)lower;
  if (request == NULL)
    {
      return -EINVAL;
    }
  nxmutex_lock(&g_wifi.packet_lock);
  if (g_wifi.active_role == WIFI_ROLE_SOFTAP)
    {
      nxmutex_unlock(&g_wifi.packet_lock);
      return -EBUSY;
    }
  nxmutex_unlock(&g_wifi.packet_lock);
  if (!set)
    {
      nxmutex_lock(&g_wifi.scan_lock);
      if (g_wifi.scan_running)
        {
          nxmutex_unlock(&g_wifi.scan_lock);
          return -EAGAIN;
        }
      if (!g_wifi.scan_cached)
        {
          ret = wifi_scan_cache_fetch();
          if (ret < 0)
            {
              nxmutex_unlock(&g_wifi.scan_lock);
              return ret;
            }
        }
      if (request->u.data.pointer == NULL ||
          request->u.data.length < g_wifi.scan_cache_length)
        {
          request->u.data.length = g_wifi.scan_cache_length;
          nxmutex_unlock(&g_wifi.scan_lock);
          return -E2BIG;
        }
      memcpy(request->u.data.pointer, g_wifi.scan_cache,
             g_wifi.scan_cache_length);
      request->u.data.length = g_wifi.scan_cache_length;
      nxmutex_unlock(&g_wifi.scan_lock);
      return OK;
    }

  nxmutex_lock(&g_wifi.scan_lock);
  wifi_scan_cache_clear();
  memset(ssid, 0, sizeof(ssid));
  if ((request->u.data.flags & IW_SCAN_THIS_ESSID) != 0 &&
      request->u.data.pointer != NULL &&
      request->u.data.length >= sizeof(struct iw_scan_req))
    {
      const struct iw_scan_req *scan = request->u.data.pointer;
      size_t length = scan->essid_len;

      if (length > sizeof(ssid) - 1)
        {
          length = sizeof(ssid) - 1;
        }
      memcpy(ssid, scan->essid, length);
    }
  result_length = 0;
  status = 0;
  ret = wifi_command(BK7258_WIFI_CMD_SCAN_WIFI, ssid, sizeof(ssid),
                     &status, sizeof(status), &result_length);
  if (ret >= 0 && result_length != sizeof(status))
    {
      printf("wifi scan start invalid response length: %u\n", result_length);
      ret = -EPROTO;
    }
  else if (ret >= 0 && status != 0)
    {
      printf("wifi scan start rejected: %ld\n", (long)status);
      ret = status;
    }
  else if (ret < 0)
    {
      printf("wifi scan start transport failed: %d\n", ret);
    }
  if (ret == OK)
    {
      g_wifi.scan_running = true;
    }
  nxmutex_unlock(&g_wifi.scan_lock);
  return ret;
}

static int wifi_range(struct netdev_lowerhalf_s *lower,
                      struct iwreq *request)
{
  struct iw_range *range = request->u.data.pointer;
  uint8_t i;

  (void)lower;
  if (range == NULL || request->u.data.length < sizeof(*range))
    {
      return -EINVAL;
    }
  memset(range, 0, sizeof(*range));
  range->num_frequency = 14;
  for (i = 0; i < range->num_frequency; i++)
    {
      range->freq[i].m = i + 1;
    }
  request->u.data.length = sizeof(*range);
  return OK;
}

static const struct wireless_ops_s g_wifi_ops =
{
  .connect = wifi_connect,
  .disconnect = wifi_disconnect,
  .essid = wifi_essid,
  .bssid = wifi_bssid,
  .passwd = wifi_passwd,
  .mode = wifi_mode,
  .auth = wifi_auth,
  .freq = wifi_freq,
  .bitrate = NULL,
  .txpower = NULL,
  .country = wifi_country,
  .sensitivity = NULL,
  .scan = wifi_scan,
  .range = wifi_range
};
#endif

static const struct netdev_ops_s g_netdev_ops =
{
  .ifup = wifi_ifup,
  .ifdown = wifi_ifdown,
  .transmit = wifi_transmit,
  .receive = wifi_receive,
#ifdef CONFIG_NETDEV_IOCTL
  .ioctl = wifi_ioctl,
#endif
};

int bk7258_wifi_initialize(void)
{
  uint8_t mac[6];
  uint8_t status[64];
  unsigned int i;
  int ret;

  if (g_wifi.initialized)
    {
      return OK;
    }

  memset(&g_wifi, 0, sizeof(g_wifi));
  nxmutex_init(&g_wifi.command_lock);
  nxmutex_init(&g_wifi.packet_lock);
  nxmutex_init(&g_wifi.scan_lock);
  nxsem_init(&g_wifi.command_sem, 0, 0);
  nxsem_init(&g_wifi.role_sem, 0, 0);
  nxsem_init(&g_wifi.work_sem, 0, 0);
  nxsem_init(&g_wifi.ready_sem, 0, 0);
  g_worker_result = -EINPROGRESS;
  g_wifi.reset_generation = bk7258_mailbox_peer_reset_generation();
  for (i = 0; i < BK7258_WIFI_CMD_SLOTS; i++)
    {
      *(uint32_t *)g_wifi.command[i].bytes =
        BK7258_WIFI_CMD_PATTERN_FREE;
    }

  ret = bk7258_mailbox_register_rx(BK7258_WIFI_CMD_RX_CHANNEL,
                                   wifi_mailbox_rx, NULL);
  if (ret < 0)
    {
      return ret;
    }
  ret = bk7258_mailbox_register_rx(BK7258_WIFI_DATA_RX_CHANNEL,
                                   wifi_mailbox_rx, NULL);
  if (ret < 0)
    {
      return ret;
    }
  bk7258_mailbox_set_link_callback(wifi_link_changed, NULL);
  ret = kthread_create("bk-wifi", 110, 3072, wifi_worker, NULL);
  if (ret < 0)
    {
      return ret;
    }
  if (nxsem_tickwait_uninterruptible(&g_wifi.ready_sem,
                                     MSEC2TICK(200)) < 0)
    {
      return -ETIMEDOUT;
    }
  if (g_worker_result < 0)
    {
      return g_worker_result;
    }

  g_wifi.lower.ops = &g_netdev_ops;
#ifdef CONFIG_NETDEV_WIRELESS_HANDLER
  g_wifi.lower.iw_ops = &g_wifi_ops;
#endif
  g_wifi.lower.quota[NETPKT_TX] = CONFIG_BK7258_WIFI_TX_SLOTS;
  g_wifi.lower.quota[NETPKT_RX] = WIFI_PACKET_QUEUE;
  g_wifi.lower.rxtype = NETDEV_RX_WORK;
  g_wifi.lower.netdev.d_pktsize = BK7258_WIFI_MAX_FRAME;
  g_wifi.configured_role = WIFI_ROLE_STA;
  g_wifi.active_role = WIFI_ROLE_NONE;
  g_wifi.role_state = WIFI_ROLE_IDLE;
  g_wifi.role_epoch = 1;
  g_wifi.sta_config.wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
  g_wifi.sta_config.cipher = IW_AUTH_CIPHER_NONE;
  g_wifi.ap_config.wpa_version = IW_AUTH_WPA_VERSION_DISABLED;
  g_wifi.ap_config.cipher = IW_AUTH_CIPHER_NONE;
  g_wifi.ap_config.channel = 1;

  memset(mac, 0, sizeof(mac));
  ret = wifi_command(BK7258_WIFI_CMD_GET_MAC_ADDR, NULL, 0,
                     mac, sizeof(mac), NULL);
  if (ret < 0)
    {
      return ret;
    }
  memcpy(g_wifi.lower.netdev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));
  memcpy(g_wifi.sta_mac, mac, sizeof(mac));
  {
    struct bk7258_wifi_ap_status_response ap_status;
    ret = wifi_ap_status_query(&ap_status);
    if (ret < 0)
      {
        return ret;
      }
    memcpy(g_wifi.ap_mac, ap_status.mac, sizeof(g_wifi.ap_mac));
  }
  memset(status, 0, sizeof(status));
  ret = wifi_command(BK7258_WIFI_CMD_GET_WLAN_STATUS, NULL, 0,
                     status, sizeof(status), NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = netdev_lower_register(&g_wifi.lower, NET_LL_IEEE80211);
  if (ret < 0)
    {
      return ret;
    }
  g_wifi.initialized = true;
  g_wifi.carrier_notified = !g_wifi.carrier;
  wifi_notify_carrier();
  return OK;
}

#endif /* CONFIG_BK7258_WIFI */
