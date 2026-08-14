/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 *
 * BK7258 SD-NAND SDIO host.  The first port is deliberately polling based:
 * it keeps the hardware state machine observable while command and PIO
 * behavior are being validated on the target board.  No SDIO DMA is used.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/signal.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"
#include "bk7258_sdio.h"
#include "hardware/bk7258_gpio.h"
#include "hardware/bk7258_sdio.h"

int bk7258_sdio_clock_request(bool enable);

struct bk7258_sdio_s
{
  struct sdio_dev_s dev;
  uint32_t blocklen;
  uint32_t nblocks;
  FAR uint8_t *buffer;
  size_t remaining;
  sdio_eventset_t wait_events;
  sdio_eventset_t wake_events;
  uint32_t wait_timeout;
  clock_t wait_start;
  uint32_t clock_code;
  bool data_active;
  bool initialized;
  mutex_t io_lock;
};

static struct bk7258_sdio_s g_sdio;

static inline uint32_t sdio_read(unsigned int reg)
{
  return getreg32(reg);
}

static inline void sdio_write(unsigned int reg, uint32_t value)
{
  putreg32(value, reg);
}

static void sdio_pinmux(void)
{
  unsigned int pin;

  for (pin = 14; pin <= 19; pin++)
    {
      /* Function zero is SDIO for the complete GPIO14..19 map. */
      bk7258_gpio_set_function(pin, 0);
      modifyreg32(BK7258_GPIO_CFG(pin), BK7258_GPIO_CAPACITY_MASK,
                  BK7258_GPIO_PULL_UP | BK7258_GPIO_PULL_ENABLE |
                  BK7258_GPIO_CAPACITY_3);
    }
}

static void sdio_set_clock(struct bk7258_sdio_s *priv, uint32_t code)
{
  uint32_t value;

  /* SYS_CPU_CLK_DIV_MODE2: SDIO divider [16:14], source [17]. */
  value = getreg32(0x44010024u);
  value &= ~(0x0fu << 14);
  value |= (code & 0x0fu) << 14;
  putreg32(value, 0x44010024u);
  priv->clock_code = code;
}

static void sdio_reset_hw(struct bk7258_sdio_s *priv)
{
  uint32_t value;

  sdio_write(BK7258_SDIO_INT_MASK, 0);
  sdio_write(BK7258_SDIO_INT_STATUS, 0xffffffffu);

  value = sdio_read(BK7258_SDIO_FIFO);
  value &= ~(0xffffu | BK7258_SDIO_FIFO_RX_RESET |
             BK7258_SDIO_FIFO_TX_RESET | BK7258_SDIO_STATE_RESET);
   value |= 0x0101u | BK7258_SDIO_CLOCK_RECOVERY;
  sdio_write(BK7258_SDIO_FIFO, value);

  sdio_write(BK7258_SDIO_CMD_TIMER, BK7258_SDIO_TIMEOUT);
  sdio_write(BK7258_SDIO_DATA_TIMER, BK7258_SDIO_TIMEOUT);
  sdio_write(BK7258_SDIO_DATA_CTRL, BK7258_SDIO_DATA_BYTE_SELECT);
  sdio_set_clock(priv, BK7258_SDIO_ID_CLOCK);
}

