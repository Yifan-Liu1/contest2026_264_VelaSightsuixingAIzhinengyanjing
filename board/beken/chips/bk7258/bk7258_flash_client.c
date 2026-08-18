/****************************************************************************
 * board/beken/chips/bk7258/bk7258_flash_client.c
 *
 * Flash access for the AP, by asking the CP to do it.
 *
 * Why this exists
 * ---------------
 * The AP had no writable persistent storage at all, which is why every API
 * key and Wi-Fi credential had to be either compiled in or re-typed after
 * each reset.  Flash is there -- the partition table even reserves
 * `easyflash_ap`, 8KB at 0x007FC000, read/write/execute-disabled, for this
 * core -- but the AP must not program it directly: both cores execute XIP
 * from the same part, so an erase started by one stalls the other's
 * instruction fetch.  That is exactly why the vendor made flash a service:
 * cp/middleware/driver/flash/flash_server.c serves requests that
 * ap/middleware/driver/flash/flash_client.c sends over the mailbox.
 *
 * And the CP firmware already on this board runs that server:
 * projects/app_ab/build/.../bk7258/config/sdkconfig.h has CONFIG_FLASH_MB=1
 * and flash_server.c.obj is in its build tree.  So no CP change is needed --
 * only a client, which is what this file is.
 *
 * Protocol (mb_ipc sockets, ap/middleware/driver/mailbox/mb_ipc.c)
 * ---------------------------------------------------------------
 * Frames ride logical channel 0x11 out (CP0_MB_CHNL_IPC) and 0x41 back.  The
 * 16-byte frame the vendor calls mb_chnl_cmd_t is byte-for-byte the wire
 * message this port already sends on the other channels; only the meaning of
 * the three parameter words differs:
 *
 *   header          hdr: cmd, state, ctrl, seq, channel
 *   payload_address param1: dst_port:6 dst_cpu:2 | src_port:6 src_cpu:2 |
 *                           tag:8 | api_impl_status:4 route_status:4
 *   payload_length  param2 low half: cmd_data_len
 *   flags           param2 bits 16-23: cmd_data_crc8
 *   crc8            param2 bits 24-31: user_cmd (0xFF means none)
 *   reserved        param3: cmd_data_buff, a pointer both cores can read
 *
 * hdr.cmd is MB_IPC_CONNECT_CMD (0), MB_IPC_DISCONNECT_CMD (1) or
 * MB_IPC_SEND_CMD (2); a response is the same value with 0x80 set.  Ports:
 * the flash server is CPU0 port 1, this client is CPU1 port 16
 * (include/driver/mb_ipc_port_cfg.h).
 *
 * Every request to the server is answered by one frame, and the server
 * expects a receive for each of its sends -- flash_client.c's read path even
 * ends with a bare recv it calls "just a handshake".  This client keeps the
 * same shape.
 *
 * Read returns the data in the *server's* buffer (it hands back a pointer in
 * cmd.buff plus a CRC-32 over the bytes), so the copy happens here.  Write
 * and erase carry the caller's buffer the same way.
 *
 * Bounded waits everywhere, deliberately: if the AP stops answering the CP's
 * heartbeat for CONFIG_INT_WDT_PERIOD_MS (8s) the CP tears the console
 * bridge down and asserts, resetting the whole part with no output from this
 * side (arm_lowputc() is itself a mailbox tunnel).  Blocking forever on a
 * silent server would look exactly like that bug.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "hardware/bk7258_mbox.h"
#include "bk7258_flash_client.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* mb_ipc command ids and the response flag (include/driver/mb_ipc.h). */

#define IPC_CMD_CONNECT        0u
#define IPC_CMD_DISCONNECT     1u
#define IPC_CMD_SEND           2u
#define IPC_RSP_FLAG           0x80u
#define IPC_CMD_MASK           0x7fu

#define IPC_USER_CMD_NONE      0xffu

/* Ports (include/driver/mb_ipc_port_cfg.h): server ids are cpu<<6 | port. */

#define IPC_FLASH_SERVER_CPU   0u
#define IPC_FLASH_SERVER_PORT  1u
#define IPC_FLASH_CLIENT_CPU   1u
#define IPC_FLASH_CLIENT_PORT  16u

