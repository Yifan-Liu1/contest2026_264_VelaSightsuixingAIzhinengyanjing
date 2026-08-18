/****************************************************************************
 * app/web_tool/wt_queue.h
 *
 * The bounded send queue and the syslog ring buffer.
 *
 * Four producers (command handling, camera thread, syslog channel, shell
 * relay) hand work to one sender thread.  Nobody writes the socket directly:
 * pushing 33 KB into a slow connection blocks, and if the blocked writer were
 * the camera thread holding a socket lock then command responses would be
 * stuck behind a preview nobody is looking at.  One writer also means the
 * socket itself needs no lock.
 *
 * Drop policy is per class, because the classes fail differently:
 *
 *   RSP        never dropped -- a lost response leaves the page waiting for
 *              ever.  Producers block instead, with a timeout that fails the
 *              connection rather than the request.
 *   EVT_LOG    dropped when the queue is full, but counted, and the count is
 *              reported to the host.  A debug tool that makes you believe you
 *              have seen every line is worse than one that admits a gap.
 *   EVT_FRAME  at most one queued; a new frame replaces the waiting one.
 *              Preview wants "now", not "all".
 *
 * The syslog side is separate on purpose: syslog can be called from interrupt
 * context and while holding kernel locks, so the channel callback does one
 * memcpy into a ring and nothing else.  Turning bytes into lines and frames
 * is the sender thread's job.
 *
 * This file compiles on the host too (see host/tests) so the drop policy can
 * be tested without a board; the only platform dependency is the critical
 * section used by the ring, which becomes a no-op off-target where the
 * producer is an ordinary thread.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_WEB_TOOL_WT_QUEUE_H
#define __APP_WEB_TOOL_WT_QUEUE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Longest single log line handed to the host.  NSH's own line limit is 256
 * (CONFIG_NSH_LINELEN) and syslog lines are shorter still; anything longer is
 * split rather than dropped.
 */

#define WT_LOG_LINE_MAX   256

/* Worst case for one escaped log line: every byte becomes \u00xx, plus the
 * timestamp and braces.  1600 bytes.
 *
 * Buffers this size are allocated on the heap, once per thread, never on the
 * stack.  CONFIG_PTHREAD_STACK_DEFAULT here is 4096, and 1600 + a 256-byte
 * line + what vsnprintf() needs does not reliably fit -- the first board run
 * of this service put the log pump on a default-sized sender thread and the
 * result was a corrupted frame length on the wire, which reads as a protocol
 * bug at the far end rather than as a stack overflow on this one.
 */

#define WT_LOG_BODY_MAX   (WT_LOG_LINE_MAX * 6 + 64)

/* wt_queue_put() results. */

#define WT_PUT_OK          0
#define WT_PUT_DROPPED     1    /* counted, payload freed by the queue     */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct wt_item_s
{
  uint8_t   type;
  uint16_t  req_id;
  uint8_t  *payload;    /* malloc'd; ownership follows the item */
  size_t    len;
};

struct wt_queue_s
{
  struct wt_item_s *slots;
  int               cap;
  int               head;      /* next slot to read  */
  int               count;
  int               frames;    /* EVT_FRAME items currently queued */
  int               closed;

  uint32_t          log_dropped;    /* since the last take */
  uint32_t          frame_dropped;  /* lifetime, for camera.stop */
  uint32_t          frames_sent;    /* lifetime, for camera.stop */

  pthread_mutex_t   lock;
  pthread_cond_t    not_empty;
  pthread_cond_t    not_full;
};

/* Byte ring the syslog channel writes into.  Overwrites the oldest bytes
 * when full rather than refusing new ones: the interesting part of a log is
 * almost always the end, and this is also what makes log.subscribe able to
 * replay what happened before the host connected.
 */

