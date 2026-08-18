/****************************************************************************
 * app/web_tool/wt_queue.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wt_queue.h"

/* The syslog channel callback can run in interrupt context, so the ring's
 * index updates need a critical section rather than a mutex -- taking a mutex
 * there is the classic way to deadlock a logging path.  Off-target the
 * producer is an ordinary thread and the ring is exercised by unit tests, so
 * the critical section degrades to nothing and the tests still cover the
 * index arithmetic, which is where the bugs live.
 */

#ifdef __NuttX__
#  include <nuttx/irq.h>
typedef irqstate_t wt_crit_t;
#  define WT_CRIT_ENTER()   up_irq_save()
#  define WT_CRIT_EXIT(f)   up_irq_restore(f)
#else
typedef unsigned long wt_crit_t;
#  define WT_CRIT_ENTER()   0UL
#  define WT_CRIT_EXIT(f)   ((void)(f))
#endif

#include "wt_protocol.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wt_deadline(int timeout_ms, struct timespec *ts)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec  += timeout_ms / 1000;
  ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_nsec -= 1000000000L;
      ts->tv_sec  += 1;
    }
}

/* Slot index of the queued EVT_FRAME, or -1.  There is at most one, which is
 * the whole point of the policy, so a linear scan over a queue this small is
 * cheaper than maintaining a second index.
 */

static int wt_queue_find_frame(struct wt_queue_s *q)
{
  int i;

  for (i = 0; i < q->count; i++)
    {
      int slot = (q->head + i) % q->cap;

      if (q->slots[slot].type == WT_TYPE_EVT_FRAME)
        {
          return slot;
        }
    }

  return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wt_queue_init(struct wt_queue_s *q, int cap)
{
  int ret;

  if (q == NULL || cap < 2)
    {
      return -EINVAL;
    }

  memset(q, 0, sizeof(*q));

  q->slots = calloc((size_t)cap, sizeof(struct wt_item_s));
  if (q->slots == NULL)
    {
      return -ENOMEM;
    }

  q->cap = cap;

  ret = pthread_mutex_init(&q->lock, NULL);
  if (ret != 0)
    {
      free(q->slots);
      q->slots = NULL;
      return -ret;
    }

  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  return 0;
}

void wt_queue_destroy(struct wt_queue_s *q)
{
  int i;

  if (q == NULL || q->slots == NULL)
    {
      return;
    }

  for (i = 0; i < q->count; i++)
    {
      int slot = (q->head + i) % q->cap;

      free(q->slots[slot].payload);
      q->slots[slot].payload = NULL;
    }

  q->count = 0;
  q->frames = 0;

  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  pthread_mutex_destroy(&q->lock);
  free(q->slots);
  q->slots = NULL;
}

int wt_queue_put(struct wt_queue_s *q, uint8_t type, uint16_t req_id,
                 uint8_t *payload, size_t len, int timeout_ms)
{
  int slot;
  int ret = WT_PUT_OK;

  pthread_mutex_lock(&q->lock);

  if (q->closed)
    {
      pthread_mutex_unlock(&q->lock);
      free(payload);
      return -ESHUTDOWN;
    }

  if (type == WT_TYPE_EVT_FRAME)
    {
      /* Replace the frame already waiting, whatever the queue depth: the
       * host only ever wants the newest one, and evicting here means a slow
       * link cannot push responses or log lines out of the queue.
       */

      slot = wt_queue_find_frame(q);
      if (slot >= 0)
        {
          free(q->slots[slot].payload);
          q->slots[slot].payload = payload;
          q->slots[slot].len     = len;
          q->slots[slot].req_id  = req_id;
          q->frame_dropped++;
          pthread_cond_signal(&q->not_empty);
          pthread_mutex_unlock(&q->lock);
          return WT_PUT_OK;
        }

      if (q->count >= q->cap)
        {
          /* No frame to evict and no room: the queue is full of things that
           * must not be dropped, so this frame is the one that goes.
           */

          q->frame_dropped++;
          pthread_mutex_unlock(&q->lock);
          free(payload);
          return WT_PUT_DROPPED;
        }
    }
  else if (type == WT_TYPE_EVT_LOG)
    {
      if (q->count >= q->cap)
        {
          q->log_dropped++;
          pthread_mutex_unlock(&q->lock);
          free(payload);
          return WT_PUT_DROPPED;
        }
    }
  else
    {
      /* RSP and PONG: wait for room.  Failing the whole connection after the
       * timeout is deliberate -- if the sender has not drained a single slot
       * in that long the link is gone, and silently dropping the response
       * would leave the page spinning instead.
       */

      while (q->count >= q->cap && !q->closed)
        {
          if (timeout_ms < 0)
            {
              pthread_cond_wait(&q->not_full, &q->lock);
            }
          else
            {
              struct timespec ts;

              wt_deadline(timeout_ms, &ts);
              if (pthread_cond_timedwait(&q->not_full, &q->lock, &ts) != 0)
                {
                  break;
                }
            }
        }

      if (q->closed)
        {
          pthread_mutex_unlock(&q->lock);
          free(payload);
          return -ESHUTDOWN;
        }

      if (q->count >= q->cap)
        {
          pthread_mutex_unlock(&q->lock);
          free(payload);
          return -ETIMEDOUT;
        }
    }

  slot = (q->head + q->count) % q->cap;
  q->slots[slot].type    = type;
  q->slots[slot].req_id  = req_id;
  q->slots[slot].payload = payload;
  q->slots[slot].len     = len;
  q->count++;

  if (type == WT_TYPE_EVT_FRAME)
    {
      q->frames++;
    }

  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->lock);
  return ret;
}