/* Flash service commands (ap/middleware/driver/flash/flash_ipc.h). */

#define FLASH_CMD_ERASE_SECTOR 0u
#define FLASH_CMD_READ         3u
#define FLASH_CMD_READ_DONE    4u
#define FLASH_CMD_WRITE        5u

/* The server chunks transfers; flash_ipc.h fixes both at 512 bytes. */

#define FLASH_IPC_CHUNK        BK7258_FLASH_CHUNK_SIZE

#define FLASH_OP_TIMEOUT_MS    3000u
#define FLASH_CONNECT_TIMEOUT_MS 1000u

/* The flash operation notification (flash_notify.c on both sides): hdr.cmd is
 * the edge, param1 carries the request/acknowledge state.
 */

#define FLASH_OP_START         0u
#define FLASH_OP_END           1u
#define FLASH_OP_STATE_REQ     1u
#define FLASH_OP_STATE_ACK     2u

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The descriptor the flash service exchanges (flash_ipc.h flash_cmd_t).
 * Layout matters: it crosses cores.
 */

struct flash_ipc_cmd_s
{
  /* The server declares this word as two bitfields:
   *
   *   u32 part_id : 8;
   *   u32 addr    : 24;
   *
   * (cp/middleware/driver/flash/flash_ipc.h).  On this little-endian target the
   * first-declared field takes the least significant bits, so part_id is
   * bits 0-7 and the address is bits 8-31 -- the address is *shifted up by
   * eight*, which is what flash_part_addr() below exists to get right.
   *
   * Writing the address unshifted, as this port did, handed the server
   * address >> 8: a request for 0x007fc000 asked for 0x00007fc0 with
   * part_id 0xc0.  See flash_part_addr().
   */

  uint32_t part_addr;
  FAR uint8_t *buff;
  uint16_t len;
  int16_t ret_status;
  uint32_t crc;
};

static_assert(sizeof(struct flash_ipc_cmd_s) == 16,
              "flash_cmd_t ABI size");

struct flash_client_s
{
  mutex_t lock;             /* One flash operation at a time */
  sem_t wire_done;          /* Low-level mailbox ACK received */
  sem_t tx_rsp;             /* Matching mb_ipc cmd|RSP received */
  sem_t rx_ready;           /* New mb_ipc command from the server */
  bool sems_initialized;
  bool connected;
  uint8_t tag;
  volatile bool tx_waiting;
  volatile bool rx_pending;
  volatile uint8_t expected_cmd;
  volatile uint8_t expected_tag;
  volatile uint8_t rsp_status;
  volatile int tx_result;
  volatile uint8_t tx_ack_status;
  volatile uint8_t tx_ack_state;
  struct bk7258_mb_wire_message rx_message;
};

/* The descriptor the CP reads.  In SWAP rather than in this structure: see
 * BK7258_FLASH_IPC_ADDRESS.
 */

#define flash_request \
  ((FAR volatile struct flash_ipc_cmd_s *)BK7258_FLASH_IPC_ADDRESS)

/* And the bytes a write hands over, for the same reason. */

#define flash_payload ((FAR volatile uint8_t *)BK7258_FLASH_DATA_ADDRESS)

static_assert(FLASH_IPC_CHUNK <= BK7258_FLASH_DATA_SIZE,
              "flash payload staging smaller than one protocol chunk");

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct flash_client_s g_flash =
{
  .lock = NXMUTEX_INITIALIZER,
};

static bk7258_flash_op_notify_t g_flash_op_notify;
static FAR void *g_flash_op_notify_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: flash_part_addr
 *
 * Description:
 *   Pack a flash offset into the server's part_id:8 / addr:24 word.
 *
 *   The address occupies bits 8-31, not 0-23.  Getting this wrong does not
 *   produce an error: the server happily reads whatever address it was given,
 *   so the symptom is data from the wrong place -- or, when the resulting
 *   address is one the server will not serve, no reply at all, which on this
 *   transport escalates into a chip reset (an unanswered transaction is
 *   aborted, the link is quarantined and re-probed, and while it is probing
 *   nothing else can be sent -- including the heartbeat the CP's 8-second
 *   watchdog is waiting for).  Measured 2026-08-18 as a boot loop with
 *   "IPC[1]heartbeat timeout" and "Assert at: mb_ipc_task:297".
 *
 ****************************************************************************/

