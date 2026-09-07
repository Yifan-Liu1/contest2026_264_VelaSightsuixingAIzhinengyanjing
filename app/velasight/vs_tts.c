/****************************************************************************
 * app/velasight/vs_tts.c
 *
 * The playback worker behind include/vs_tts.h.
 *
 * The RIFF walk and the chunk loop in here started life inside vs_social.c,
 * where they ran on the session thread.  That worked for exactly one caller.
 * Moving them out did three things at once: the session thread no longer
 * spends the length of the audio inside its teardown, the history page gained
 * a way to play a file at all, and the two can no longer reach the DAC
 * simultaneously because there is now one worker between them and it.
 *
 * The generation counter is what makes "stop" and "play something else" the
 * same operation.  Every request bumps it; the loop feeding the DAC compares
 * the generation it started with against the current one on every chunk and
 * leaves if they differ.  A stop is then just a bump with no new path, and the
 * UI thread never has to know whether anything was playing.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arch/chip/bk7258_psram.h>

#include <agent_config.h>

#include "include/vs_audio.h"
#include "include/vs_tts.h"
#include "include/vs_types.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_VS_TTS_STACKSIZE
#  define CONFIG_VS_TTS_STACKSIZE 8192
#endif

/* One read from storage and one write to the DAC per iteration.
 *
 * Sized against the card rather than against the audio.  This is FAT on
 * SD-NAND with a 1-bit bus, single-block PIO transfers and no write buffer or
 * read-ahead underneath (CONFIG_SDIO_WIDTH_D1_ONLY,
 * CONFIG_MMCSD_MULTIBLOCK_LIMIT=1), so a read costs a fixed amount of
 * per-call filesystem work plus one 512-byte command per sector -- and a read
 * that is not sector-aligned pays to fetch the partial sectors at both ends.
 *
 * The constraint is that a chunk has to be read in less time than the chunk
 * lasts, or the DAC starves.  That was once a real problem: the same storage
 * measured 8.9 KB/s against the 32 KB/s playback consumes, so no block size
 * worked and the file had to be read in full before the DAC was opened.
 *
 * It measures 605 KB/s now -- 486400 bytes in 803 ms, 2026-09-07 -- which is
 * 19 times what playback needs.  The margin is that ratio, set by the card
 * against the sample rate, and it does not change with the block size, so the
 * block is now chosen for the memory it holds rather than for throughput.
 * 16 KB is 512 ms of 16 kHz mono and 32 sectors exactly, read in about 27 ms.
 *
 * Reaction time also improves with a smaller block: a stop is acted on at
 * chunk boundaries, so the loop leaves within one read rather than one 2 s
 * read.  Either way nobody hears the difference, because
 * vs_audio_playback_stop() silences the DAC immediately and independently --
 * the loop leaving late only delays the file being closed.
 */

#ifndef CONFIG_VS_TTS_CHUNK_BYTES
#  define CONFIG_VS_TTS_CHUNK_BYTES 16384
#endif

#define VS_TTS_CHUNK CONFIG_VS_TTS_CHUNK_BYTES

/* Largest file that is read into memory before the DAC is opened, or zero to
 * always stream.  Zero by default now; see the reasoning where it is used.
 */

#ifndef CONFIG_VS_TTS_PRELOAD_MAX_BYTES
#  define CONFIG_VS_TTS_PRELOAD_MAX_BYTES 0
#endif

/* How far into a file the RIFF walk is willing to go before deciding the
 * chunk sizes are not trustworthy.  A file whose length fields are garbage --
 * an HTML error page saved under a .wav name, a truncated download -- would
 * otherwise be walked forever.
 */

#define VS_TTS_CHUNK_GUARD 64