static int sdio_wait_command(struct bk7258_sdio_s *priv, uint32_t cmd)
{
  clock_t deadline = clock_systime_ticks() + MSEC2TICK(1000);
  uint32_t status;

  for (;;)
    {
      status = sdio_read(BK7258_SDIO_INT_STATUS);
      if ((status & (BK7258_SDIO_CMD_NO_RESPONSE |
                     BK7258_SDIO_CMD_RESPONSE_END |
                     BK7258_SDIO_CMD_TIMEOUT)) != 0)
        {
          break;
        }

      if ((int32_t)(clock_systime_ticks() - deadline) >= 0)
        {
          printf("SDIO CMD%u timeout status=0x%08lx\n",
                 (unsigned)(cmd & MMCSD_CMDIDX_MASK),
                 (unsigned long)status);
          return -ETIMEDOUT;
        }

      /* At transfer speed the RX FIFO can fill before a 100 us sleep
       * expires.  Keep the PIO loop tight while data is active; otherwise a
       * 512-byte sector consistently overflows after the first 256 bytes. */
      if (!priv->data_active)
        {
          nxsig_usleep(100);
        }
    }

  sdio_write(BK7258_SDIO_INT_STATUS,
             status & (BK7258_SDIO_CMD_NO_RESPONSE |
                       BK7258_SDIO_CMD_RESPONSE_END |
                       BK7258_SDIO_CMD_TIMEOUT |
                       BK7258_SDIO_CMD_CRC_OK |
                       BK7258_SDIO_CMD_CRC_FAIL));

  if ((status & BK7258_SDIO_CMD_TIMEOUT) != 0)
    {
      printf("SDIO CMD%u response timeout status=0x%08lx\n",
             (unsigned)(cmd & MMCSD_CMDIDX_MASK),
             (unsigned long)status);
      return -ETIMEDOUT;
    }

  /* The BK7258 host reports CRC failure for the open-drain/long-response
   * identification commands even when the vendor stack accepts the response.
   * Armino explicitly tolerates this for ACMD41, CMD2 and CMD9.  Keep the
   * same compatibility rule while preserving CRC errors for normal R1/R7.
   */
  if ((status & BK7258_SDIO_CMD_CRC_FAIL) != 0 &&
      (cmd & MMCSD_CMDIDX_MASK) != SD_ACMDIDX41 &&
      (cmd & MMCSD_CMDIDX_MASK) != MMCSD_CMDIDX2 &&
      (cmd & MMCSD_CMDIDX_MASK) != MMCSD_CMDIDX9)
    {
      printf("SDIO CMD%u CRC error status=0x%08lx\n",
             (unsigned)(cmd & MMCSD_CMDIDX_MASK),
             (unsigned long)status);
      return -EIO;
    }

  return OK;
}

static int sdio_sendcmd(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        uint32_t arg)
{
  uint32_t response;
  uint32_t value;

  response = (cmd & MMCSD_RESPONSE_MASK) >> MMCSD_RESPONSE_SHIFT;
  value = ((cmd & MMCSD_CMDIDX_MASK) << BK7258_SDIO_CMD_INDEX_SHIFT);
  if (response != MMCSD_NO_RESPONSE)
    {
      value |= BK7258_SDIO_CMD_RESPONSE;
    }
  if (response == MMCSD_R2_RESPONSE)
    {
      value |= BK7258_SDIO_CMD_LONG;
    }
  if (response != MMCSD_NO_RESPONSE &&
      (cmd & MMCSD_CMDIDX_MASK) != MMCSD_CMDIDX2 &&
      (cmd & MMCSD_CMDIDX_MASK) != MMCSD_CMDIDX9 &&
      (cmd & MMCSD_CMDIDX_MASK) != SD_ACMDIDX41)
    {
      value |= BK7258_SDIO_CMD_CRC_CHECK;
    }

  sdio_write(BK7258_SDIO_CMD_ARGUMENT, arg);
  sdio_write(BK7258_SDIO_CMD_CTRL, value | BK7258_SDIO_CMD_START);
  return OK;
}

static int sdio_waitresponse(FAR struct sdio_dev_s *dev, uint32_t cmd)
{
  return sdio_wait_command((FAR struct bk7258_sdio_s *)dev, cmd);
}

static int sdio_recv_r1(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  *value = sdio_read(BK7258_SDIO_RESPONSE(0));
  printf("SDIO R1 CMD%u=0x%08lx\n", (unsigned)(cmd & MMCSD_CMDIDX_MASK),
         (unsigned long)*value);
  return OK;
}

static int sdio_recv_r2(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t value[4])
{
  /* The vendor driver assigns response register 0 to CSD_3 (bits
   * 127..96), register 1 to CSD_2, register 2 to CSD_1 and register 3 to
   * CSD_0. This is already the word order expected by NuttX. */
  value[0] = sdio_read(BK7258_SDIO_RESPONSE(0));
  value[1] = sdio_read(BK7258_SDIO_RESPONSE(1));
  value[2] = sdio_read(BK7258_SDIO_RESPONSE(2));
  value[3] = sdio_read(BK7258_SDIO_RESPONSE(3));
  printf("SDIO R2 CMD%u=%08lx %08lx %08lx %08lx\n",
         (unsigned)(cmd & MMCSD_CMDIDX_MASK), (unsigned long)value[0],
         (unsigned long)value[1], (unsigned long)value[2],
         (unsigned long)value[3]);
  return OK;
}