int wt_queue_get(struct wt_queue_s *q, struct wt_item_s *out, int timeout_ms)
{
  pthread_mutex_lock(&q->lock);

  while (q->count == 0 && !q->closed)
    {
      if (timeout_ms < 0)
        {
          pthread_cond_wait(&q->not_empty, &q->lock);
        }
      else
        {
          struct timespec ts;

          wt_deadline(timeout_ms, &ts);
          if (pthread_cond_timedwait(&q->not_empty, &q->lock, &ts) != 0)
            {
              break;
            }
        }
    }

  if (q->count == 0)
    {
      int ret = q->closed ? -ESHUTDOWN : -ETIMEDOUT;

      pthread_mutex_unlock(&q->lock);
      return ret;
    }

  *out = q->slots[q->head];
  q->slots[q->head].payload = NULL;
  q->head = (q->head + 1) % q->cap;
  q->count--;

  if (out->type == WT_TYPE_EVT_FRAME && q->frames > 0)
    {
      q->frames--;
    }

  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->lock);
  return 0;
}

void wt_queue_close(struct wt_queue_s *q)
{
  pthread_mutex_lock(&q->lock);
  q->closed = 1;
  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_unlock(&q->lock);
}

uint32_t wt_queue_take_log_dropped(struct wt_queue_s *q)
{
  uint32_t n;

  pthread_mutex_lock(&q->lock);
  n = q->log_dropped;
  q->log_dropped = 0;
  pthread_mutex_unlock(&q->lock);
  return n;
}

void wt_queue_frame_sent(struct wt_queue_s *q)
{
  pthread_mutex_lock(&q->lock);
  q->frames_sent++;
  pthread_mutex_unlock(&q->lock);
}

void wt_queue_frame_drop(struct wt_queue_s *q)
{
  pthread_mutex_lock(&q->lock);
  q->frame_dropped++;
  pthread_mutex_unlock(&q->lock);
}

void wt_queue_frame_stats(struct wt_queue_s *q, uint32_t *sent,
                          uint32_t *dropped)
{
  pthread_mutex_lock(&q->lock);
  if (sent != NULL)
    {
      *sent = q->frames_sent;
    }

  if (dropped != NULL)
    {
      *dropped = q->frame_dropped;
    }

  pthread_mutex_unlock(&q->lock);
}

void wt_queue_frame_reset(struct wt_queue_s *q)
{
  pthread_mutex_lock(&q->lock);
  q->frames_sent   = 0;
  q->frame_dropped = 0;
  pthread_mutex_unlock(&q->lock);
}