#define VS_TTS_TAG "vs_tts"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vs_tts_state_s
{
  pthread_mutex_t lock;
  pthread_cond_t  cond;

  bool running;      /* the worker thread exists */
  bool quit;         /* shutdown requested */

  /* The path the worker should be playing, and the one it actually has open.
   *
   * Two fields rather than one because they answer different questions.
   * wanted is the request, and is cleared as soon as the worker adopts it;
   * active is what is on the speaker right now, and is what vs_tts_play()
   * compares against to decide whether a repeat request is a no-op.
   */

  char wanted[VS_TTS_PATH_MAX];
  char active[VS_TTS_PATH_MAX];

  /* Bumped by every request, including a stop.  See the file header. */

  uint32_t generation;

  /* Published while the DAC is open so the UI thread can silence it.  Cleared
   * under the lock before the worker closes it, which is the contract
   * vs_audio.h states: the owner must stop publishing the pointer before it
   * calls close().
   */

  struct vs_audio_pb_s *pb;

  pthread_t thread;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct vs_tts_state_s g_tts =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .cond = PTHREAD_COND_INITIALIZER
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static unsigned char *vs_tts_alloc(size_t len, bool *from_psram)
{
  unsigned char *p = bk7258_psram_malloc(len);

  *from_psram = p != NULL;
  if (p == NULL)
    {
      p = malloc(len);
    }

  return p;
}

static void vs_tts_free(unsigned char *p, bool from_psram)
{
  if (p == NULL)
    {
      return;
    }

  if (from_psram)
    {
      bk7258_psram_free(p);
    }
  else
    {
      free(p);
    }
}

/****************************************************************************
 * Name: vs_tts_le16 / vs_tts_le32
 *
 * Description:
 *   Read a little-endian field out of a RIFF header without assuming the
 *   host's byte order or that the field is aligned.  Both hold on this chip,
 *   and neither is worth depending on for eight lines of code.
 *
 ****************************************************************************/

static uint64_t vs_tts_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
      return 0;
    }

  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint16_t vs_tts_le16(const unsigned char *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t vs_tts_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/****************************************************************************
 * Name: vs_tts_read_exact
 *
 * Description:
 *   read() that either fills the buffer or says why it could not.  Short
 *   reads are normal on a file and a header parser that treated one as an
 *   error would fail on a perfectly good WAV.
 *
 ****************************************************************************/

static int vs_tts_read_exact(int fd, void *buf, size_t len)
{
  unsigned char *p = buf;
  size_t off = 0;

  while (off < len)
    {
      ssize_t n = read(fd, p + off, len - off);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -ENODATA;
        }

      off += (size_t)n;
    }

  return 0;
}

/****************************************************************************
 * Name: vs_tts_wav_open
 *
 * Description:
 *   Open a WAV file, walk its chunk chain and leave the descriptor positioned
 *   at the first PCM byte.
 *
 * Returned Value:
 *   0 with *fd_out open and the format fields filled, or a negative errno
 *   with nothing left open.
 *
 ****************************************************************************/

static int vs_tts_wav_open(const char *path, int *fd_out, unsigned int *rate,
                           unsigned int *channels, unsigned int *bits,
                           size_t *data_len)
{
  unsigned char head[16];
  bool have_fmt = false;
  size_t file_len = 0;
  off_t end;
  int guard;
  int fd;
  int ret;

  *rate = *channels = *bits = 0;
  *data_len = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  /* The real length, so a data chunk that claims more than the file holds can
   * be clamped.  They are independent claims and reading past the end of a
   * truncated download would play whatever follows it on the card.
   */

  end = lseek(fd, 0, SEEK_END);
  if (end < 0 || lseek(fd, 0, SEEK_SET) < 0)
    {
      ret = -errno;
      goto err;
    }

  file_len = (size_t)end;

  ret = vs_tts_read_exact(fd, head, 12);
  if (ret < 0)
    {
      goto err;
    }

  if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)
    {
      printf("%s: %s is not RIFF/WAVE\n", VS_TTS_TAG, path);
      ret = -EINVAL;
      goto err;
    }

  for (guard = 0; guard < VS_TTS_CHUNK_GUARD; guard++)
    {
      uint32_t size;
      size_t skip;

      ret = vs_tts_read_exact(fd, head, 8);
      if (ret < 0)
        {
          /* Ran off the end without finding data. */

          ret = -EINVAL;
          goto err;
        }

      size = vs_tts_le32(head + 4);
      skip = (size_t)size + (size & 1u); /* chunks are word aligned */

      if (memcmp(head, "data", 4) == 0)
        {
          off_t here;
          size_t avail;

          if (!have_fmt)
            {
              printf("%s: %s has data before fmt\n", VS_TTS_TAG, path);
              ret = -EINVAL;
              goto err;
            }

          /* Ask the descriptor where it is rather than tracking it alongside.
           * A parallel counter has to be adjusted at every read and every
           * seek, and the one place it was missed -- the sixteen fmt bytes
           * read below -- made this overstate what the file held.
           */

          here = lseek(fd, 0, SEEK_CUR);
          if (here < 0)
            {
              ret = -errno;
              goto err;
            }

          avail = file_len > (size_t)here ? file_len - (size_t)here : 0;
          *data_len = (size_t)size < avail ? (size_t)size : avail;

          if (*data_len == 0)
            {
              ret = -ENODATA;
              goto err;
            }

          *fd_out = fd;
          return 0;
        }

      if (memcmp(head, "fmt ", 4) == 0 && size >= 16)
        {
          unsigned char fmt[16];

          ret = vs_tts_read_exact(fd, fmt, sizeof(fmt));
          if (ret < 0)
            {
              goto err;
            }

          if (vs_tts_le16(fmt) != 1)
            {
              printf("%s: %s is compressed, format %u\n", VS_TTS_TAG, path,
                     vs_tts_le16(fmt));
              ret = -ENOTSUP;
              goto err;
            }

          *channels = vs_tts_le16(fmt + 2);
          *rate     = vs_tts_le32(fmt + 4);
          *bits     = vs_tts_le16(fmt + 14);
          have_fmt  = true;
          skip -= sizeof(fmt);
        }

      if (skip > 0 && lseek(fd, (off_t)skip, SEEK_CUR) < 0)
        {
          ret = -errno;
          goto err;
        }
    }

  ret = -EINVAL;

