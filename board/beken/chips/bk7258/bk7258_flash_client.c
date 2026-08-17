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

#define FLASH_IPC_CHUNK        0x200u

#define FLASH_OP_TIMEOUT_MS    3000u
#define FLASH_CONNECT_TIMEOUT_MS 1000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The descriptor the flash service exchanges (flash_ipc.h flash_cmd_t).
 * Layout matters: it crosses cores.
 */

struct flash_ipc_cmd_s
{
  uint32_t part_addr;       /* part_id:8 then addr:24 */
  FAR uint8_t *buff;
  uint16_t len;
  int16_t ret_status;
  uint32_t crc;
};

static_assert(sizeof(struct flash_ipc_cmd_s) == 16,
              "flash_cmd_t ABI size");

struct flash_client_s
{
  mutex_t lock;             /* One request at a time */
  sem_t done;               /* Posted by the RX callback */
  bool connected;
  uint8_t tag;
  volatile uint8_t rsp_cmd;      /* hdr.cmd of the frame received */
  volatile uint8_t rsp_user_cmd; /* user_cmd of the frame received */
  volatile uint8_t rsp_status;   /* api_impl_status | route_status */
  volatile uint16_t rsp_len;
  FAR uint8_t *volatile rsp_buff;
  volatile int tx_result;
  volatile uint8_t tx_ack_status;
  volatile uint8_t tx_ack_state;
};

/* The descriptor the CP reads.  In SWAP rather than in this structure: see
 * BK7258_FLASH_IPC_ADDRESS.
 */

