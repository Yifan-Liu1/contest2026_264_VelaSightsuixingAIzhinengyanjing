/****************************************************************************
 * SPDX-License-Identifier: Apache-2.0
 *
 * BK7258 SD-NAND SDIO host.  Commands use bounded polling; PIO data
 * completion is interrupt driven and FIFO data is copied in task context.
 * No SDIO DMA is used.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sdio.h>
#include <nuttx/signal.h>
#include <nuttx/wqueue.h>

#include "arm_internal.h"
#include "irq.h"
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
  uint32_t clock_code;
  uint32_t irq_status;
  uint32_t last_cmd;
  int last_cmd_result;
  int data_result;
  bool data_active;
  bool irq_attached;
  bool initialized;
  sem_t data_sem;
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  worker_t callback;
  FAR void *callback_arg;
  sdio_eventset_t callback_events;
  struct work_s callback_work;
#endif
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

static int sdio_recover_hw(struct bk7258_sdio_s *priv)
{
  int ret;

  ret = bk7258_sdio_clock_request(false);
  if (ret < 0)
    {
      return ret;
    }

  ret = bk7258_sdio_clock_request(true);
  if (ret < 0)
    {
      return ret;
    }

  sdio_pinmux();
  sdio_reset_hw(priv);
  modifyreg32(BK7258_SDIO_FIFO, 0, BK7258_SDIO_CLOCK_GATE);
  nxsig_usleep(10000);
  return OK;
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static void sdio_callback_worker(FAR void *arg)
{
  FAR struct bk7258_sdio_s *priv = arg;
  worker_t callback = priv->callback;
  FAR void *callback_arg = priv->callback_arg;
  sdio_eventset_t events = priv->callback_events;
  int ret;

  priv->callback_events = 0;
  if (callback == NULL || (events & SDIOMEDIA_INSERTED) == 0)
    {
      return;
    }

  ret = sdio_recover_hw(priv);
  if (ret < 0)
    {
      printf("SDIO host recovery failed, error=%d\n", ret);
      return;
    }

  printf("SDIO host recovered; retrying fixed-media probe\n");
  callback(callback_arg);
}
#endif

static void sdio_reset_data_state(void)
{
  modifyreg32(BK7258_SDIO_FIFO,
              BK7258_SDIO_FIFO_RX_RESET |
              BK7258_SDIO_FIFO_TX_RESET |
              BK7258_SDIO_STATE_RESET, 0);
}

static int sdio_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct bk7258_sdio_s *priv = &g_sdio;
  uint32_t status;
  uint32_t clear;

  (void)irq;
  (void)context;
  (void)arg;

  status = sdio_read(BK7258_SDIO_INT_STATUS);
  clear = status & (BK7258_SDIO_DATA_RECEIVE_END |
                    BK7258_SDIO_DATA_TIMEOUT |
                    BK7258_SDIO_RX_NEED_READ |
                    BK7258_SDIO_DATA_CRC_OK |
                    BK7258_SDIO_DATA_CRC_FAIL);

  if (priv->data_active &&
      (status & (BK7258_SDIO_DATA_RECEIVE_END |
                 BK7258_SDIO_DATA_TIMEOUT)) != 0)
    {
      priv->irq_status = status;
      if ((status & BK7258_SDIO_DATA_TIMEOUT) != 0)
        {
          priv->data_result = -ETIMEDOUT;
        }
      else if ((status & BK7258_SDIO_DATA_CRC_FAIL) != 0 ||
               (status & BK7258_SDIO_DATA_CRC_OK) == 0)
        {
          priv->data_result = -EIO;
        }
      else
        {
          priv->data_result = OK;
        }

      priv->data_active = false;
      if (clear != 0)
        {
          sdio_write(BK7258_SDIO_INT_STATUS, clear);
        }

      nxsem_post(&priv->data_sem);
    }
  else if (clear != 0)
    {
      sdio_write(BK7258_SDIO_INT_STATUS, clear);
    }

  return OK;
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
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  uint32_t response;
  uint32_t value;

  if ((cmd & MMCSD_DATAXFR_MASK) == MMCSD_WRDATAXFR)
    {
      return -EROFS;
    }

  response = (cmd & MMCSD_RESPONSE_MASK) >> MMCSD_RESPONSE_SHIFT;
  value = ((cmd & MMCSD_CMDIDX_MASK) << BK7258_SDIO_CMD_INDEX_SHIFT);
  if (response != MMCSD_NO_RESPONSE)
    {
      value |= BK7258_SDIO_CMD_RESPONSE;
    }
  if (response == (MMCSD_R2_RESPONSE >> MMCSD_RESPONSE_SHIFT))
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
  priv->last_cmd = cmd;
  priv->last_cmd_result = -EINPROGRESS;
  sdio_write(BK7258_SDIO_CMD_CTRL, value | BK7258_SDIO_CMD_START);
  return OK;
}

static int sdio_waitresponse(FAR struct sdio_dev_s *dev, uint32_t cmd)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  priv->last_cmd_result = sdio_wait_command(priv, cmd);
  return priv->last_cmd_result;
}

static int sdio_response_result(FAR struct bk7258_sdio_s *priv,
                                uint32_t cmd)
{
  if (priv->last_cmd != cmd)
    {
      return -EPROTO;
    }

  return priv->last_cmd_result;
}

static int sdio_recv_r1(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t *value)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  int ret;

  ret = sdio_response_result(priv, cmd);
  if (ret < 0)
    {
      return ret;
    }

  *value = sdio_read(BK7258_SDIO_RESPONSE(0));
  return OK;
}

static int sdio_recv_r2(FAR struct sdio_dev_s *dev, uint32_t cmd,
                        FAR uint32_t value[4])
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  int ret;

  ret = sdio_response_result(priv, cmd);
  if (ret < 0)
    {
      return ret;
    }

  /* The vendor driver assigns response register 0 to CSD_3 (bits
   * 127..96), register 1 to CSD_2, register 2 to CSD_1 and register 3 to
   * CSD_0. This is already the word order expected by NuttX. */
  value[0] = sdio_read(BK7258_SDIO_RESPONSE(0));
  value[1] = sdio_read(BK7258_SDIO_RESPONSE(1));
  value[2] = sdio_read(BK7258_SDIO_RESPONSE(2));
  value[3] = sdio_read(BK7258_SDIO_RESPONSE(3));
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
  priv->buffer = NULL;
  priv->remaining = 0;
  priv->wait_events = 0;
  priv->wake_events = 0;
  priv->irq_status = 0;
  priv->data_result = -ECANCELED;
  while (nxsem_trywait(&priv->data_sem) == OK)
    {
    }
}

