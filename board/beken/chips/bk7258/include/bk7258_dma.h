/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_dma.h
 *
 * BK7258 general DMA driver.
 *
 * Two interfaces:
 *
 *   bk7258_dma_configure() and friends
 *     Single-shot memory-to-memory on channel 0.  Unchanged; callers that
 *     only need a block copy keep working exactly as before.
 *
 *   bk7258_dma_configure_ex()
 *     Any channel, a peripheral at either end with hardware flow control,
 *     REPEAT mode, and a circular destination.  Needed by anything draining
 *     or feeding a FIFO: the hardware JPEG encoder's bitstream leaves only
 *     through a FIFO register, and the audio path wants to stop copying
 *     samples in its interrupt handler.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Channels the unit has.  The controller's per-channel bitmask registers
 * (secure_attr, int_status0/1) are 12 bits wide, which is where this comes
 * from (dma_struct.h).
 */

#define BK7258_DMA_NCHANNELS       12u

/* Peripheral request lines.
 *
 * These are the hardware's req_mux encodings, taken from
 * ap/middleware/soc/bk7258_ap/soc/dma_reg.h (DMA_V_REQ_MUX_*).  They are
 * deliberately not the vendor's dma_dev_t enum values: that enum is an API
 * convenience and dma_ll_dev_to_req_mux() maps it onto these numbers.  JPEG,
 * for instance, is enum entry 28 but request line 0x19.  Getting this wrong
 * does not fail loudly -- the channel simply waits for a request that never
 * comes, or worse, responds to a different peripheral's.
 */

#define BK7258_DMA_DEV_MEM         0x00u   /* DMA_V_REQ_MUX_DTCM  */
#define BK7258_DMA_DEV_AUDIO_TX    0x0du   /* DMA_V_REQ_MUX_AUDIO */
#define BK7258_DMA_DEV_AUDIO_RX    0x0eu   /* DMA_V_REQ_MUX_AUDIO_RX */
#define BK7258_DMA_DEV_JPEG        0x19u   /* DMA_V_REQ_MUX_JPEG */

/* Transfer unit width, as encoded in ctrl.src_data_width/dest_data_width. */

#define BK7258_DMA_WIDTH_8BITS     0u
#define BK7258_DMA_WIDTH_16BITS    1u
#define BK7258_DMA_WIDTH_32BITS    2u

/* AHB burst length, per endpoint.  Same encoding as the vendor's
 * dma_burst_len_t (bk_avdk_smp/ap/include/driver/hal/hal_dma_types.h), which
 * is also the raw field value.  Reset state is SINGLE; the vendor sets
 * SINGLE on the source and INC16 on the destination for its JPEG drain
 * channel, but only on secure-mode builds (dvp_camera_dma_config() under
 * CONFIG_SPE), so it is a throughput choice rather than a requirement.
 */

#define BK7258_DMA_BURST_SINGLE    0u
#define BK7258_DMA_BURST_INC4      1u
#define BK7258_DMA_BURST_INC8      2u
#define BK7258_DMA_BURST_INC16     3u

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*bk7258_dma_done_cb_t)(void *arg);

/* One channel's configuration.
 *
 * dest_loop_start/dest_loop_end describe a circular destination and are
 * applied only when end > start; leave both zero for a linear write.  A
 * FIFO endpoint wants its *_inc false so the address stays put.
 */

struct bk7258_dma_cfg_s
{
  uint8_t  channel;
  uint32_t src_addr;
  uint32_t dest_addr;
  uint32_t transfer_len;      /* Bytes per finish interrupt. */
  uint8_t  src_dev;           /* BK7258_DMA_DEV_* request line. */
  uint8_t  dest_dev;
  bool     src_inc;
  bool     dest_inc;
  bool     repeat;            /* true = REPEAT, false = SINGLE. */
  uint32_t dest_loop_start;
  uint32_t dest_loop_end;
  uint8_t  data_width;        /* BK7258_DMA_WIDTH_*. */
  uint8_t  src_burst;         /* BK7258_DMA_BURST_*, 0 = SINGLE (reset). */
  uint8_t  dest_burst;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void bk7258_dma_init(void);

/* Legacy single-shot memory-to-memory interface, channel 0. */

void bk7258_dma_configure(uint32_t src_addr, uint32_t dest_addr,
                           uint32_t transfer_len);
void bk7258_dma_set_done_callback(bk7258_dma_done_cb_t cb, void *arg);
void bk7258_dma_start(void);
void bk7258_dma_stop(void);
bool bk7258_dma_is_busy(void);
uint32_t bk7258_dma_get_remain_len(void);

/* General interface.  Returns 0, or -1 if the configuration is not
 * expressible (bad channel, zero or oversized length, bad width).
 */

int bk7258_dma_configure_ex(const struct bk7258_dma_cfg_s *cfg);
void bk7258_dma_set_channel_callback(uint8_t channel,
                                     bk7258_dma_done_cb_t cb, void *arg);
void bk7258_dma_start_channel(uint8_t channel);
void bk7258_dma_stop_channel(uint8_t channel);
uint32_t bk7258_dma_get_channel_remain_len(uint8_t channel);

/* Read remain_len and the finish-pending bit from one status-register
 * snapshot.  This matters at a REPEAT boundary: reading them separately can
 * combine two different chunks.  Safe in interrupt context.
 */

void bk7258_dma_get_channel_progress(uint8_t channel,
                                     uint32_t *remain_len,
                                     bool *finish_pending);

/* The channel's live destination pointer; see the implementation for why it
 * can be read while the channel runs.
 */

uint32_t bk7258_dma_get_channel_dest_addr(uint8_t channel);

/* Pushes out a partial data-width unit held on the channel's source side.
 * Call before reading remain_len at the end of a peripheral-sourced
 * transfer, or the last few bytes are missed.  Safe from interrupt context:
 * the poll for completion is bounded.
 */

void bk7258_dma_flush_src_buffer(uint8_t channel);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_DMA_H */