static int sdio_recv_r3(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  return sdio_recv_r1(dev, cmd, value);
}

static int sdio_recv_r4(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  return sdio_recv_r1(dev, cmd, value);
}

static int sdio_recv_r5(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  return sdio_recv_r1(dev, cmd, value);
}

static int sdio_recv_r6(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  return sdio_recv_r1(dev, cmd, value);
}

static int sdio_recv_r7(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  return sdio_recv_r1(dev, cmd, value);
}

static void sdio_reset(FAR struct sdio_dev_s *dev)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  sdio_reset_hw(priv);
  priv->data_active = false;
  priv->remaining = 0;
  priv->wake_events = 0;
}

static sdio_capset_t sdio_capabilities(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return SDIO_CAPS_1BIT_ONLY;
}

static sdio_statset_t sdio_status(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return SDIO_STATUS_PRESENT;
}

static void sdio_widebus(FAR struct sdio_dev_s *dev, bool enable)
{
  (void)dev;
  (void)enable;
}

static void sdio_clock(FAR struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  if (rate == CLOCK_IDMODE)
    {
      sdio_set_clock(priv, BK7258_SDIO_ID_CLOCK);
      modifyreg32(BK7258_SDIO_FIFO, 0, BK7258_SDIO_CLOCK_GATE);
    }
  else if (rate == CLOCK_SD_TRANSFER_1BIT)
    {
      sdio_set_clock(priv, BK7258_SDIO_TRANSFER_CLOCK);
      modifyreg32(BK7258_SDIO_FIFO, BK7258_SDIO_CLOCK_GATE, 0);
    }
}

static int sdio_attach(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return OK;
}

static void sdio_blocksetup(FAR struct sdio_dev_s *dev,
                            unsigned int blocklen, unsigned int nblocks)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  priv->blocklen = blocklen;
  priv->nblocks = nblocks;
}

static int sdio_recvsetup(FAR struct sdio_dev_s *dev, FAR uint8_t *buffer,
                          size_t nbytes)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  uint32_t value;

  if (buffer == NULL || nbytes == 0 || priv->blocklen == 0 ||
      nbytes > (priv->blocklen * priv->nblocks))
    {
      return -EINVAL;
    }

  /* Match the vendor read setup order: program timeout and clear stale W1C
   * status before asserting the RX FIFO and SD state resets. */
  sdio_write(BK7258_SDIO_DATA_TIMER, BK7258_SDIO_TIMEOUT);
  sdio_write(BK7258_SDIO_INT_STATUS, 0xffffffffu);

  /* Complete the vendor V2 reset pulse before configuring the receive
   * transaction. */
  value = sdio_read(BK7258_SDIO_FIFO);
  value &= ~(0xffffu | BK7258_SDIO_FIFO_RX_RESET |
             BK7258_SDIO_FIFO_TX_RESET | BK7258_SDIO_STATE_RESET);
  sdio_write(BK7258_SDIO_FIFO, value);

  value |= 0x0101u | BK7258_SDIO_FIFO_RX_RESET |
           BK7258_SDIO_STATE_RESET | BK7258_SDIO_CLOCK_RECOVERY;
  sdio_write(BK7258_SDIO_FIFO, value);

  value = ((priv->blocklen << BK7258_SDIO_DATA_BLOCK_SHIFT) &
           BK7258_SDIO_DATA_BLOCK_MASK) | BK7258_SDIO_DATA_BYTE_SELECT;
  if (priv->blocklen == 512)
    {
      value |= BK7258_SDIO_DATA_MULTIBLOCK;
    }
  /* Keep configuration and start as separate writes, matching the vendor
   * set_read_multi_block_data()/start_receive_data() sequence. */
  sdio_write(BK7258_SDIO_DATA_CTRL, value);
  modifyreg32(BK7258_SDIO_INT_MASK, 0, BK7258_SDIO_DATA_RECEIVE_END);
  sdio_write(BK7258_SDIO_DATA_CTRL, value | BK7258_SDIO_DATA_ENABLE);
  priv->buffer = buffer;
  priv->remaining = nbytes;
  priv->data_active = true;
  printf("SDIO data setup block=%lu count=%lu bytes=%lu\n",
         (unsigned long)priv->blocklen, (unsigned long)priv->nblocks,
         (unsigned long)nbytes);
  return OK;
}