static inline uint32_t flash_part_addr(uint32_t address, uint8_t part_id)
{
  return ((address & 0x00ffffffu) << 8) | part_id;
}

/* CRC-32 over the payload bytes, in exactly the form the flash service uses.
 *
 * The polynomial is the usual reflected 0xedb88320, but the server calls it as
 * calc_crc32(0, buf, len) (flash_server.c) -- no 0xffffffff seed and no final
 * complement.  Seeding and complementing, which is what the same polynomial
 * normally comes with, produces a different number for the same bytes: every
 * read would then fail its check here as -EBADMSG, and every write would be
 * rejected by the server, which re-computes this over the buffer it was handed
 * and compares.  The store's own CRC (bk7258_kvdb.c) is a separate matter --
 * nothing but this port reads it, so it keeps the seeded form.
 */

static uint32_t flash_payload_crc32(FAR const uint8_t *data, size_t len)
{
  uint32_t crc = 0;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
        }
    }

  return crc;
}

/* CRC-8 with polynomial 0x31, table-free.  mb_ipc puts this over the payload
 * in cmd_data_crc8 and the receiver checks it (mb_ipc.c:1985); sending zero
 * instead made the server drop every request, which looked like a timeout
 * rather than a rejection.  Same table as cal_crc8_0x31(), computed rather
 * than copied.
 */

static uint8_t flash_crc8(FAR const uint8_t *data, size_t len)
{
  uint8_t crc = 0x00;
  size_t i;
  int bit;

  for (i = 0; i < len; i++)
    {
      crc ^= data[i];

      for (bit = 0; bit < 8; bit++)
        {
          crc = (uint8_t)((crc & 0x80u) != 0 ?
                          ((crc << 1) ^ 0x31u) : (crc << 1));
        }
    }

  return crc;
}

static uint32_t flash_ipc_param1(uint8_t tag)
{
  uint32_t dst = IPC_FLASH_SERVER_PORT | (IPC_FLASH_SERVER_CPU << 6);
  uint32_t src = IPC_FLASH_CLIENT_PORT | (IPC_FLASH_CLIENT_CPU << 6);

  return dst | (src << 8) | ((uint32_t)tag << 16);
}

/****************************************************************************
 * Name: flash_ipc_rx
 *
 * Description:
 *   One frame arrived from the CP on the IPC channel.  Interrupt context:
 *   record what came in and wake the requester, nothing else.
 *
 ****************************************************************************/

static int flash_ipc_rx(FAR const struct bk7258_mb_wire_message *message,
                        FAR uint8_t *ack_flags, FAR void *arg)
{
  uint8_t command = bk7258_mb_header_cmd(message);

  UNUSED(arg);

  /* Low-level mailbox completion is carried in ACK word 3.  It only says the
   * 16-byte frame was consumed; mb_ipc delivery is a separate cmd|0x80 frame.
   */

  if (ack_flags != NULL)
    {
      *ack_flags = BK7258_MB_ACK_STATE_COMPLETE;
    }

  if ((command & BK7258_MB_IPC_RSP_FLAG) != 0)
    {
      if (g_flash.tx_waiting &&
          bk7258_mb_ipc_is_response(message, g_flash.expected_cmd,
                                    g_flash.expected_tag))
        {
          g_flash.rsp_status =
            (uint8_t)(message->payload_address >> 24);
          nxsem_post(&g_flash.tx_rsp);
        }

      return OK;
    }

  /* A server SEND is not the response above.  Preserve it until the waiting
   * flash operation has copied the descriptor and emitted its own mb_ipc
   * response.  Returning -EAGAIN leaves the physical descriptor queued if a
   * second command somehow arrives before the first is consumed.
   */

  if (g_flash.rx_pending)
    {
      return -EAGAIN;
    }

  memcpy(&g_flash.rx_message, message, sizeof(g_flash.rx_message));
  g_flash.rx_pending = true;
  nxsem_post(&g_flash.rx_ready);
  return OK;
}

