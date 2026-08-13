/****************************************************************************
 * board/beken/chips/bk7258/bk7258_trng.c
 *
 * BK7258 hardware true random number generator.
 *
 * Why this exists: mbedTLS cannot seed its CTR_DRBG without an entropy
 * source, and ai_agent's agent_secure_random() reads /dev/urandom or
 * /dev/random.  With neither present every TLS handshake failed before a
 * packet was sent:
 *
 *   [vela_tls] CRITICAL: No secure entropy source available
 *   [vela_tls] ctr_drbg_seed ret=0x34   (ENTROPY_SOURCE_FAILED)
 *
 * The first fix for that was NuttX's software entropy pool, whose own Kconfig
 * says it "may not actually be cryptographically secure if not enough
 * entropy is made available".  This driver is the real source: the chip has a
 * TRNG, and this file is what makes it reachable, both as /dev/random and as
 * a feed into that pool (RND_SRC_HW), so /dev/urandom stops being seeded by
 * timer jitter alone.
 *
 * Hardware facts and their provenance are in hardware/bk7258_trng.h.  The
 * enable sequence is the vendor's trng_ll_enable(): assert soft reset, bypass
 * the clock gate, then set the enable bit.  Their comment on the clock gate
 * is worth repeating -- with the gate left enabled the first read of each
 * session returns the same value every time, which is exactly the failure
 * mode that a "looks random enough" test would miss.
 *
 * No power domain request is needed.  The vendor driver's only extra step,
 * sys_drv_trng_disckg_set() around each read, is compiled under
 * CONFIG_SOC_BK7256XX and does not apply to this part.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>
#include <nuttx/random.h>

#include "arm_internal.h"

#include "hardware/bk7258_trng.h"
#include "bk7258_trng.h"

#ifdef CONFIG_BK7258_TRNG

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How many 32-bit words to push into the entropy pool at bring-up.  32 words
 * is 128 bytes, comfortably more than the pool needs to be considered seeded
 * and far more than one TLS handshake consumes.
 */

#define BK7258_TRNG_SEED_WORDS    32

/* How many distinct values a self-test must see out of this many reads.  The
 * failure this guards against is not "the numbers look non-random" -- that
 * needs a statistical suite, not a boot test -- but the two ways this block
 * fails silently: a dead register returning a constant, and the clock-gate
 * mistake above, which repeats one value.
 */

#define BK7258_TRNG_PROBE_READS   8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_trng_dev_s
{
  mutex_t lock;      /* One reader at a time */
  bool    enabled;   /* Hardware has been through the enable sequence */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static ssize_t bk7258_trng_read(struct file *filep, char *buffer,
                                size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct bk7258_trng_dev_s g_trng =
{
  .lock    = NXMUTEX_INITIALIZER,
  .enabled = false,
};

static const struct file_operations g_trng_ops =
{
  NULL,               /* open */
  NULL,               /* close */
  bk7258_trng_read,   /* read */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_trng_enable
 *
 * Description:
 *   Runs the vendor enable sequence, once.  Called lazily rather than from
 *   registration because /dev/random is registered during driver
 *   initialisation, long before board bring-up has finished, and there is no
 *   reason to leave the block running from then on if nobody reads it.
 *
 ****************************************************************************/

static void bk7258_trng_enable(void)
{
  if (g_trng.enabled)
    {
      return;
    }

  putreg32(BK7258_TRNG_SOFT_RESET | BK7258_TRNG_CLK_GATE_BYPASS,
           BK7258_TRNG_GLOBAL_CTRL);
  putreg32(BK7258_TRNG_EN, BK7258_TRNG_CTRL);

  g_trng.enabled = true;
}

/****************************************************************************
 * Name: bk7258_trng_word
 ****************************************************************************/

static uint32_t bk7258_trng_word(void)
{
  bk7258_trng_enable();
  return getreg32(BK7258_TRNG_DATA);
}

/****************************************************************************
 * Name: bk7258_trng_read
 ****************************************************************************/

static ssize_t bk7258_trng_read(struct file *filep, char *buffer,
                                size_t buflen)
{
  union
  {
    uint32_t w;
    uint8_t  b[4];
  } value;

  ssize_t remaining;
  int ret;
  int i;

  ret = nxmutex_lock(&g_trng.lock);
  if (ret < 0)
    {
      return ret;
    }

  /* A byte at a time out of each word, so that an unaligned or non-multiple
   * length is served exactly rather than over-read.
   */

  for (remaining = (ssize_t)buflen; remaining > 0; )
    {
      value.w = bk7258_trng_word();

      for (i = 0; i < (int)sizeof(uint32_t) && remaining > 0;
           i++, remaining--)
        {
          *buffer++ = value.b[i];
        }
    }

  nxmutex_unlock(&g_trng.lock);
  return (ssize_t)buflen;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: devrandom_register
 *
 * Description:
 *   NuttX calls this from drivers_initialize() when CONFIG_DEV_RANDOM is set
 *   and the architecture claims ARCH_HAVE_RNG; providing it is this port's
 *   half of that contract.  The symbol is weak in the drivers layer, so
 *   before this file existed /dev/random simply never appeared.
 *
 ****************************************************************************/

void devrandom_register(void)
{
  register_driver("/dev/random", &g_trng_ops, 0444, NULL);
}

/****************************************************************************
 * Name: bk7258_trng_initialize
 *
 * Description:
 *   Probes the block, reports what it found, and seeds the kernel entropy
 *   pool with hardware randomness.  Called from board bring-up.
 *
 *   Seeding the pool is the part that matters for TLS: /dev/urandom is backed
 *   by the pool (CONFIG_DEV_URANDOM_RANDOM_POOL), and the pool otherwise
 *   collects only interrupt timing.
 *
 * Returned Value:
 *   OK, or -ENODEV when the probe cannot tell the block apart from a dead
 *   register.  Bring-up treats that as non-fatal: the pool still works, it is
 *   just weaker, and the caller reports it rather than refusing to boot.
 *
 ****************************************************************************/

int bk7258_trng_initialize(void)
{
  uint32_t seed[BK7258_TRNG_SEED_WORDS];
  uint32_t first;
  uint32_t dev_id;
  uint32_t version;
  unsigned int distinct = 1;
  unsigned int i;

  dev_id  = getreg32(BK7258_TRNG_DEV_ID);
  version = getreg32(BK7258_TRNG_DEV_VERSION);

  /* Probe: read a few words and count how many differ from the first.  Zero
   * distinct values means a constant, which is what both known silent
   * failures look like.
   */

  first = bk7258_trng_word();
  for (i = 1; i < BK7258_TRNG_PROBE_READS; i++)
    {
      if (bk7258_trng_word() != first)
        {
          distinct++;
        }
    }

  if (distinct <= 1)
    {
      printf("trng: dev_id=0x%08" PRIx32 " version=0x%08" PRIx32
             " but %u reads returned one value; not seeding the pool\n",
             dev_id, version, BK7258_TRNG_PROBE_READS);
      return -ENODEV;
    }

  for (i = 0; i < BK7258_TRNG_SEED_WORDS; i++)
    {
      seed[i] = bk7258_trng_word();
    }

  up_rngaddentropy(RND_SRC_HW, seed, BK7258_TRNG_SEED_WORDS);

  printf("trng: dev_id=0x%08" PRIx32 " version=0x%08" PRIx32
         ", %u/%u reads distinct, %u words seeded into the entropy pool\n",
         dev_id, version, distinct, BK7258_TRNG_PROBE_READS,
         BK7258_TRNG_SEED_WORDS);

  return OK;
}

#endif /* CONFIG_BK7258_TRNG */