static sdio_capset_t sdio_capabilities(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return SDIO_CAPS_1BIT_ONLY;
}

static sdio_statset_t sdio_status(FAR struct sdio_dev_s *dev)
{
  (void)dev;
  return SDIO_STATUS_PRESENT | SDIO_STATUS_WRPROTECTED;
}

static void sdio_widebus(FAR struct sdio_dev_s *dev, bool enable)
{
  (void)dev;
  (void)enable;
}

static void sdio_clock(FAR struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  if (rate == CLOCK_SDIO_DISABLED)
    {
      sdio_write(BK7258_SDIO_DATA_CTRL, 0);
      modifyreg32(BK7258_SDIO_FIFO, BK7258_SDIO_CLOCK_GATE, 0);
    }
  else if (rate == CLOCK_IDMODE)
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
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  int ret;

  if (priv->irq_attached)
    {
      return OK;
    }

  ret = irq_attach(BK7258_IRQ_SDIO, sdio_interrupt, priv);
  if (ret < 0)
    {
      return ret;
    }

  up_enable_irq(BK7258_IRQ_SDIO);
  priv->irq_attached = true;
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
      priv->nblocks == 0 || nbytes != (priv->blocklen * priv->nblocks))
    {
      return -EINVAL;
    }

  /* Match the vendor read setup order: program timeout and clear stale W1C
   * status before asserting the RX FIFO and SD state resets. */
  sdio_write(BK7258_SDIO_DATA_TIMER, BK7258_SDIO_TIMEOUT);
  sdio_write(BK7258_SDIO_INT_STATUS, 0xffffffffu);
  while (nxsem_trywait(&priv->data_sem) == OK)
    {
    }

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
  /* BK7258 V2 uses its multi-block data engine for 512-byte reads even when
   * the card command is CMD17. Short register transfers such as SCR remain
   * in single-block mode. */

  if (priv->blocklen == 512)
    {
      value |= BK7258_SDIO_DATA_MULTIBLOCK;
    }
  /* Keep configuration and start as separate writes, matching the vendor
   * set_read_multi_block_data()/start_receive_data() sequence. */
  priv->buffer = buffer;
  priv->remaining = nbytes;
  priv->irq_status = 0;
  priv->data_result = -EINPROGRESS;
  priv->data_active = true;
  sdio_write(BK7258_SDIO_DATA_CTRL, value);
  modifyreg32(BK7258_SDIO_INT_MASK, 0,
              BK7258_SDIO_DATA_RECEIVE_END |
              BK7258_SDIO_DATA_TIMEOUT);
  sdio_write(BK7258_SDIO_DATA_CTRL, value | BK7258_SDIO_DATA_ENABLE);

  /* The vendor card driver requires this settling interval after starting
   * the receive engine; without it the receive-end event can be missed. */

  nxsig_usleep(2000);
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
  modifyreg32(BK7258_SDIO_INT_MASK,
              BK7258_SDIO_DATA_RECEIVE_END |
              BK7258_SDIO_DATA_TIMEOUT, 0);
  sdio_write(BK7258_SDIO_INT_STATUS, 0xffffffffu);
  priv->data_active = false;
  priv->buffer = NULL;
  priv->remaining = 0;
  priv->wake_events = SDIOWAIT_ERROR;
  priv->data_result = -ECANCELED;
  nxsem_post(&priv->data_sem);
  return OK;
}