/****************************************************************************
 * Name: flash_ipc_tx_done
 *
 * Description:
 *   Channel-level acknowledgement of our frame.  The vendor's router takes
 *   its tx_status from here (mb_ipc.c:962), so this is where a request that
 *   the peer's *router* rejected shows up -- as opposed to one its handler
 *   answered.  Recorded so a silent server can be told apart from a refused
 *   frame.
 *
 ****************************************************************************/

static void flash_ipc_tx_done(FAR const struct bk7258_mb_wire_message *ack,
                              int result, FAR void *arg)
{
  UNUSED(arg);

  g_flash.tx_result = result;
  g_flash.tx_ack_status = ack != NULL ?
                          (uint8_t)(ack->payload_address >> 24) : 0xffu;
  g_flash.tx_ack_state = ack != NULL ? bk7258_mb_header_state(ack) : 0xffu;
  nxsem_post(&g_flash.wire_done);
}

/****************************************************************************
 * Name: flash_peer_ptr_ok
 *
 * Description:
 *   Whether a pointer the server handed us is one this core may dereference.
 *
 *   Everything in the reply is a CP-side address: the descriptor arrives in
 *   the frame's reserved field, and the read payload arrives as a pointer
 *   inside that descriptor.  Both are written by the other core, so neither
 *   can be trusted; a bad one faults, and an AP fault is nearly invisible
 *   because arm_lowputc() goes out through the mailbox that is itself in
 *   trouble.  The observed symptom was a boot that reached the flash read and
 *   then simply never got to NSH, with nothing printed.
 *
 *   Only CP RAM and the SWAP window are legitimate: those are the two regions
 *   both cores map (bk7258_mbox.h).  A pointer to CP *flash* or to a
 *   peripheral alias would read as something, which is worse than failing.
 *
 ****************************************************************************/

static bool flash_peer_ptr_ok(FAR const void *ptr, size_t len)
{
  uintptr_t addr = (uintptr_t)ptr;

  if (addr == 0 || len == 0)
    {
      return false;
    }

  if (addr >= BK7258_CP_RAM_START && addr + len <= BK7258_CP_RAM_END)
    {
      return true;
    }

  if (addr >= BK7258_SWAP_BASE &&
      addr + len <= BK7258_SWAP_BASE + BK7258_SWAP_SIZE)
    {
      return true;
    }

  return false;
}

/****************************************************************************
 * Name: flash_ipc_send_wire_wait
 *
 * Description:
 *   Send one mailbox frame and wait only for its low-level ACK.  This is not
 *   an mb_ipc response: the latter is a separate command with bit 7 set.
 *
 ****************************************************************************/