#define flash_request \
  ((FAR volatile struct flash_ipc_cmd_s *)BK7258_FLASH_IPC_ADDRESS)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct flash_client_s g_flash =
{
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* CRC-32 over the bytes the server reports, same polynomial the vendor's
 * calc_crc32() uses (reflected 0xedb88320, init and final xor 0xffffffff).
 * Used to check a read rather than trust it; a mismatch means the descriptor
 * and the data disagree, which is the failure worth catching.
 */

static uint32_t flash_crc32(FAR const uint8_t *data, size_t len)
{
  uint32_t crc = 0xffffffffu;
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

  return ~crc;
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
  UNUSED(arg);

  /* Tell the peer its frame is handled and done (ACK_STATE_COMPLETE, the
   * value mailbox_channel.c puts in ack_data1).  Answering 0 leaves the
   * server's socket in "receive in process", and every later request comes
   * back rejected: measured as ack status 0x52, i.e. route RX_BUSY (5) and
   * api RX_BUSY (2) at once, with no reply frame at all.
   */

  if (ack_flags != NULL)
    {
      *ack_flags = 2u;
    }

  g_flash.rsp_cmd = bk7258_mb_header_cmd(message);
  g_flash.rsp_user_cmd = message->crc8;
  g_flash.rsp_status = (uint8_t)(message->payload_address >> 24);
  g_flash.rsp_len = message->payload_length;
  g_flash.rsp_buff = (FAR uint8_t *)(uintptr_t)message->reserved;

  nxsem_post(&g_flash.done);
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
 * Name: flash_ipc_xfer
 *
 * Description:
 *   Send one frame and wait for the single frame that answers it.  Returns
 *   OK, or -ETIMEDOUT if the CP said nothing in time, or -EREMOTEIO if it
 *   answered with a router or API error.
 *
 ****************************************************************************/

static int flash_ipc_xfer(uint8_t cmd, uint8_t user_cmd,
                          FAR void *data, uint16_t len,
                          unsigned int timeout_ms)
{
  struct bk7258_mb_wire_message message;
  int ret;

  memset(&message, 0, sizeof(message));
  message.header = cmd;
  message.payload_address = flash_ipc_param1(g_flash.tag++);
  message.payload_length = len;
  message.flags = (data != NULL && len > 0) ?
                  flash_crc8((FAR const uint8_t *)data, len) : 0;
  message.crc8 = user_cmd;                 /* param2 bits 24-31 */
  message.reserved = (uint32_t)(uintptr_t)data;

  /* Drain a stale post, if any: a previous timeout may have left one. */

  while (nxsem_trywait(&g_flash.done) == OK)
    {
    }

  g_flash.rsp_cmd = IPC_USER_CMD_NONE;

  g_flash.tx_result = -EINPROGRESS;
  g_flash.tx_ack_status = 0xffu;
  g_flash.tx_ack_state = 0xffu;

  ret = bk7258_mailbox_send_wire(BK7258_MB_CHAN_IPC_TX, &message,
                                 flash_ipc_tx_done, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_tickwait_uninterruptible(&g_flash.done,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      printf("flash: no answer to cmd=%u user=%u len=%u "
             "(tx result=%d ack state=0x%02x status=0x%02x)\n",
             cmd, user_cmd, len, g_flash.tx_result,
             g_flash.tx_ack_state, g_flash.tx_ack_status);
      return -ETIMEDOUT;
    }

  if (g_flash.rsp_buff != NULL &&
      !flash_peer_ptr_ok(g_flash.rsp_buff, sizeof(struct flash_ipc_cmd_s)))
    {
      printf("flash: reply descriptor at %p is outside CP RAM "
             "0x%08x-0x%08x and SWAP 0x%08x+0x%x; not dereferencing it\n",
             g_flash.rsp_buff,
             (unsigned int)BK7258_CP_RAM_START,
             (unsigned int)BK7258_CP_RAM_END,
             (unsigned int)BK7258_SWAP_BASE,
             (unsigned int)BK7258_SWAP_SIZE);
      return -EFAULT;
    }

  if (g_flash.rsp_status != 0)
    {
      return -EREMOTEIO;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int bk7258_flash_client_init(void)
{
  int ret;

  if (g_flash.connected)
    {
      return OK;
    }

  nxsem_init(&g_flash.done, 0, 0);

  ret = bk7258_mailbox_register_rx(BK7258_MB_CHAN_IPC_RX, flash_ipc_rx,
                                   NULL);
  if (ret < 0)
    {
      printf("flash: cannot take the IPC channel: %d\n", ret);
      return ret;
    }

  ret = flash_ipc_xfer(IPC_CMD_CONNECT, IPC_USER_CMD_NONE, NULL, 0,
                       FLASH_CONNECT_TIMEOUT_MS);
  if (ret < 0)
    {
      printf("flash: no answer from the CP flash server: %d\n", ret);
      return ret;
    }

  if ((g_flash.rsp_cmd & IPC_CMD_MASK) != IPC_CMD_CONNECT ||
      (g_flash.rsp_cmd & IPC_RSP_FLAG) == 0)
    {
      printf("flash: unexpected answer to connect: cmd=0x%02x\n",
             g_flash.rsp_cmd);
      return -EPROTO;
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
      size_t chunk = len > FLASH_IPC_CHUNK ? FLASH_IPC_CHUNK : len;

      memset((FAR void *)flash_request, 0, sizeof(struct flash_ipc_cmd_s));
      (*flash_request).part_addr = address & 0x00ffffffu;
      (*flash_request).len = (uint16_t)chunk;

      ret = flash_ipc_xfer(IPC_CMD_SEND, FLASH_CMD_READ, (FAR void *)flash_request,
                           sizeof(struct flash_ipc_cmd_s), FLASH_OP_TIMEOUT_MS);
      if (ret < 0)
        {
          break;
        }

      if (g_flash.rsp_user_cmd != FLASH_CMD_READ ||
          g_flash.rsp_len != sizeof(struct flash_ipc_cmd_s) ||
          g_flash.rsp_buff == NULL)
        {
          ret = -EPROTO;
          break;
        }

      {
        FAR const struct flash_ipc_cmd_s *rsp =
          (FAR const struct flash_ipc_cmd_s *)g_flash.rsp_buff;

        if (rsp->ret_status != 0 || rsp->len != chunk || rsp->buff == NULL)
          {
            ret = -EIO;
            break;
          }

        if (!flash_peer_ptr_ok(rsp->buff, chunk))
          {
            printf("flash: read payload at %p (%u bytes) is outside the "
                   "regions this core maps; refusing the copy\n",
                   rsp->buff, (unsigned int)chunk);
            ret = -EFAULT;
            break;
          }

        memcpy(out, rsp->buff, chunk);

        if (flash_crc32(out, chunk) != rsp->crc)
          {
            ret = -EBADMSG;
            break;
          }
      }

      /* The server waits for this before releasing its buffer, and for one
       * receive per send of its own -- see flash_client.c's read path.
       */

      (void)flash_ipc_xfer(IPC_CMD_SEND, FLASH_CMD_READ_DONE,
                           (FAR void *)flash_request, sizeof(struct flash_ipc_cmd_s),
                           FLASH_OP_TIMEOUT_MS);

      out += chunk;
      address += chunk;
      len -= chunk;
    }

  nxmutex_unlock(&g_flash.lock);
  return ret;
}

int bk7258_flash_erase_sector(uint32_t address)
{
  int ret;

  if (!g_flash.connected)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_flash.lock);

  memset((FAR void *)flash_request, 0, sizeof(struct flash_ipc_cmd_s));
  (*flash_request).part_addr = address & 0x00ffffffu;

  ret = flash_ipc_xfer(IPC_CMD_SEND, FLASH_CMD_ERASE_SECTOR,
                       (FAR void *)flash_request, sizeof(struct flash_ipc_cmd_s),
                       FLASH_OP_TIMEOUT_MS);
  if (ret == OK)
    {
      FAR const struct flash_ipc_cmd_s *rsp =
        (FAR const struct flash_ipc_cmd_s *)g_flash.rsp_buff;

      if (g_flash.rsp_user_cmd != FLASH_CMD_ERASE_SECTOR)
        {
          ret = -EPROTO;
        }
      else if (rsp == NULL || rsp->ret_status != 0)
        {
          ret = -EIO;
        }
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
      size_t chunk = len > FLASH_IPC_CHUNK ? FLASH_IPC_CHUNK : len;

      memset((FAR void *)flash_request, 0, sizeof(struct flash_ipc_cmd_s));
      (*flash_request).part_addr = address & 0x00ffffffu;
      (*flash_request).buff = (FAR uint8_t *)in;
      (*flash_request).len = (uint16_t)chunk;
      (*flash_request).crc = flash_crc32(in, chunk);

      ret = flash_ipc_xfer(IPC_CMD_SEND, FLASH_CMD_WRITE, (FAR void *)flash_request,
                           sizeof(struct flash_ipc_cmd_s), FLASH_OP_TIMEOUT_MS);
      if (ret < 0)
        {
          break;
        }

      {
        FAR const struct flash_ipc_cmd_s *rsp =
          (FAR const struct flash_ipc_cmd_s *)g_flash.rsp_buff;

        if (g_flash.rsp_user_cmd != FLASH_CMD_WRITE)
          {
            ret = -EPROTO;
            break;
          }

        if (rsp == NULL || rsp->ret_status != 0)
          {
            ret = -EIO;
            break;
          }
      }

      in += chunk;
      address += chunk;
      len -= chunk;
    }

  nxmutex_unlock(&g_flash.lock);
  return ret;
}