err:
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: vs_tts_superseded
 *
 * Description:
 *   True once the request this playback belongs to has been replaced, which is
 *   how both a stop and a switch to another file arrive here.
 *
 ****************************************************************************/

static bool vs_tts_superseded(uint32_t generation)
{
  bool stale;

  pthread_mutex_lock(&g_tts.lock);
  stale = g_tts.quit || g_tts.generation != generation;
  pthread_mutex_unlock(&g_tts.lock);
  return stale;
}

/****************************************************************************
 * Name: vs_tts_play_file
 *
 * Description:
 *   Play one file to completion, or until the request is superseded.  Runs on
 *   the worker thread.
 *
 *   Everything here is best effort and reported rather than returned: the
 *   callers are a session that has already saved its record and a history page
 *   that has already drawn itself, and neither has anything useful to do with
 *   a failure except say so in the log.
 *
 ****************************************************************************/

static void vs_tts_play_file(const char *path, uint32_t generation)
{
  struct vs_audio_pb_s *pb = NULL;
  unsigned char *buf = NULL;
  bool buf_psram = false;
  size_t data_len = 0;
  size_t done = 0;
  unsigned int rate = 0;
  unsigned int channels = 0;
  unsigned int bits = 0;

  /* Non-zero when buf holds the whole file rather than one chunk of it, which
   * is what tells the loop below to feed from memory instead of from the
   * descriptor.
   */

  size_t preload = 0;

  /* What the storage cost, kept so a chopped playback can be attributed.
   *
   * An underrun count alone does not say why: it looks the same whether the
   * card was slow or the DAC was starved by something else on the core.  Read
   * time against played time answers that -- if the reads took longer than the
   * audio lasts, the card is the reason and nothing else needs looking at.
   */

  uint64_t read_ms = 0;
  unsigned int reads = 0;
  int fd = -1;
  int ret;

  ret = vs_tts_wav_open(path, &fd, &rate, &channels, &bits, &data_len);
  if (ret < 0)
    {
      /* -ENOENT is the ordinary case, not a fault: the seed records ship with
       * an empty ttsMinutes and any session whose download failed has no file
       * either.  Named separately so a missing file is not read as a parse
       * failure by whoever is looking at the log.
       */

      printf("%s: %s %s (%d)\n", VS_TTS_TAG, path,
             ret == -ENOENT ? "has no audio" : "is unplayable", ret);
      return;
    }

  if (bits != 16)
    {
      printf("%s: %s is %u-bit, only 16 is supported\n", VS_TTS_TAG, path,
             bits);
      goto out;
    }

  printf("%s: playing %s, %zu bytes, %u Hz %u ch, %.1f s\n", VS_TTS_TAG, path,
         data_len, rate, channels,
         rate * channels > 0 ?
           (double)data_len / (double)(rate * channels * 2u) : 0.0);

  /* Stream through one small buffer, or read the file in full first when
   * CONFIG_VS_TTS_PRELOAD_MAX_BYTES allows it.
   *
   * Streaming is the default and preloading is the fallback, which is the
   * reverse of how this started.  Preloading existed because the storage could
   * not feed realtime audio: measured 2026-09-07 before the SD-NAND fix,
   * 199680 bytes took 22467 ms to read, 8.9 KB/s, against the 32 KB/s that
   * 16 kHz 16-bit mono consumes.  At 28% of what playback needed a streamed
   * file underran by arithmetic rather than by bad luck, and no block size
   * fixed it -- a bigger read reduces the number of calls, not the bytes per
   * second.  Reading the whole file first moved that waiting to the front,
   * where it was silence before the audio instead of holes inside it.
   *
   * The card measures 605 KB/s now, 19 times what playback consumes, so the
   * premise is gone and preloading only has costs left.  Measured on the run
   * that first played a file end to end: a 486400-byte preload left 103008
   * bytes of PSRAM heap free out of 3014656, 3.4%, with the playback ring
   * holding another 327680.  The budget above it was 1048576 -- so a file
   * slightly larger would have failed to allocate, come back here anyway, and
   * blamed memory for it.  Streaming has neither the peak nor that cliff, nor
   * the 803 ms of silence in front of the audio.
   */

  if (CONFIG_VS_TTS_PRELOAD_MAX_BYTES != 0 &&
      data_len <= CONFIG_VS_TTS_PRELOAD_MAX_BYTES)
    {
      buf = vs_tts_alloc(data_len, &buf_psram);
      if (buf != NULL)
        {
          preload = data_len;
        }
      else
        {
          /* Asked for and refused, which is worth a line: the configuration
           * wanted this file held and the heap could not, so what follows is
           * not what was configured.
           */

          printf("%s: no memory to preload %zu bytes, streaming\n", VS_TTS_TAG,
                 data_len);
        }
    }

  if (buf == NULL)
    {
      buf = vs_tts_alloc(VS_TTS_CHUNK, &buf_psram);
      if (buf == NULL)
        {
          printf("%s: no playback buffer\n", VS_TTS_TAG);
          goto out;
        }
    }

  /* The preload itself, in slices so a stop during it is still prompt. */

  if (preload != 0)
    {
      size_t got = 0;

      while (got < preload)
        {
          size_t want = preload - got;
          uint64_t began;
          ssize_t n;

          if (want > VS_TTS_CHUNK)
            {
              want = VS_TTS_CHUNK;
            }

          if (vs_tts_superseded(generation))
            {
              printf("%s: preload abandoned at %zu of %zu bytes\n", VS_TTS_TAG,
                     got, preload);
              goto out;
            }

          began = vs_tts_now_ms();
          n = read(fd, buf + got, want);
          read_ms += vs_tts_now_ms() - began;
          reads++;

          if (n < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              printf("%s: preload read failed: %d\n", VS_TTS_TAG, errno);
              goto out;
            }

          if (n == 0)
            {
              /* Shorter than the header claimed even after clamping to the
               * file size.  Play what arrived.
               */

              break;
            }

          got += (size_t)n;
        }

      /* What was actually read is what there is to play. */

      data_len = got;
      printf("%s: preloaded %zu bytes in %lu ms (%lu KB/s)\n", VS_TTS_TAG,
             data_len, (unsigned long)read_ms,
             (unsigned long)(read_ms > 0 ? data_len / read_ms : 0));

      if (data_len == 0)
        {
          goto out;
        }
    }

  /* The rate goes to the DAC exactly as the file declared it.  This board can
   * only reach 8000, 16000 and 32000 -- see vs_audio_params_valid() -- and it
   * names the rate it rejected, which is the diagnosis.
   */

  pb = vs_audio_playback_open(AGENT_AUDIO_PLAYBACK_DEV, rate, channels, bits);
  if (pb == NULL)
    {
      printf("%s: cannot open the speaker at %u Hz\n", VS_TTS_TAG, rate);
      goto out;
    }

  /* Published before the first write, so a stop arriving during the very first
   * chunk still reaches the handle.
   */

  pthread_mutex_lock(&g_tts.lock);
  g_tts.pb = pb;
  pthread_mutex_unlock(&g_tts.lock);

  while (done < data_len)
    {
      const unsigned char *from;
      size_t want = data_len - done;
      ssize_t n;

      if (want > VS_TTS_CHUNK)
        {
          want = VS_TTS_CHUNK;
        }

      if (vs_tts_superseded(generation))
        {
          vs_audio_playback_stop(pb);
          break;
        }

      if (preload != 0)
        {
          /* Already in memory.  Still handed over a slice at a time rather than
           * in one call, so a stop is acted on without waiting for the rest of
           * the file to be queued.
           */

          from = buf + done;
          n = (ssize_t)want;
        }
      else
        {
          uint64_t began = vs_tts_now_ms();

          from = buf;
          n = read(fd, buf, want);
          read_ms += vs_tts_now_ms() - began;
          reads++;

          if (n < 0)
            {
              if (errno == EINTR)
                {
                  continue;
                }

              printf("%s: read failed: %d\n", VS_TTS_TAG, errno);
              break;
            }

          if (n == 0)
            {
              /* Shorter than the header claimed even after clamping to the
               * file size.  Nothing to do about it but stop; what played,
               * played.
               */

              break;
            }
        }

      ret = vs_audio_playback_write(pb, from, (size_t)n);
      if (ret < 0)
        {
          /* -ECANCELED is a stop that landed between the check above and this
           * write, which is the expected way an interrupted file ends.
           */

          if (ret != -ECANCELED)
            {
              printf("%s: write failed: %d\n", VS_TTS_TAG, ret);
            }

          break;
        }

      done += (size_t)n;
    }

  vs_audio_playback_drain(pb);

  {
    unsigned int underruns = vs_audio_playback_underruns(pb);

    /* Unpublished before the close, per vs_audio.h's threading contract. */

    pthread_mutex_lock(&g_tts.lock);
    g_tts.pb = NULL;
    pthread_mutex_unlock(&g_tts.lock);

    vs_audio_playback_close(pb);

    /* Read time next to played time, so the two questions a chopped playback
     * raises are answered on one line: did it underrun, and was storage the
     * reason.
     */

    printf("%s: %s after %zu of %zu bytes, %s, %u read(s) took %lu ms for "
           "%lu ms of audio, %u underrun(s)\n", VS_TTS_TAG,
           done >= data_len ? "finished" : "stopped", done, data_len,
           preload != 0 ? "preloaded" : "streamed", reads,
           (unsigned long)read_ms,
           (unsigned long)(rate * channels > 0 ?
                           done * 1000ull / (rate * channels * 2u) : 0),
           underruns);
  }

out:
  vs_tts_free(buf, buf_psram);

  if (fd >= 0)
    {
      close(fd);
    }
}

