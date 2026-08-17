/****************************************************************************
 * board/beken/chips/bk7258/bk7258_dma.c
 *
 * BK7258 general DMA driver.  Register layout source: bk_avdk_smp
 * release/v3.1.1 ap/middleware/soc/bk7258_ap/soc/dma_struct.h and
 * hal/dma_ll.h.
 *
 * Two levels of interface, because two very different users need it:
 *
 *   bk7258_dma_configure()/start()/stop()  single-shot memory-to-memory on
 *                                         channel 0, unchanged since this
 *                                         file was written for copying
 *                                         YUV_BUF line batches.
 *   bk7258_dma_configure_ex()             everything else: any channel, a
 *                                         peripheral at either end with
 *                                         hardware flow control, REPEAT
 *                                         mode, and a circular destination.
 *
 * The second form exists because two subsystems were blocked on it: the
 * hardware JPEG encoder has no memory-mapped output at all on this SoC (its
 * bitstream leaves through a FIFO register that something must drain), and
 * the audio driver is copying FIFOs by hand in its interrupt handler, which
 * it notes will not hold up at 48kHz stereo.  Both need a channel with a
 * peripheral request line; the JPEG side additionally needs REPEAT plus a
 * looping destination so one configuration can absorb a whole frame.
 *
 * Register map (byte offset from BK7258_DMA_BASE):
 *   Channel N's 16-word config group starts at 0x40 + N*0x40:
 *     ctrl            word 0  - bit[0] enable, bit[1] finish_int_en,
 *                     bit[2] half_finish_int_en, bit[3] mode
 *                     (0=SINGLE, 1=REPEAT), bit[4:5] src_data_width,
 *                     bit[6:7] dest_data_width, bit[8] src_addr_inc_en,
 *                     bit[9] dest_addr_inc_en, bit[10] src_addr_loop_en,
 *                     bit[11] dest_addr_loop_en, bit[12:14] chan_prio,
 *                     bit[16:31] transfer_len (encoded as byte_count - 1)
 *     dest_start_addr word 1
 *     src_start_addr  word 2
 *     dest_loop_end   word 3
 *     dest_loop_start word 4
 *     src_loop_end    word 5
 *     src_loop_start  word 6
 *     req_mux         word 7  - bit[0:4] src_req_mux, bit[5:9] dest_req_mux
 *     status          word 12 - bit[0:16] remain_len, bit[17] flush_src_buff,
 *                     bit[18] finish_int, bit[19] half_finish_int,
 *                     bit[20] bus_err_int
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"
#include "irq.h"
#include "bk7258_dma.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_DMA_BASE              0x45020000u  /* SOC_GENER_DMA_REG_BASE */
#define BK7258_DMA_CHANNEL           0u   /* the legacy interface's channel */

#define BK7258_DMA_CHAN_GROUP_BASE   (BK7258_DMA_BASE + 0x40u)
#define BK7258_DMA_CHAN_STRIDE       0x40u  /* 16 words per channel */

/* Unit-level registers (word offsets from BK7258_DMA_BASE). */

#define BK7258_DMA_PRIO_MODE         (BK7258_DMA_BASE + 2u * 4u)
#define BK7258_DMA_SECURE_ATTR       (BK7258_DMA_BASE + 4u * 4u)
#define BK7258_DMA_PRIV_ATTR         (BK7258_DMA_BASE + 5u * 4u)

#define BK7258_DMA_PRIO_SOFT_RESET   (1u << 0)
#define BK7258_DMA_PRIO_MODE_RR      (0u << 2)   /* DMA_V_PRIO_MODE_ROUND_ROBIN */

/* Both attribute registers are per-channel bitmaps, 12 bits wide. */

#define BK7258_DMA_ATTR_ALL_CHANNELS 0xfffu

#define BK7258_DMA_CH_REG(ch, word_off) \
  (BK7258_DMA_CHAN_GROUP_BASE + (ch) * BK7258_DMA_CHAN_STRIDE + \
   (word_off) * 4u)