struct wt_logring_s
{
  char            *buf;
  size_t           cap;
  volatile size_t  head;      /* producer writes here */
  volatile size_t  tail;      /* consumer reads here  */
  volatile uint32_t lost;     /* bytes overwritten unread */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- Send queue ------------------------------------------------------- */

int  wt_queue_init(struct wt_queue_s *q, int cap);
void wt_queue_destroy(struct wt_queue_s *q);

/****************************************************************************
 * Name: wt_queue_put
 *
 * Description:
 *   Enqueue one frame.  The queue takes ownership of payload in every case,
 *   including when it decides to drop it, so the caller never has to work out
 *   who frees what.
 *
 *   timeout_ms only applies to the classes that must not be dropped (RSP,
 *   PONG); it is ignored for EVT_LOG and EVT_FRAME, which have their own
 *   policies and never block a producer.
 *
 *   Returns WT_PUT_OK, WT_PUT_DROPPED, or a negated errno (-ETIMEDOUT when a
 *   response could not be queued in time, -ESHUTDOWN after close).
 *
 ****************************************************************************/

int wt_queue_put(struct wt_queue_s *q, uint8_t type, uint16_t req_id,
                 uint8_t *payload, size_t len, int timeout_ms);

/****************************************************************************
 * Name: wt_queue_get
 *
 * Description:
 *   Pop the oldest item.  Blocks up to timeout_ms (negative waits for ever).
 *   Returns 0 with *out filled, -ETIMEDOUT, or -ESHUTDOWN after close.
 *   The caller owns out->payload and must free it.
 *
 ****************************************************************************/

int wt_queue_get(struct wt_queue_s *q, struct wt_item_s *out, int timeout_ms);

/* Wake every waiter and refuse further work.  Idempotent. */

void wt_queue_close(struct wt_queue_s *q);

/* Read and clear the dropped-log counter.  Returns 0 when nothing was lost,
 * which is what lets the sender emit the "dropped" notice only when there is
 * something to admit.
 */

uint32_t wt_queue_take_log_dropped(struct wt_queue_s *q);

/* Frame accounting for camera.stop. */

void wt_queue_frame_sent(struct wt_queue_s *q);

/* A frame given up on because the peer was not draining.  Counted the same way
 * as one superseded in the queue: from the far end both are "a frame you did
 * not get", and camera.stop reports the total.
 */

void wt_queue_frame_drop(struct wt_queue_s *q);
void wt_queue_frame_stats(struct wt_queue_s *q, uint32_t *sent,
                          uint32_t *dropped);
void wt_queue_frame_reset(struct wt_queue_s *q);

/* ---- Syslog ring ----------------------------------------------------- */

int  wt_logring_init(struct wt_logring_s *r, size_t cap);
void wt_logring_free(struct wt_logring_s *r);

/****************************************************************************
 * Name: wt_logring_put
 *
 * Description:
 *   Append bytes.  Safe to call from interrupt context: one memcpy and two
 *   index updates under a short critical section, no allocation, no locks
 *   that a thread could be holding.
 *
 ****************************************************************************/

void wt_logring_put(struct wt_logring_s *r, const char *data, size_t len);

/****************************************************************************
 * Name: wt_logring_getline
 *
 * Description:
 *   Remove one line.  Returns its length (newline stripped, NUL added), 0
 *   when no complete line is buffered yet, or -1 on a bad argument.
 *
 *   A run of more than outcap-1 bytes with no newline in it is emitted as a
 *   line of its own instead of stalling for ever: a peer that never sends a
 *   newline must not be able to freeze the log stream.
 *
 ****************************************************************************/

int wt_logring_getline(struct wt_logring_s *r, char *out, size_t outcap);

/* How many bytes the ring has had to overwrite unread, cleared on read. */

uint32_t wt_logring_take_lost(struct wt_logring_s *r);

/* Bytes currently buffered.  Used only for diagnostics. */

size_t wt_logring_used(struct wt_logring_s *r);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEB_TOOL_WT_QUEUE_H */
