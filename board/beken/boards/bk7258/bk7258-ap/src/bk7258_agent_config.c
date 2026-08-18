/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_agent_config.c
 *
 * Hand the API key and endpoint stored in flash to ai_agent, by writing the
 * config file it already reads at start-up.
 *
 * Why it is done this way
 * ----------------------
 * packages/ai_agent is a public repository this project must not modify, so
 * the agent cannot be taught to read our key-value store directly.  It does,
 * however, already persist its own configuration as a flat JSON object
 * (src/infra/config_store.c, AGENT_CONFIG_FILE) and load it on start; and its
 * LLM router keeps backend slots under "llm_backend_<n>" whose value is
 * itself a JSON object (src/llm/llm_router.c).  Writing that file before the
 * agent runs is therefore the supported path: the agent starts already
 * configured, with no patch and no key compiled into the image.
 *
 * What this replaces: packages/ai_agent/include/agent_secrets.h, a header
 * copied into the public tree with the key in it, which put the key into
 * every .bin built from that tree.
 *
 * The file is only written when both a key and a host are stored, and only if
 * the data directory is on a real filesystem -- in the nsh configuration
 * /mnt is not mounted at all, and a missing mount is not an error worth
 * printing on every boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bk7258_kvdb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#  define AGENT_DATA_DIR CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#else
#  define AGENT_DATA_DIR "/mnt/ai_agent"
#endif

#define AGENT_CONFIG_DIR  AGENT_DATA_DIR "/config"
#define AGENT_CONFIG_FILE AGENT_CONFIG_DIR "/config.json"

/* Defaults for the parts of a backend slot that are not worth a key of their
 * own.  MiMo is HTTPS and the OpenAI-compatible path; model falls back to the
 * only one that still exists (mimo-v2-flash and -omni went away 2026-06-30).
 */

#define AGENT_DEFAULT_PATH  "/v1/chat/completions"
#define AGENT_DEFAULT_PORT  "443"
#define AGENT_DEFAULT_MODEL "mimo-v2.5"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void bk7258_kvdb_seed_agent_config(void)
{
  char key[513];
  char host[128];
  char model[64];
  FILE *f;

  if (bk7258_kvdb_get(BK7258_KVDB_KEY_LLM_KEY, key, sizeof(key)) <= 0 ||
      bk7258_kvdb_get(BK7258_KVDB_KEY_LLM_HOST, host, sizeof(host)) <= 0)
    {
      return;
    }

  if (bk7258_kvdb_get(BK7258_KVDB_KEY_LLM_MODEL, model, sizeof(model)) <= 0)
    {
      strlcpy(model, AGENT_DEFAULT_MODEL, sizeof(model));
    }

  /* mkdir the tree.  The agent does this itself as well, but it does it when
   * it starts, which is after this runs.
   */

  mkdir(AGENT_DATA_DIR, 0700);
  mkdir(AGENT_CONFIG_DIR, 0700);

  f = fopen(AGENT_CONFIG_FILE, "w");
  if (f == NULL)
    {
      /* Almost always "no filesystem there yet", which is the normal state of
       * the nsh configuration.  Not worth a line on every boot.
       */

      return;
    }

  /* One flat object, exactly the shape config_store.c writes.  The backend
   * slot's value is a JSON object *as a string*, so its quotes are escaped;
   * that nesting is the router's format, not a mistake.
   */

  fprintf(f,
          "{\"llm_router_profile\":\"kvdb\","
          "\"llm_backend_0\":\""
          "{\\\"host\\\":\\\"%s\\\","
          "\\\"path\\\":\\\"%s\\\","
          "\\\"port\\\":\\\"%s\\\","
          "\\\"api_key\\\":\\\"%s\\\","
          "\\\"model\\\":\\\"%s\\\","
          "\\\"priority\\\":0,"
          "\\\"cost_tier\\\":1}\"}",
          host, AGENT_DEFAULT_PATH, AGENT_DEFAULT_PORT, key, model);

  fclose(f);

  printf("kvdb: ai_agent configured from flash (host=%s model=%s, "
         "key %zu bytes)\n", host, model, strlen(key));
}
