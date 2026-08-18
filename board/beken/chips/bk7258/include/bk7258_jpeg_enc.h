/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_jpeg_enc.h
 *
 * BK7258 hardware JPEG encoder (register layer only).
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_JPEG_ENC_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_JPEG_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/* Where the encoded bitstream actually comes out.
 *
 * The BK7258 JPEG block has NO destination-address register: unlike
 * YUV_BUF (which owns em_base_addr/emr_base_addr and writes whole frames
 * into PSRAM by itself), this module pushes the compressed bytes into an
 * internal stream FIFO that somebody else has to drain.  The reference
 * implementation drains it with a DMA channel whose source is the FIFO
 * register:
 *
 *   bk_avdk_smp release/v3.1.1
 *   ap/components/bk_dvp/src/bk_dvp.c dvp_camera_dma_config():
 *     bk_jpeg_enc_get_fifo_addr(&encode_fifo_addr);
 *     handle->dma_channel = bk_fixed_dma_alloc(DMA_DEV_JPEG, DMA_ID_8);
 *     dma_config.src.dev = DMA_DEV_JPEG;
 *   ap/middleware/driver/jpeg_enc/jpeg_driver.c
 *   bk_jpeg_enc_get_fifo_addr(): *fifo_addr = JPEG_R_RX_FIFO;
 *
 * Consequently bk7258_jpeg_enc_set_buffer() only *records* the
 * destination address for the DMA layer that will be built on top of
 * this driver -- there is no register to put it in.  See that function's
 * comment for the full evidence, including why the vendor's
 * misleadingly-named jpeg_ll_set_em_base_addr() is not called on this
 * board's configuration.
 */

/* Called from interrupt context once per encoded frame (EOF interrupt),
 * with the hardware's byte_count_pfrm value for that frame.  MUST be
 * interrupt-safe: no printf(), no blocking, no allocation.
 */

typedef void (*bk7258_jpeg_enc_eof_cb_t)(FAR void *arg, uint32_t bytes);

/* Interrupt/event counters, for task-level diagnostics.  Snapshot with
 * bk7258_jpeg_enc_get_stats(); all counters are cleared by
 * bk7258_jpeg_enc_configure().
 */

struct bk7258_jpeg_enc_stats_s
{
  uint32_t isr_count;        /* Total ISR invocations. */
  uint32_t frame_count;      /* EOF (one encoded frame) events. */
  uint32_t sof_count;        /* SOF (frame start) events. */
  uint32_t err_count;        /* Frame-error events. */
  uint32_t last_status;      /* int_status of the most recent ISR. */
  uint32_t last_bytes;       /* byte_count_pfrm of the last EOF. */
};

/* Powers on the video pipeline, enables the JPEG module clock gate,
 * pulses the module's soft reset, silences/acknowledges its interrupts
 * and attaches/enables its interrupt line.  Must be called once before
 * bk7258_jpeg_enc_configure()/_start().  Task context only (prints
 * diagnostics).  Returns OK, or a negated errno on failure.
 */

int bk7258_jpeg_enc_init(void);

/* Stops the encoder, pulses the soft reset, disables and detaches the
 * interrupt and drops the module clock gate.  Deliberately does NOT
 * power the video pipeline back down: that power domain is shared with
 * YUV_BUF (and H264), so switching it off here would kill a capture
 * path this driver does not own.  Task context only.
 */

void bk7258_jpeg_enc_uninit(void);

/* Programs the encoder for 'width' x 'height': x_pixel/y_pixel (both in
 * units of 8 pixels), the quantisation table, the rate-control target
 * byte window for this resolution, bitrate control, the encoded-size
 * field, and the EOF interrupt enable.  Clears the statistics counters.
 * Both dimensions must be non-zero multiples of 8 no larger than 2040
 * (the x_pixel/y_pixel fields are 8 bits), otherwise -EINVAL is
 * returned and no register is touched.  Task context only (prints
 * diagnostics).
 */

int bk7258_jpeg_enc_configure(uint16_t width, uint16_t height);

/* Records where the caller wants the encoded bitstream to end up.
 * Writes NO register -- this module has no destination-address register
 * (see the file header).  Kept as part of the API so the DMA layer built
 * on top of this driver has one place to publish/query the current
 * output buffer.  Interrupt-safe.
 */

void bk7258_jpeg_enc_set_buffer(uint32_t addr);

/* Address most recently recorded by bk7258_jpeg_enc_set_buffer(), or 0
 * if none.  Interrupt-safe.
 */

uint32_t bk7258_jpeg_enc_get_buffer(void);

/* Absolute address of the encoder's stream FIFO register, to be used as
 * the DMA source when draining the bitstream (the equivalent of
 * bk_jpeg_enc_get_fifo_addr()).  Interrupt-safe.
 */

uint32_t bk7258_jpeg_enc_get_fifo_addr(void);

/* Registers the end-of-frame callback.  Pass cb == NULL to unregister.
 * THE CALLBACK RUNS IN INTERRUPT CONTEXT (from the JPEG EOF interrupt)
 * and receives the frame's byte_count_pfrm value; it must not print,
 * block or allocate.
 */

void bk7258_jpeg_enc_register_callback(bk7258_jpeg_enc_eof_cb_t cb,
                                       FAR void *arg);

/* Starts encoding: acknowledges anything latched while idle, bypasses
 * the module clock gate and sets cfg.jpeg_enc_en.  Leaves every other
 * cfg field (geometry, rate control) as bk7258_jpeg_enc_configure() left
 * it.  Interrupt-safe.
 */

void bk7258_jpeg_enc_start(void);