/****************************************************************************
 * Name: vs_tts_worker
 ****************************************************************************/

static void *vs_tts_worker(void *arg)
{
  (void)arg;

  for (; ; )
    {
      char path[VS_TTS_PATH_MAX];
      uint32_t generation;

      pthread_mutex_lock(&g_tts.lock);

      while (!g_tts.quit && g_tts.wanted[0] == '\0')
        {
          pthread_cond_wait(&g_tts.cond, &g_tts.lock);
        }

      if (g_tts.quit)
        {
          pthread_mutex_unlock(&g_tts.lock);
          break;
        }

      /* Adopt the request: it moves from wanted to active, so a repeat of the
       * same path while this is playing is recognised as a no-op and the
       * condition above does not fire again for it.
       */

      snprintf(path, sizeof(path), "%s", g_tts.wanted);
      snprintf(g_tts.active, sizeof(g_tts.active), "%s", g_tts.wanted);
      g_tts.wanted[0] = '\0';
      generation = g_tts.generation;
      pthread_mutex_unlock(&g_tts.lock);

      vs_tts_play_file(path, generation);

      /* Cleared unconditionally, and that has to be unconditional.
       *
       * Guarding it with "only if the generation still matches" looks safer and
       * is the opposite: a stop bumps the generation, so the guard would skip
       * the clear and leave active naming a file that is no longer playing.
       * From then on vs_tts_busy() reports true forever and vs_tts_play() of
       * that same file is a permanent no-op -- the record would never speak
       * again until a reboot.
       *
       * There is no race to guard against anyway.  active is only ever written
       * here and at adoption above, both on this thread, and a newer request
       * lives in wanted until this loop comes back round for it.
       */

      pthread_mutex_lock(&g_tts.lock);
      g_tts.active[0] = '\0';
      pthread_mutex_unlock(&g_tts.lock);
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vs_tts_open(void)
{
  pthread_attr_t attr;
  struct sched_param param;
  int ret;

  pthread_mutex_lock(&g_tts.lock);
  if (g_tts.running)
    {
      pthread_mutex_unlock(&g_tts.lock);
      return 0;
    }

  g_tts.quit       = false;
  g_tts.generation = 1;
  g_tts.wanted[0]  = '\0';
  g_tts.active[0]  = '\0';
  g_tts.pb         = NULL;
  pthread_mutex_unlock(&g_tts.lock);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_VS_TTS_STACKSIZE);

  /* Below the UI, for the same reason the social workers are: this reads a
   * file and tops up a staging ring that vs_audio.c's own drain thread empties
   * at VS_PRIORITY_AUDIO.  Being late here costs a possible underrun, which is
   * counted; being late in the UI costs the key highlight the user is waiting
   * for.
   */

  param.sched_priority = VS_PRIORITY_SOCIAL;
  pthread_attr_setschedparam(&attr, &param);

  ret = pthread_create(&g_tts.thread, &attr, vs_tts_worker, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      printf("%s: worker thread failed: %d\n", VS_TTS_TAG, ret);
      return -ret;
    }

  pthread_mutex_lock(&g_tts.lock);
  g_tts.running = true;
  pthread_mutex_unlock(&g_tts.lock);

  printf("%s: ready\n", VS_TTS_TAG);
  return 0;
}

int vs_tts_play(const char *path)
{
  struct vs_audio_pb_s *pb;

  if (path == NULL || path[0] == '\0')
    {
      return -EINVAL;
    }

  if (strlen(path) >= VS_TTS_PATH_MAX)
    {
      return -ENAMETOOLONG;
    }

  pthread_mutex_lock(&g_tts.lock);

  if (!g_tts.running)
    {
      pthread_mutex_unlock(&g_tts.lock);
      return -ENODEV;
    }

  /* Already on the speaker, or already queued.  See the header for why this is
   * a contract and not an optimisation.
   */

  if (strcmp(g_tts.active, path) == 0 || strcmp(g_tts.wanted, path) == 0)
    {
      pthread_mutex_unlock(&g_tts.lock);
      return 0;
    }

  snprintf(g_tts.wanted, sizeof(g_tts.wanted), "%s", path);
  g_tts.generation++;

  /* Cut the current file short inside the lock, for the same reason
   * vs_social_abort() does: the worker clears g_tts.pb under this lock before
   * closing it, so holding it means the handle is either still valid or
   * already NULL rather than mid-free.
   */

  pb = g_tts.pb;
  vs_audio_playback_stop(pb);
  pthread_cond_broadcast(&g_tts.cond);
  pthread_mutex_unlock(&g_tts.lock);
  return 0;
}

void vs_tts_stop(void)
{
  pthread_mutex_lock(&g_tts.lock);

  if (!g_tts.running)
    {
      pthread_mutex_unlock(&g_tts.lock);
      return;
    }

  /* Nothing to do, and saying so cheaply matters: the history page calls this
   * on every navigation, most of which are not interrupting anything.
   */

  if (g_tts.wanted[0] == '\0' && g_tts.active[0] == '\0')
    {
      pthread_mutex_unlock(&g_tts.lock);
      return;
    }

  g_tts.wanted[0] = '\0';
  g_tts.generation++;
  vs_audio_playback_stop(g_tts.pb);
  pthread_cond_broadcast(&g_tts.cond);
  pthread_mutex_unlock(&g_tts.lock);
}

bool vs_tts_busy(void)
{
  bool busy;

  pthread_mutex_lock(&g_tts.lock);
  busy = g_tts.running &&
         (g_tts.wanted[0] != '\0' || g_tts.active[0] != '\0');
  pthread_mutex_unlock(&g_tts.lock);
  return busy;
}

void vs_tts_current(char *out, size_t len)
{
  if (out == NULL || len == 0)
    {
      return;
    }

  pthread_mutex_lock(&g_tts.lock);
  snprintf(out, len, "%s", g_tts.active);
  pthread_mutex_unlock(&g_tts.lock);
}

void vs_tts_close(void)
{
  pthread_t thread;
  bool running;

  pthread_mutex_lock(&g_tts.lock);
  running = g_tts.running;
  thread  = g_tts.thread;
  g_tts.quit      = true;
  g_tts.running   = false;
  g_tts.wanted[0] = '\0';
  g_tts.generation++;
  vs_audio_playback_stop(g_tts.pb);
  pthread_cond_broadcast(&g_tts.cond);
  pthread_mutex_unlock(&g_tts.lock);

  if (running)
    {
      pthread_join(thread, NULL);
    }
}