#define BK7258_DMA_CH_CTRL(ch)            BK7258_DMA_CH_REG(ch, 0)
#define BK7258_DMA_CH_DEST_ADDR(ch)       BK7258_DMA_CH_REG(ch, 1)
#define BK7258_DMA_CH_SRC_ADDR(ch)        BK7258_DMA_CH_REG(ch, 2)
#define BK7258_DMA_CH_DEST_LOOP_END(ch)   BK7258_DMA_CH_REG(ch, 3)
#define BK7258_DMA_CH_DEST_LOOP_START(ch) BK7258_DMA_CH_REG(ch, 4)
#define BK7258_DMA_CH_SRC_LOOP_END(ch)    BK7258_DMA_CH_REG(ch, 5)
#define BK7258_DMA_CH_SRC_LOOP_START(ch)  BK7258_DMA_CH_REG(ch, 6)
#define BK7258_DMA_CH_REQ_MUX(ch)         BK7258_DMA_CH_REG(ch, 7)
#define BK7258_DMA_CH_STATUS(ch)          BK7258_DMA_CH_REG(ch, 12)

/* Channel 0 aliases, so the original code below reads as it did. */

#define BK7258_DMA_CTRL              BK7258_DMA_CH_CTRL(BK7258_DMA_CHANNEL)
#define BK7258_DMA_DEST_ADDR    BK7258_DMA_CH_DEST_ADDR(BK7258_DMA_CHANNEL)
#define BK7258_DMA_SRC_ADDR     BK7258_DMA_CH_SRC_ADDR(BK7258_DMA_CHANNEL)
#define BK7258_DMA_STATUS       BK7258_DMA_CH_STATUS(BK7258_DMA_CHANNEL)

#define BK7258_DMA_CTRL_ENABLE           (1u << 0)
#define BK7258_DMA_CTRL_FINISH_INT_EN    (1u << 1)
#define BK7258_DMA_CTRL_MODE_SINGLE      (0u << 3)
#define BK7258_DMA_CTRL_MODE_REPEAT      (1u << 3)
#define BK7258_DMA_CTRL_MODE_MASK        (1u << 3)
#define BK7258_DMA_CTRL_SRC_WIDTH_SHIFT  4u
#define BK7258_DMA_CTRL_DEST_WIDTH_SHIFT 6u
#define BK7258_DMA_CTRL_WIDTH_MASK       0x3u
#define BK7258_DMA_CTRL_SRC_INC_EN       (1u << 8)
#define BK7258_DMA_CTRL_DEST_INC_EN      (1u << 9)
#define BK7258_DMA_CTRL_SRC_LOOP_EN      (1u << 10)
#define BK7258_DMA_CTRL_DEST_LOOP_EN     (1u << 11)
#define BK7258_DMA_CTRL_LEN_SHIFT        16u
#define BK7258_DMA_CTRL_LEN_MASK         0xffffu

#define BK7258_DMA_REQ_MUX_SRC_SHIFT     0u
#define BK7258_DMA_REQ_MUX_DEST_SHIFT    5u
#define BK7258_DMA_REQ_MUX_MASK          0x1fu
#define BK7258_DMA_REQ_MUX_SRC_SEC       (1u << 20)
#define BK7258_DMA_REQ_MUX_DEST_SEC      (1u << 21)

#define BK7258_DMA_STATUS_REMAIN_LEN_MASK 0x1ffffu
#define BK7258_DMA_STATUS_FLUSH_SRC_BUFF  (1u << 17)
#define BK7258_DMA_STATUS_FINISH_INT      (1u << 18)

/* Spin bound for the source-buffer flush.  The flush drains at most one
 * data-width unit, so it completes in a handful of bus cycles; the bound is
 * only here so a wedged bus cannot hang an interrupt handler forever.
 */