/* Stops encoding: clears cfg.jpeg_enc_en and the clock-gate bypass, and
 * nothing else -- the geometry/rate-control configuration and the
 * interrupt enables survive, so _start() can resume without
 * reconfiguring.  Interrupt-safe.
 */

void bk7258_jpeg_enc_stop(void);

/* Pulses the module's global soft reset, preserving the configuration.  The
 * recovery step after a frame error; safe from interrupt context.
 */

void bk7258_jpeg_enc_soft_reset(void);

/* True when the output FIFO is drained.  The EOF interrupt means "the
 * encoder stopped producing", not "the bitstream has been delivered", so a
 * frame's length is only final once this reads true.  Safe in interrupt
 * context.
 */

bool bk7258_jpeg_enc_fifo_empty(void);

/* Bytes reserved in front of the bitstream so that the standards-conforming
 * header (which is longer than the one this block emits) fits without moving
 * the entropy data.  The DMA destination is the V4L2 buffer plus this.
 *
 * 256 covers the measured need: the block's own header is about 429 bytes and
 * the replacement is 605, i.e. 176 more.  The slack absorbs a header that
 * comes out slightly longer, e.g. more 0xFF fill.
 */

#define BK7258_JPEG_ENC_PAD           256u

/* Staging room for the SOF0 and DQT segments copied out of the block's
 * header: 19 + 69 + 69 = 157 bytes measured, rounded up.
 */

#define BK7258_JPEG_ENC_STAGE_MAX     192u

/* How far into the buffer to look for the end of the block's header.  Its
 * own header is ~429 bytes; anything much past that means the stream is not
 * shaped as expected and the scan should give up rather than run away.
 */

#define BK7258_JPEG_ENC_HDR_SCAN_MAX  1024u

/* How far past the end of the block's last header segment to look for the
 * block's own SOS.  The block pads with a run of FF fill whose length varies
 * from frame to frame, so the SOS does not sit at a fixed offset; 48 bytes
 * covers every run measured on this board with room to spare.  FF DA cannot
 * occur inside entropy data, so a wider window cannot cause a false hit.
 */

#define BK7258_JPEG_ENC_SOS_SCAN      48u

/* Find the hardware block's real SOS anywhere in its bounded raw-header
 * window.  The parser fallback can lie before, after, or inside this segment:
 * captured bad frames exposed suffixes such as 11 00 3f 00 at the declared
 * entropy start.  JPEG byte-stuffs FF in entropy as FF 00, so a validated
 * FF DA / length 12 / three-component segment cannot be entropy data.
 */

static inline size_t bk7258_jpeg_find_sos_entropy(
    FAR const uint8_t *buf, size_t begin, size_t end, size_t fallback)
{
  size_t scan_begin;
  size_t scan_end;
  size_t i;

  if (buf == NULL || begin >= end)
    {
      return fallback;
    }

  scan_begin = fallback > BK7258_JPEG_ENC_SOS_SCAN ?
               fallback - BK7258_JPEG_ENC_SOS_SCAN : begin;
  if (scan_begin < begin)
    {
      scan_begin = begin;
    }

  scan_end = fallback + BK7258_JPEG_ENC_SOS_SCAN;
  if (scan_end > end)
    {
      scan_end = end;
    }

  for (i = scan_begin; i + 4u < scan_end; i++)
    {
      if (buf[i] == 0xffu && buf[i + 1u] == 0xdau)
        {
          size_t seglen = ((size_t)buf[i + 2u] << 8) | buf[i + 3u];
          size_t entropy = i + 2u + seglen;
          size_t components = seglen >= 6u ? (seglen - 6u) / 2u : 0u;

          if (seglen >= 8u && ((seglen - 6u) & 1u) == 0u &&
              components == buf[i + 4u] && entropy <= scan_end)
            {
              return entropy;
            }
        }
    }

  return fallback;
}

/* Validate one baseline 4:2:2 entropy scan against its exact MCU count.
 * An aligned scan is left untouched and returns 0.  If the hardware inserted
 * 1-7 leading bits, remove the smallest structurally valid count in place,
 * update *len for changed byte stuffing, and return that count.  Return a
 * negated errno without mutation when no alignment validates or capacity is
 * insufficient for the overlap-safe repair.  `capacity` starts at `buf`.
 */

int bk7258_jpeg_realign_entropy(FAR uint8_t *buf, FAR size_t *len,
                                size_t capacity, uint16_t width,
                                uint16_t height);


/* Rewrites the header in front of an encoded frame so that standard decoders
 * accept it, in place and without moving the entropy data.  Returns the
 * offset of the entropy data in buf (== the new header length), or 0 if the
 * bitstream was not shaped as expected, in which case buf is untouched.
 *
 * The block's own AC Huffman table does not match the table it encodes with,
 * and it emits neither a chroma AC table nor an SOS segment, so its header
 * cannot be used as-is; see the function comment and
 * docs/reference/camera.md 14.7.  Interrupt-context safe.
 */

size_t bk7258_jpeg_enc_write_header(FAR uint8_t *buf, size_t pad);

/* Snapshot of the interrupt/event counters.  Interrupt-safe. */

void bk7258_jpeg_enc_get_stats(FAR struct bk7258_jpeg_enc_stats_s *stats);

/* Debug: prints the module identity registers, global_ctrl/cfg/int_en/
 * int_status/byte_count_pfrm/fifo_status/target byte window and the
 * event counters, tagged with the given label.  TASK CONTEXT ONLY -- it
 * uses printf().  Reads raw registers, so it stays meaningful even if
 * the interrupt path is broken: a zero int_status across repeated calls
 * means the hardware produced no event at all, as opposed to an event
 * that was never delivered to the CPU.
 */

void bk7258_jpeg_enc_dump_status(FAR const char *tag);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_JPEG_ENC_H */