static int sdio_sendsetup(FAR struct sdio_dev_s *dev,
                          FAR const uint8_t *buffer, size_t nbytes)
{
  (void)dev;
  (void)buffer;
  (void)nbytes;
  return -EOPNOTSUPP;
}

static int sdio_cancel(FAR struct sdio_dev_s *dev)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  sdio_write(BK7258_SDIO_DATA_CTRL, 0);
  sdio_write(BK7258_SDIO_INT_STATUS, 0xffffffffu);
  priv->data_active = false;
  priv->remaining = 0;
  priv->wake_events = SDIOWAIT_ERROR;
  return OK;
}

static void sdio_waitenable(FAR struct sdio_dev_s *dev,
                            sdio_eventset_t eventset, uint32_t timeout)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  priv->wait_events = eventset;
  priv->wake_events = 0;
  priv->wait_timeout = timeout == 0 ? 1000 : timeout;
  priv->wait_start = clock_systime_ticks();
}

static void sdio_drain_fifo(FAR struct bk7258_sdio_s *priv)
{
  while (priv->remaining != 0)
    {
      uint32_t word;
      unsigned int timeout = 400;
      size_t copy = priv->remaining < sizeof(word) ? priv->remaining :
                    sizeof(word);

      while ((sdio_read(BK7258_SDIO_FIFO) & BK7258_SDIO_RX_READY) == 0 &&
             timeout-- != 0)
        {
          up_udelay(1);
        }

      if (timeout == 0)
        {
          return;
        }

      word = sdio_read(BK7258_SDIO_RX_FIFO);
      memcpy(priv->buffer, &word, copy);
      priv->buffer += copy;
      priv->remaining -= copy;
    }
}