static int flash_ipc_send_wire_wait(
    FAR const struct bk7258_mb_wire_message *message,
    unsigned int timeout_ms)
{
  int ret;

  while (nxsem_trywait(&g_flash.wire_done) == OK)
    {
    }

  g_flash.tx_result = -EINPROGRESS;
  g_flash.tx_ack_status = 0xffu;
  g_flash.tx_ack_state = 0xffu;

  ret = bk7258_mailbox_send_wire(BK7258_MB_CHAN_IPC_TX, message,
                                 flash_ipc_tx_done, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_tickwait_uninterruptible(&g_flash.wire_done,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("flash: no mailbox ACK for ipc cmd=0x%02x\n",
             bk7258_mb_header_cmd(message));
      return -ETIMEDOUT;
    }

  if (g_flash.tx_result < 0)
    {
      return g_flash.tx_result;
    }

  if (g_flash.tx_ack_status != 0)
    {
      return -EREMOTEIO;
    }

  return OK;
}

/****************************************************************************
 * Name: flash_ipc_send_command
 *
 * Description:
 *   Send an mb_ipc command, first waiting for the mailbox ACK and then for
 *   the matching cmd|0x80 response.  The response only confirms delivery;
 *   the flash operation result arrives later as a new SEND command.
 *
 ****************************************************************************/

static int flash_ipc_send_command(uint8_t command, uint8_t user_cmd,
                                  FAR const void *data, uint16_t len,
                                  unsigned int timeout_ms)
{
  struct bk7258_mb_wire_message message;
  uint8_t tag = g_flash.tag++;
  int ret;

  memset(&message, 0, sizeof(message));
  message.header = command;
  message.payload_address = flash_ipc_param1(tag);
  message.payload_length = len;
  message.flags = data != NULL && len > 0 ?
                  flash_crc8((FAR const uint8_t *)data, len) : 0;
  message.crc8 = user_cmd;
  message.reserved = (uint32_t)(uintptr_t)data;

  while (nxsem_trywait(&g_flash.tx_rsp) == OK)
    {
    }

  g_flash.expected_cmd = command;
  g_flash.expected_tag = tag;
  g_flash.rsp_status = 0xffu;
  g_flash.tx_waiting = true;

  ret = flash_ipc_send_wire_wait(&message, timeout_ms);
  if (ret < 0)
    {
      g_flash.tx_waiting = false;
      return ret;
    }

  ret = nxsem_tickwait_uninterruptible(&g_flash.tx_rsp,
                                       MSEC2TICK(timeout_ms));
  g_flash.tx_waiting = false;
  if (ret < 0)
    {
      printf("flash: no ipc response to cmd=%u user=%u tag=%u len=%u\n",
             command, user_cmd, tag, len);
      return -ETIMEDOUT;
    }

  return g_flash.rsp_status == 0 ? OK : -EREMOTEIO;
}

/****************************************************************************
 * Name: flash_ipc_receive
 *
 * Description:
 *   Receive the server's separate SEND command, copy its descriptor while the
 *   CP still owns it, then send the mandatory cmd|0x80 response that releases
 *   the CP's mb_ipc_send().
 *
 ****************************************************************************/

static int flash_ipc_receive(uint8_t expected_user_cmd,
                             FAR struct flash_ipc_cmd_s *descriptor,
                             unsigned int timeout_ms)
{
  struct bk7258_mb_wire_message request;
  struct bk7258_mb_wire_message response;
  FAR const void *peer;
  int result = OK;
  int ret;

  ret = nxsem_tickwait_uninterruptible(&g_flash.rx_ready,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0 || !g_flash.rx_pending)
    {
      printf("flash: no server command for user=%u\n", expected_user_cmd);
      return -ETIMEDOUT;
    }

  memcpy(&request, &g_flash.rx_message, sizeof(request));
  peer = (FAR const void *)(uintptr_t)request.reserved;

  if (bk7258_mb_header_cmd(&request) != IPC_CMD_SEND ||
      request.crc8 != expected_user_cmd ||
      request.payload_length != sizeof(*descriptor))
    {
      result = -EPROTO;
    }
  else if ((uint8_t)(request.payload_address >> 24) != 0)
    {
      result = -EREMOTEIO;
    }
  else if (!flash_peer_ptr_ok(peer, sizeof(*descriptor)))
    {
      printf("flash: server descriptor at %p is outside shared memory\n",
             peer);
      result = -EFAULT;
    }
  else if (flash_crc8((FAR const uint8_t *)peer, sizeof(*descriptor)) !=
           request.flags)
    {
      result = -EBADMSG;
    }
  else
    {
      memcpy(descriptor, peer, sizeof(*descriptor));
    }

  /* Clear ownership before replying: the response can let the CP send its
   * next command immediately.  The physical mailbox ACK was already emitted
   * when flash_ipc_rx returned.
   */

  g_flash.rx_pending = false;
  bk7258_mb_ipc_make_response(&request, &response);
  ret = flash_ipc_send_wire_wait(&response, timeout_ms);
  if (ret < 0)
    {
      return ret;
    }

  return result;
}

static int flash_ipc_transaction(uint8_t user_cmd,
                                 FAR const struct flash_ipc_cmd_s *request,
                                 FAR struct flash_ipc_cmd_s *response,
                                 unsigned int timeout_ms)
{
  int ret = flash_ipc_send_command(IPC_CMD_SEND, user_cmd, request,
                                   sizeof(*request), timeout_ms);

  if (ret < 0)
    {
      return ret;
    }

  return flash_ipc_receive(user_cmd, response, timeout_ms);
}

/****************************************************************************
 * Name: flash_notify_rx
 *
 * Description:
 *   The CP announces every flash access it is about to make, and its end, on
 *   MB_CHNL_FLASH -- for its own writes as much as for ours.  It then spins
 *   waiting for an acknowledgement whose ack_data1 reads IPC_FLASH_OP_ACK,
 *   giving up after 5ms (FLASH_WAIT_ACK_TIMEOUT in its flash_notify.c).  With
 *   this channel unregistered the transport answered COM_FAIL instead, so the
 *   CP burned that 5ms twice around every flash operation and nothing on this
 *   side ever learned that the part was busy.
 *
 *   Interrupt context.  Whatever a subscriber does here has to be ISR-safe and
 *   short: the CP is spinning on the answer.
 *
 ****************************************************************************/

static int flash_notify_rx(FAR const struct bk7258_mb_wire_message *message,
                           FAR uint8_t *ack_flags, FAR void *arg)
{
  uint8_t edge = bk7258_mb_header_cmd(message);

  UNUSED(arg);

  /* The vendor's handler answers unconditionally, before looking at anything;
   * an unanswered notification costs the CP 5ms whatever we think of it.
   */

  if (ack_flags != NULL)
    {
      *ack_flags = FLASH_OP_STATE_ACK;
    }

  if (message->payload_address != FLASH_OP_STATE_REQ)
    {
      return OK;
    }

  if (g_flash_op_notify != NULL &&
      (edge == FLASH_OP_START || edge == FLASH_OP_END))
    {
      g_flash_op_notify(edge == FLASH_OP_START, g_flash_op_notify_arg);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_flash_notify_init(void)
{
  return bk7258_mailbox_register_rx(BK7258_MB_CHAN_FLASH_RX,
                                    flash_notify_rx, NULL);
}

void bk7258_flash_op_notify_register(bk7258_flash_op_notify_t callback,
                                     FAR void *arg)
{
  g_flash_op_notify_arg = arg;
  g_flash_op_notify = callback;
}

int bk7258_flash_client_init(void)
{
  int ret;

  if (g_flash.connected)
    {
      return OK;
    }

  if (!g_flash.sems_initialized)
    {
      nxsem_init(&g_flash.wire_done, 0, 0);
      nxsem_init(&g_flash.tx_rsp, 0, 0);
      nxsem_init(&g_flash.rx_ready, 0, 0);
      g_flash.sems_initialized = true;
    }

  ret = bk7258_mailbox_register_rx(BK7258_MB_CHAN_IPC_RX, flash_ipc_rx,
                                   NULL);
  if (ret < 0)
    {
      printf("flash: cannot take the IPC channel: %d\n", ret);
      return ret;
    }

  ret = flash_ipc_send_command(IPC_CMD_CONNECT, IPC_USER_CMD_NONE, NULL, 0,
                               FLASH_CONNECT_TIMEOUT_MS);
  if (ret < 0)
    {
      printf("flash: no answer from the CP flash server: %d\n", ret);
      return ret;
    }

  g_flash.connected = true;
  printf("flash: connected to the CP flash server (cpu%u port%u)\n",
         IPC_FLASH_SERVER_CPU, IPC_FLASH_SERVER_PORT);
  return OK;
}

bool bk7258_flash_client_ready(void)
{
  return g_flash.connected;
}

int bk7258_flash_read(uint32_t address, FAR void *buffer, size_t len)
{
  FAR uint8_t *out = buffer;
  int ret = OK;

  if (!g_flash.connected || buffer == NULL || len == 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_flash.lock);

  while (len > 0)
    {
      struct flash_ipc_cmd_s response;
      struct flash_ipc_cmd_s done_response;
      size_t chunk = len > FLASH_IPC_CHUNK ? FLASH_IPC_CHUNK : len;

      memset((FAR void *)flash_request, 0, sizeof(*flash_request));
      (*flash_request).part_addr = flash_part_addr(address, 0);
      (*flash_request).len = (uint16_t)chunk;

      ret = flash_ipc_transaction(FLASH_CMD_READ,
                                  (FAR const struct flash_ipc_cmd_s *)
                                    flash_request,
                                  &response, FLASH_OP_TIMEOUT_MS);
      if (ret < 0)
        {
          break;
        }

      if (response.ret_status != 0 || response.len != chunk ||
          response.buff == NULL)
        {
          ret = -EIO;
          break;
        }

      if (!flash_peer_ptr_ok(response.buff, chunk))
        {
          printf("flash: read payload at %p (%u bytes) is outside shared "
                 "memory\n", response.buff, (unsigned int)chunk);
          ret = -EFAULT;
          break;
        }

      memcpy(out, response.buff, chunk);
      if (flash_payload_crc32(out, chunk) != response.crc)
        {
          ret = -EBADMSG;
          break;
        }

      /* The completed descriptor is the READ_DONE payload.  The server sends
       * one final READ_DONE command in return, so receive and answer that too
       * before its shared read buffer may be reused.
       */

      memcpy((FAR void *)flash_request, &response, sizeof(response));
      ret = flash_ipc_send_command(IPC_CMD_SEND, FLASH_CMD_READ_DONE,
                                   (FAR const void *)flash_request,
                                   sizeof(*flash_request),
                                   FLASH_OP_TIMEOUT_MS);
      if (ret == OK)
        {
          ret = flash_ipc_receive(FLASH_CMD_READ_DONE, &done_response,
                                  FLASH_OP_TIMEOUT_MS);
        }

      if (ret < 0 || done_response.ret_status != 0)
        {
          if (ret == OK)
            {
              ret = -EIO;
            }

          break;
        }

      out += chunk;
      address += chunk;
      len -= chunk;
    }

  nxmutex_unlock(&g_flash.lock);
  return ret;
}

int bk7258_flash_erase_sector(uint32_t address)
{
  struct flash_ipc_cmd_s response;
  int ret;

  if (!g_flash.connected)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_flash.lock);

  memset((FAR void *)flash_request, 0, sizeof(*flash_request));
  (*flash_request).part_addr = flash_part_addr(address, 0);

  ret = flash_ipc_transaction(FLASH_CMD_ERASE_SECTOR,
                              (FAR const struct flash_ipc_cmd_s *)flash_request,
                              &response, FLASH_OP_TIMEOUT_MS);
  if (ret == OK && response.ret_status != 0)
    {
      ret = -EIO;
    }

  nxmutex_unlock(&g_flash.lock);
  return ret;
}

int bk7258_flash_write(uint32_t address, FAR const void *buffer, size_t len)
{
  FAR const uint8_t *in = buffer;
  int ret = OK;

  if (!g_flash.connected || buffer == NULL || len == 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_flash.lock);

  while (len > 0)
    {
      struct flash_ipc_cmd_s response;
      size_t chunk = len > FLASH_IPC_CHUNK ? FLASH_IPC_CHUNK : len;

      /* The CP dereferences both pointers, so descriptor and payload must be
       * in the shared, non-cacheable SWAP window rather than the AP heap.
       */

      memcpy((FAR void *)flash_payload, in, chunk);

      memset((FAR void *)flash_request, 0, sizeof(*flash_request));
      (*flash_request).part_addr = flash_part_addr(address, 0);
      (*flash_request).buff = (FAR uint8_t *)flash_payload;
      (*flash_request).len = (uint16_t)chunk;
      (*flash_request).crc = flash_payload_crc32(
        (FAR const uint8_t *)flash_payload, chunk);

      ret = flash_ipc_transaction(FLASH_CMD_WRITE,
                                  (FAR const struct flash_ipc_cmd_s *)
                                    flash_request,
                                  &response, FLASH_OP_TIMEOUT_MS);
      if (ret < 0)
        {
          break;
        }

      if (response.ret_status != 0)
        {
          ret = -EIO;
          break;
        }

      in += chunk;
      address += chunk;
      len -= chunk;
    }

  nxmutex_unlock(&g_flash.lock);
  return ret;
}