#define BK7258_DMA_FLUSH_TIMEOUT     1000u

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bk7258_dma_done_cb_t g_done_cb[BK7258_DMA_NCHANNELS];
static void *g_done_cb_arg[BK7258_DMA_NCHANNELS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_dma_isr
 *
 * Description:
 *   Services every channel that has a callback registered.  Scanning is
 *   used rather than the global int_status word because channel-to-interrupt
 *   line assignment goes through the int_allocate register, which this
 *   driver leaves at its reset value; the per-channel finish_int bit is
 *   unambiguous either way, and with one active channel the scan is a
 *   handful of register reads.
 *
 *   A SINGLE-mode channel is disabled once it completes, matching what this
 *   driver has always done: a stale "finish" state would otherwise confuse
 *   the next configure().  A REPEAT-mode channel is left running -- that is
 *   the whole point of REPEAT, and its finish interrupt is a progress
 *   report, not an end.
 *
 *   Interrupt context: no printf, no allocation, no blocking.
 *
 ****************************************************************************/

static int bk7258_dma_isr(int irq, void *context, void *arg)
{
  unsigned int ch;

  for (ch = 0; ch < BK7258_DMA_NCHANNELS; ch++)
    {
      uint32_t status;

      if (g_done_cb[ch] == NULL)
        {
          continue;
        }

      status = getreg32(BK7258_DMA_CH_STATUS(ch));
      if ((status & BK7258_DMA_STATUS_FINISH_INT) == 0)
        {
          continue;
        }

      /* Write-1-to-clear, and only that bit: the low half of this register
       * is the read-only remain_len, and the neighbouring interrupt bits are
       * W1C too, so a read-modify-write would clear whichever of them
       * happened to be pending.
       */

      putreg32(BK7258_DMA_STATUS_FINISH_INT, BK7258_DMA_CH_STATUS(ch));

      if ((getreg32(BK7258_DMA_CH_CTRL(ch)) & BK7258_DMA_CTRL_MODE_MASK) ==
          BK7258_DMA_CTRL_MODE_SINGLE)
        {
          modifyreg32(BK7258_DMA_CH_CTRL(ch), BK7258_DMA_CTRL_ENABLE, 0);
        }

      g_done_cb[ch](g_done_cb_arg[ch]);
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_dma_init
 *
 * Description:
 *   Brings up the DMA unit, then attaches its interrupt.
 *
 *   The unit-level work here was missing while this driver had no callers,
 *   and it is not optional -- a channel programmed without it is configured
 *   correctly and moves nothing:
 *
 *     - soft_reset must be released.  As with YUV_BUF, the bit reads as
 *       "released" when it is 1, so the reset value of 0 means the unit is
 *       held in reset (dma_ll_init(): prio_mode.v = 0 then soft_reset = 1).
 *     - secure_attr and privileged_attr are per-channel bitmaps, and this AP
 *       runs in the secure world (CONFIG_ARCH_TRUSTZONE_SECURE=y, hence the
 *       -mcmse in its compile flags).  The reference opens all twelve
 *       channels in both maps under CONFIG_SPE; leaving them at zero leaves
 *       the channel unable to access secure memory, with no error reported
 *       anywhere.
 *     - prio_mode round-robin matches dma_hal_init().
 *
 ****************************************************************************/

void bk7258_dma_init(void)
{
  putreg32(BK7258_DMA_PRIO_SOFT_RESET | BK7258_DMA_PRIO_MODE_RR,
           BK7258_DMA_PRIO_MODE);
  putreg32(BK7258_DMA_ATTR_ALL_CHANNELS, BK7258_DMA_SECURE_ATTR);
  putreg32(BK7258_DMA_ATTR_ALL_CHANNELS, BK7258_DMA_PRIV_ATTR);

  modifyreg32(BK7258_DMA_CTRL, BK7258_DMA_CTRL_ENABLE, 0);

  irq_attach(BK7258_IRQ_DMA, bk7258_dma_isr, NULL);
  up_enable_irq(BK7258_IRQ_DMA);

  printf("bk7258_dma: init: prio_mode=0x%08x secure_attr=0x%08x "
         "priv_attr=0x%08x irq=%d\n",
         (unsigned int)getreg32(BK7258_DMA_PRIO_MODE),
         (unsigned int)getreg32(BK7258_DMA_SECURE_ATTR),
         (unsigned int)getreg32(BK7258_DMA_PRIV_ATTR),
         (int)BK7258_IRQ_DMA);
}

void bk7258_dma_configure(uint32_t src_addr, uint32_t dest_addr,
                           uint32_t transfer_len)
{
  struct bk7258_dma_cfg_s cfg;

  /* Expressed through the general path so there is one implementation of
   * the register writes, with the memory-to-memory defaults spelled out.
   */

  cfg.channel         = BK7258_DMA_CHANNEL;
  cfg.src_addr        = src_addr;
  cfg.dest_addr       = dest_addr;
  cfg.transfer_len    = transfer_len;
  cfg.src_dev         = BK7258_DMA_DEV_MEM;
  cfg.dest_dev        = BK7258_DMA_DEV_MEM;
  cfg.src_inc         = true;
  cfg.dest_inc        = true;
  cfg.repeat          = false;
  cfg.dest_loop_start = 0;
  cfg.dest_loop_end   = 0;
  cfg.data_width      = BK7258_DMA_WIDTH_8BITS;

  bk7258_dma_configure_ex(&cfg);
}

int bk7258_dma_configure_ex(const struct bk7258_dma_cfg_s *cfg)
{
  uint32_t ctrl;
  uint32_t len_field;

  if (cfg == NULL || cfg->channel >= BK7258_DMA_NCHANNELS ||
      cfg->transfer_len == 0 ||
      cfg->transfer_len > BK7258_DMA_CTRL_LEN_MASK + 1u ||
      cfg->data_width > BK7258_DMA_WIDTH_32BITS)
    {
      return -1;
    }

  /* Disable before reprogramming source/dest/length. */

  modifyreg32(BK7258_DMA_CH_CTRL(cfg->channel),
              BK7258_DMA_CTRL_ENABLE, 0);

  putreg32(cfg->src_addr, BK7258_DMA_CH_SRC_ADDR(cfg->channel));
  putreg32(cfg->dest_addr, BK7258_DMA_CH_DEST_ADDR(cfg->channel));

  /* Request lines, plus the security attribute of each endpoint.
   *
   * The value written for a device is the hardware's req_mux encoding, which
   * is NOT the position of the device in the vendor's dma_dev_t enum --
   * dma_ll_dev_to_req_mux() translates between them (JPEG is enum entry 28
   * but req_mux 0x19).  bk7258_dma.h therefore defines the encodings, not
   * the enum order.
   *
   * src_sec_attr/dest_sec_attr are set because this AP is the secure world;
   * the reference sets both for its JPEG channel under CONFIG_SPE
   * (bk_dma_set_src_sec_attr/bk_dma_set_dest_sec_attr in
   * dvp_camera_dma_config()).  Burst lengths are left at their reset value:
   * the reference's INC16 destination burst is a throughput choice, not a
   * requirement, and one variable at a time.
   */

  putreg32(((uint32_t)cfg->src_dev & BK7258_DMA_REQ_MUX_MASK) <<
             BK7258_DMA_REQ_MUX_SRC_SHIFT |
           ((uint32_t)cfg->dest_dev & BK7258_DMA_REQ_MUX_MASK) <<
             BK7258_DMA_REQ_MUX_DEST_SHIFT |
           BK7258_DMA_REQ_MUX_SRC_SEC | BK7258_DMA_REQ_MUX_DEST_SEC,
           BK7258_DMA_CH_REQ_MUX(cfg->channel));

  /* Circular destination, for a channel that has to keep writing past the
   * end of one transfer_len chunk without software touching it.
   */

  if (cfg->dest_loop_end > cfg->dest_loop_start)
    {
      putreg32(cfg->dest_loop_start,
               BK7258_DMA_CH_DEST_LOOP_START(cfg->channel));
      putreg32(cfg->dest_loop_end,
               BK7258_DMA_CH_DEST_LOOP_END(cfg->channel));
    }

  /* ctrl.transfer_len encodes (byte_count - 1). */

  len_field = (cfg->transfer_len - 1u) & BK7258_DMA_CTRL_LEN_MASK;

  ctrl = BK7258_DMA_CTRL_FINISH_INT_EN |
         (cfg->repeat ? BK7258_DMA_CTRL_MODE_REPEAT :
                        BK7258_DMA_CTRL_MODE_SINGLE) |
         ((uint32_t)cfg->data_width << BK7258_DMA_CTRL_SRC_WIDTH_SHIFT) |
         ((uint32_t)cfg->data_width << BK7258_DMA_CTRL_DEST_WIDTH_SHIFT) |
         (len_field << BK7258_DMA_CTRL_LEN_SHIFT);

  if (cfg->src_inc)
    {
      ctrl |= BK7258_DMA_CTRL_SRC_INC_EN;
    }

  if (cfg->dest_inc)
    {
      ctrl |= BK7258_DMA_CTRL_DEST_INC_EN;
    }

  if (cfg->dest_loop_end > cfg->dest_loop_start)
    {
      ctrl |= BK7258_DMA_CTRL_DEST_LOOP_EN;
    }

  putreg32(ctrl, BK7258_DMA_CH_CTRL(cfg->channel));

  return 0;
}

void bk7258_dma_set_done_callback(bk7258_dma_done_cb_t cb, void *arg)
{
  bk7258_dma_set_channel_callback(BK7258_DMA_CHANNEL, cb, arg);
}

void bk7258_dma_set_channel_callback(uint8_t channel,
                                     bk7258_dma_done_cb_t cb, void *arg)
{
  if (channel < BK7258_DMA_NCHANNELS)
    {
      /* Argument first: the ISR tests the callback pointer to decide whether
       * a channel is in use, so publishing it last means it is never called
       * with a stale argument.
       */

      g_done_cb_arg[channel] = arg;
      g_done_cb[channel] = cb;
    }
}

void bk7258_dma_start(void)
{
  bk7258_dma_start_channel(BK7258_DMA_CHANNEL);
}

void bk7258_dma_stop(void)
{
  bk7258_dma_stop_channel(BK7258_DMA_CHANNEL);
}

void bk7258_dma_start_channel(uint8_t channel)
{
  if (channel < BK7258_DMA_NCHANNELS)
    {
      modifyreg32(BK7258_DMA_CH_CTRL(channel), 0, BK7258_DMA_CTRL_ENABLE);
    }
}

void bk7258_dma_stop_channel(uint8_t channel)
{
  if (channel < BK7258_DMA_NCHANNELS)
    {
      modifyreg32(BK7258_DMA_CH_CTRL(channel), BK7258_DMA_CTRL_ENABLE, 0);
    }
}

bool bk7258_dma_is_busy(void)
{
  return (getreg32(BK7258_DMA_CTRL) & BK7258_DMA_CTRL_ENABLE) != 0;
}

uint32_t bk7258_dma_get_remain_len(void)
{
  return bk7258_dma_get_channel_remain_len(BK7258_DMA_CHANNEL);
}

uint32_t bk7258_dma_get_channel_remain_len(uint8_t channel)
{
  if (channel >= BK7258_DMA_NCHANNELS)
    {
      return 0;
    }

  return getreg32(BK7258_DMA_CH_STATUS(channel)) &
         BK7258_DMA_STATUS_REMAIN_LEN_MASK;
}

/****************************************************************************
 * Name: bk7258_dma_get_channel_dest_addr
 *
 * Description:
 *   The channel's destination address register.  On this DMA word 1 is not a
 *   latched copy of what was programmed: the channel advances it as it
 *   writes, so in REPEAT mode it is the ring's write pointer and can be read
 *   at any time without disturbing the transfer.
 *
 *   That matters for the JPEG drain, which needs to know where one frame's
 *   bitstream ends without stopping the channel -- stopping it is what loses
 *   the bytes of the following frame.
 *
 ****************************************************************************/

uint32_t bk7258_dma_get_channel_dest_addr(uint8_t channel)
{
  if (channel >= BK7258_DMA_NCHANNELS)
    {
      return 0;
    }

  return getreg32(BK7258_DMA_CH_DEST_ADDR(channel));
}

/****************************************************************************
 * Name: bk7258_dma_flush_src_buffer
 *
 * Description:
 *   Pushes out whatever partial data-width unit the channel is holding on
 *   its source side.
 *
 *   Needed at the end of a peripheral-sourced transfer: the last bytes of a
 *   JPEG frame are unlikely to land on a 32-bit boundary, and without this
 *   they sit in the channel and are counted as "not yet transferred", so the
 *   frame looks short by up to three bytes.
 *
 *   Only bit[17] is written, never a read-modify-write, for the reason given
 *   in the interrupt handler: the neighbouring interrupt bits are
 *   write-1-to-clear.  The poll is bounded so this stays safe to call from
 *   interrupt context.
 *
 ****************************************************************************/

void bk7258_dma_flush_src_buffer(uint8_t channel)
{
  unsigned int spins;

  if (channel >= BK7258_DMA_NCHANNELS)
    {
      return;
    }

  putreg32(BK7258_DMA_STATUS_FLUSH_SRC_BUFF,
           BK7258_DMA_CH_STATUS(channel));

  for (spins = 0; spins < BK7258_DMA_FLUSH_TIMEOUT; spins++)
    {
      if ((getreg32(BK7258_DMA_CH_STATUS(channel)) &
           BK7258_DMA_STATUS_FLUSH_SRC_BUFF) == 0)
        {
          break;
        }
    }
}