int bk7258_sdio_read_blocks(FAR uint8_t *buffer, uint32_t sector,
                            unsigned int nblocks)
{
  FAR struct sdio_dev_s *dev;
  FAR struct bk7258_sdio_s *priv;
  sdio_eventset_t event;
  unsigned int i;
  unsigned int nonff = 0;
  uint32_t hash = 2166136261u;
  uint32_t response;
  uint32_t value;
  bool stream_active = false;
  int data_ret;
  int ret;

  if (buffer == NULL || nblocks == 0 || nblocks > 1)
    {
      return -EINVAL;
    }

  dev = bk7258_sdio_initialize(0);
  if (dev == NULL)
    {
      return -ENODEV;
    }

  priv = (FAR struct bk7258_sdio_s *)dev;
  ret = nxmutex_lock(&priv->io_lock);
  if (ret < 0)
    {
      return ret;
    }

  SDIO_BLOCKSETUP(dev, 512, 1);
  SDIO_WAITENABLE(dev, SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT |
                  SDIOWAIT_ERROR, 2000);
  ret = SDIO_RECVSETUP(dev, buffer, 512);
  if (ret == OK)
    {
      /* OCR=0x80ff8000 has CCS clear, so CMD18 uses byte addressing. */
      ret = SDIO_SENDCMD(dev, MMCSD_CMD18, sector << 9);
    }
  if (ret == OK)
    {
      ret = SDIO_WAITRESPONSE(dev, MMCSD_CMD18);
    }
  if (ret == OK)
    {
      ret = SDIO_RECVR1(dev, MMCSD_CMD18, &response);
      stream_active = ret == OK;
    }
  if (ret == OK)
    {
      event = SDIO_EVENTWAIT(dev);
      if ((event & SDIOWAIT_TRANSFERDONE) == 0)
        {
          ret = (event & SDIOWAIT_TIMEOUT) != 0 ? -ETIMEDOUT : -EIO;
        }
    }

  data_ret = ret;

  if (stream_active)
    {
      /* Match the vendor V2 CMD12 path after the FIFO block is consumed. */
      value = sdio_read(BK7258_SDIO_FIFO);
      value &= ~(BK7258_SDIO_FIFO_RX_RESET |
                 BK7258_SDIO_FIFO_TX_RESET | BK7258_SDIO_STATE_RESET);
      sdio_write(BK7258_SDIO_FIFO, value);
      modifyreg32(BK7258_SDIO_FIFO, 0, BK7258_SDIO_CLOCK_GATE);

      ret = SDIO_SENDCMD(dev, MMCSD_CMD12, 0);
      if (ret == OK)
        {
          ret = SDIO_WAITRESPONSE(dev, MMCSD_CMD12);
        }
      if (ret == OK)
        {
          ret = SDIO_RECVR1(dev, MMCSD_CMD12, &response);
        }

      modifyreg32(BK7258_SDIO_FIFO, BK7258_SDIO_CLOCK_GATE, 0);
      value = sdio_read(BK7258_SDIO_FIFO);
      value &= ~(BK7258_SDIO_FIFO_RX_RESET |
                 BK7258_SDIO_FIFO_TX_RESET | BK7258_SDIO_STATE_RESET);
      sdio_write(BK7258_SDIO_FIFO, value);

      if (data_ret != OK)
        {
          ret = data_ret;
        }
    }

  if (ret == OK)
    {
      int signature = -1;

      for (i = 0; i < 512; i++)
        {
          hash = (hash ^ buffer[i]) * 16777619u;
          if (buffer[i] != 0xff)
            {
              nonff++;
            }

          if (i != 0 && buffer[i - 1] == 0x55 && buffer[i] == 0xaa)
            {
              signature = (int)i - 1;
            }
        }

      printf("SDIO sector=%lu nonff=%u hash=%08lx first=%02x%02x%02x%02x "
             "last=%02x%02x%02x%02x signature=%d\n",
             (unsigned long)sector, nonff, (unsigned long)hash,
              buffer[0], buffer[1], buffer[2], buffer[3],
              buffer[508], buffer[509], buffer[510], buffer[511], signature);

    }

  nxmutex_unlock(&priv->io_lock);
  return ret;
}

static sdio_eventset_t sdio_eventwait(FAR struct sdio_dev_s *dev)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  clock_t deadline = priv->wait_start + MSEC2TICK(priv->wait_timeout);

  for (;;)
    {
      uint32_t status = sdio_read(BK7258_SDIO_INT_STATUS);

      if ((status & BK7258_SDIO_DATA_CRC_FAIL) != 0)
        {
          printf("SDIO data CRC error status=0x%08lx remaining=%lu\n",
                 (unsigned long)status, (unsigned long)priv->remaining);
          sdio_write(BK7258_SDIO_INT_STATUS,
                     status & (BK7258_SDIO_DATA_RECEIVE_END |
                               BK7258_SDIO_DATA_CRC_FAIL |
                               BK7258_SDIO_DATA_CRC_OK));
          priv->data_active = false;
          priv->wake_events = SDIOWAIT_ERROR;
          break;
        }

      if ((status & (BK7258_SDIO_DATA_TIMEOUT |
                      BK7258_SDIO_FIFO_OVERFLOW)) != 0)
        {
          clock_t drain_deadline = clock_systime_ticks() + MSEC2TICK(10);

          while (priv->remaining != 0 &&
                 (int32_t)(clock_systime_ticks() - drain_deadline) < 0)
            {
              sdio_drain_fifo(priv);
            }

          printf("SDIO data error status=0x%08lx remaining=%lu\n",
                 (unsigned long)status, (unsigned long)priv->remaining);
          sdio_write(BK7258_SDIO_INT_STATUS,
                      status & (BK7258_SDIO_DATA_RECEIVE_END |
                                BK7258_SDIO_DATA_TIMEOUT |
                                BK7258_SDIO_RX_NEED_READ |
                                BK7258_SDIO_DATA_CRC_FAIL |
                                BK7258_SDIO_FIFO_OVERFLOW |
                                BK7258_SDIO_DATA_CRC_OK));
          priv->data_active = false;
           priv->wake_events = SDIOWAIT_ERROR;
          break;
        }

      if (priv->data_active &&
          (status & BK7258_SDIO_DATA_RECEIVE_END) != 0)
        {
           if ((status & BK7258_SDIO_DATA_CRC_OK) == 0)
             {
               priv->data_active = false;
               priv->wake_events = SDIOWAIT_ERROR;
               break;
             }

           /* The vendor reader clears the receive-end W1C bit before
            * consuming the completed block from the RX FIFO. */
           sdio_write(BK7258_SDIO_INT_STATUS,
                      BK7258_SDIO_DATA_RECEIVE_END);
           sdio_drain_fifo(priv);
           sdio_write(BK7258_SDIO_INT_STATUS,
                      status & (BK7258_SDIO_DATA_RECEIVE_END |
                                BK7258_SDIO_DATA_CRC_OK |
                                BK7258_SDIO_DATA_CRC_FAIL |
                                BK7258_SDIO_RX_NEED_READ));
           priv->data_active = false;
           priv->wake_events = priv->remaining == 0 ? SDIOWAIT_TRANSFERDONE :
                               SDIOWAIT_ERROR;
            printf("SDIO data complete status=0x%08lx remaining=%lu event=0x%x\n",
                   (unsigned long)status, (unsigned long)priv->remaining,
                   priv->wake_events);
            break;
        }

      if ((int32_t)(clock_systime_ticks() - deadline) >= 0)
        {
          priv->data_active = false;
          priv->wake_events = SDIOWAIT_TIMEOUT;
          printf("SDIO data timeout status=0x%08lx remaining=%lu\n",
                 (unsigned long)status, (unsigned long)priv->remaining);
          break;
        }

      if (!priv->data_active)
        {
          nxsig_usleep(100);
        }
    }

  priv->wait_events = 0;
  return priv->wake_events;
}