static void sdio_waitenable(FAR struct sdio_dev_s *dev,
                            sdio_eventset_t eventset, uint32_t timeout)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  priv->wait_events = eventset;
  priv->wake_events = 0;
  priv->wait_timeout = timeout;
}

static int sdio_drain_fifo(FAR struct bk7258_sdio_s *priv)
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
          return -EIO;
        }

      word = sdio_read(BK7258_SDIO_RX_FIFO);
      memcpy(priv->buffer, &word, copy);
      priv->buffer += copy;
      priv->remaining -= copy;
    }

  return OK;
}

static sdio_eventset_t sdio_eventwait(FAR struct sdio_dev_s *dev)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;
  uint32_t status;
  int ret;

  if ((priv->wait_events & SDIOWAIT_TIMEOUT) != 0)
    {
      ret = nxsem_tickwait_uninterruptible(&priv->data_sem,
                                            MSEC2TICK(priv->wait_timeout));
    }
  else
    {
      ret = nxsem_wait_uninterruptible(&priv->data_sem);
    }

  status = priv->irq_status;
  if (ret < 0)
    {
      priv->data_active = false;
      priv->wake_events = SDIOWAIT_TIMEOUT;
      printf("SDIO data wait timeout status=0x%08lx remaining=%lu\n",
             (unsigned long)sdio_read(BK7258_SDIO_INT_STATUS),
             (unsigned long)priv->remaining);
    }
  else if (priv->data_result < 0)
    {
      priv->wake_events = priv->data_result == -ETIMEDOUT ? SDIOWAIT_TIMEOUT :
                          SDIOWAIT_ERROR;
      printf("SDIO data error=%d status=0x%08lx remaining=%lu\n",
             priv->data_result, (unsigned long)status,
             (unsigned long)priv->remaining);
    }
  else if (sdio_drain_fifo(priv) == OK && priv->remaining == 0)
    {
      priv->wake_events = SDIOWAIT_TRANSFERDONE;
    }
  else
    {
      priv->wake_events = SDIOWAIT_ERROR;
      printf("SDIO FIFO drain failed status=0x%08lx remaining=%lu\n",
             (unsigned long)status, (unsigned long)priv->remaining);
    }

  modifyreg32(BK7258_SDIO_INT_MASK,
              BK7258_SDIO_DATA_RECEIVE_END |
              BK7258_SDIO_DATA_TIMEOUT, 0);
  sdio_reset_data_state();

  priv->wait_events = 0;
  priv->buffer = NULL;
  priv->remaining = 0;
  return priv->wake_events;
}

static void sdio_callbackenable(FAR struct sdio_dev_s *dev,
                                sdio_eventset_t eventset)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  int ret;

  priv->callback_events = eventset;
  if (priv->callback != NULL &&
      (eventset & SDIOMEDIA_INSERTED) != 0 &&
      (sdio_status(dev) & SDIO_STATUS_PRESENT) != 0)
    {
      ret = work_queue(HPWORK, &priv->callback_work,
                       sdio_callback_worker, priv, MSEC2TICK(100));
      if (ret < 0)
        {
          printf("SDIO media probe retry queue failed, error=%d\n", ret);
        }
    }
#else
  (void)eventset;
#endif
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int sdio_registercallback(FAR struct sdio_dev_s *dev,
                                 worker_t callback, FAR void *arg)
{
  FAR struct bk7258_sdio_s *priv = (FAR struct bk7258_sdio_s *)dev;

  priv->callback = callback;
  priv->callback_arg = arg;
  priv->callback_events = 0;
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
      memset(&g_sdio, 0, sizeof(g_sdio));
      memcpy(&g_sdio.dev, &g_sdio_template, sizeof(g_sdio.dev));
      nxmutex_init(&g_sdio.dev.mutex);
      nxsem_init(&g_sdio.data_sem, 0, 0);

      ret = sdio_recover_hw(&g_sdio);
      if (ret < 0)
        {
          return NULL;
        }

      g_sdio.initialized = true;
    }

  return &g_sdio.dev;
}