/* ---- Syslog ring ------------------------------------------------------ */

int wt_logring_init(struct wt_logring_s *r, size_t cap)
{
  if (r == NULL || cap < WT_LOG_LINE_MAX)
    {
      return -EINVAL;
    }

  r->buf = malloc(cap);
  if (r->buf == NULL)
    {
      return -ENOMEM;
    }

  r->cap  = cap;
  r->head = 0;
  r->tail = 0;
  r->lost = 0;
  return 0;
}

void wt_logring_free(struct wt_logring_s *r)
{
  if (r != NULL)
    {
      free(r->buf);
      r->buf = NULL;
      r->cap = 0;
    }
}

void wt_logring_put(struct wt_logring_s *r, const char *data, size_t len)
{
  wt_crit_t flags;
  size_t i;

  if (r == NULL || r->buf == NULL || len == 0)
    {
      return;
    }

  flags = WT_CRIT_ENTER();

  for (i = 0; i < len; i++)
    {
      size_t next = (r->head + 1) % r->cap;

      if (next == r->tail)
        {
          /* Full: drop the oldest byte.  Counting bytes rather than lines is
           * on purpose -- at this level there are no lines yet, and a count
           * of bytes is still enough to tell the host that a gap exists.
           */

          r->tail = (r->tail + 1) % r->cap;
          r->lost++;
        }

      r->buf[r->head] = data[i];
      r->head = next;
    }

  WT_CRIT_EXIT(flags);
}

int wt_logring_getline(struct wt_logring_s *r, char *out, size_t outcap)
{
  wt_crit_t flags;

  if (r == NULL || r->buf == NULL || out == NULL || outcap < 2)
    {
      return -1;
    }

  flags = WT_CRIT_ENTER();

  /* Loop so that a return of 0 always means "nothing was consumed".  Blank
   * lines are common in this log (the console emits them between commands)
   * and if an empty line were reported as 0 the caller would read it as "the
   * ring is drained" and stop, one line short, every time.
   */

  for (; ; )
    {
      size_t idx;
      size_t n = 0;
      int found = 0;

      /* Look for a newline without consuming anything yet: a partial line has
       * to stay in the ring so the rest of it can arrive.
       */

      for (idx = r->tail; idx != r->head; idx = (idx + 1) % r->cap)
        {
          if (r->buf[idx] == '\n')
            {
              found = 1;
              break;
            }

          if (++n >= outcap - 1)
            {
              /* No newline within a line's worth of bytes.  Emit what we have
               * rather than waiting: a producer that never emits a newline
               * must not be able to stall the whole log stream.
               */

              found = 1;
              break;
            }
        }

      if (!found)
        {
          WT_CRIT_EXIT(flags);
          return 0;
        }

      /* Copy up to the delimiter, then step over it when it really was one. */

      n = 0;
      while (r->tail != r->head && n < outcap - 1)
        {
          char c = r->buf[r->tail];

          r->tail = (r->tail + 1) % r->cap;

          if (c == '\n')
            {
              break;
            }

          /* Carriage returns come from the console path, not from the log
           * text; keeping them would double every line break in the browser.
           */

          if (c != '\r')
            {
              out[n++] = c;
            }
        }

      if (n == 0)
        {
          continue;             /* blank line: consumed, try the next one */
        }

      out[n] = '\0';
      WT_CRIT_EXIT(flags);
      return (int)n;
    }
}

uint32_t wt_logring_take_lost(struct wt_logring_s *r)
{
  wt_crit_t flags;
  uint32_t n;

  flags = WT_CRIT_ENTER();
  n = r->lost;
  r->lost = 0;
  WT_CRIT_EXIT(flags);
  return n;
}

size_t wt_logring_used(struct wt_logring_s *r)
{
  wt_crit_t flags;
  size_t used;

  flags = WT_CRIT_ENTER();
  used = (r->head + r->cap - r->tail) % r->cap;
  WT_CRIT_EXIT(flags);
  return used;
}