static void sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                sdio_eventset_t eventset)
{
  (void)dev;
  (void)eventset;
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int sdio_registercallback(FAR struct sdio_dev_s *dev,
                                 worker_t callback, FAR void *arg)
{
  (void)dev;
  (void)callback;
  (void)arg;
  return OK;
}
#endif

static const struct sdio_dev_s g_sdio_template =
{
  .mutex          = NXMUTEX_INITIALIZER,
  .reset          = sdio_reset,
  .capabilities   = sdio_capabilities,
  .status         = sdio_status,
  .widebus        = sdio_widebus,
  .clock          = sdio_clock,
  .attach         = sdio_attach,
  .sendcmd        = sdio_sendcmd,
  .blocksetup     = sdio_blocksetup,
  .recvsetup      = sdio_recvsetup,
  .sendsetup      = sdio_sendsetup,
  .cancel         = sdio_cancel,
  .waitresponse   = sdio_waitresponse,
  .recv_r1        = sdio_recv_r1,
  .recv_r2        = sdio_recv_r2,
  .recv_r3        = sdio_recv_r3,
  .recv_r4        = sdio_recv_r4,
  .recv_r5        = sdio_recv_r5,
  .recv_r6        = sdio_recv_r6,
  .recv_r7        = sdio_recv_r7,
  .waitenable     = sdio_waitenable,
  .eventwait      = sdio_eventwait,
  .callbackenable = sdio_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  .registercallback = sdio_registercallback,
#endif
};

FAR struct sdio_dev_s *bk7258_sdio_initialize(int slotno)
{
  int ret;

  if (slotno != 0)
    {
      return NULL;
    }

  if (!g_sdio.initialized)
    {
      ret = bk7258_sdio_clock_request(true);
      if (ret < 0)
        {
          return NULL;
        }

      memset(&g_sdio, 0, sizeof(g_sdio));
      memcpy(&g_sdio.dev, &g_sdio_template, sizeof(g_sdio.dev));
      nxmutex_init(&g_sdio.dev.mutex);
      nxmutex_init(&g_sdio.io_lock);
      sdio_pinmux();
      sdio_reset_hw(&g_sdio);
      modifyreg32(BK7258_SDIO_FIFO, 0, BK7258_SDIO_CLOCK_GATE);
      g_sdio.initialized = true;
    }

  return &g_sdio.dev;
}
