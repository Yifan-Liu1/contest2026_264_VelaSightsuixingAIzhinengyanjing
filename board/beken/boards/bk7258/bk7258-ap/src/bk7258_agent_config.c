/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_agent_config.c
 *
 * Hand the API key stored in the unified SD-NAND provisioning file to ai_agent,
 * by writing the
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "velasight_provisioning.h"

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

/* config_store.c's own JSON string escaper is not reachable from here (it is
 * a private static in a public repository this project must not modify), so
 * this writes exactly the two characters that would break the surrounding
 * quoted string if left alone.  Volcengine app_id/token are already
 * constrained to printable ASCII with no quote or backslash by
 * vp_form.c's validation before they ever reach SD-NAND, so this is normally
 * a no-op copy; it exists so a future relaxation of that validation cannot
 * turn a token into unterminated JSON here.
 */

static void escape_json_string(const char *in, char *out, size_t outlen)
{
  size_t o = 0;

  while (*in != '\0' && o + 2 < outlen)
    {
      if (*in == '"' || *in == '\\')
        {
          out[o++] = '\\';
        }

      out[o++] = *in++;
    }

  out[o] = '\0';
}

void bk7258_nand_seed_agent_config(void)
{
  struct velasight_prov_credentials_s credentials;
  char host[128];
  char model[64];
  char volc_appid[VELASIGHT_PROV_VOLC_APPID_MAX * 2 + 1];
  char volc_token[VELASIGHT_PROV_VOLC_TOKEN_MAX * 2 + 1];
  bool have_mimo;
  bool have_volc;
  FILE *f;

  if (velasight_provisioning_load(&credentials) < 0)
    {
      return;
    }

  have_mimo = credentials.api_key[0] != '\0';
  have_volc = credentials.volc_appid[0] != '\0' &&
              credentials.volc_token[0] != '\0';

  if (!have_mimo && !have_volc)
    {
      return;
    }

  snprintf(host, sizeof(host), "%s",
           strncmp(credentials.api_key, "tp-", 3) == 0 ?
           "token-plan-cn.xiaomimimo.com" : "api.xiaomimimo.com");
  snprintf(model, sizeof(model), "%s", AGENT_DEFAULT_MODEL);
  escape_json_string(credentials.volc_appid, volc_appid, sizeof(volc_appid));
  escape_json_string(credentials.volc_token, volc_token, sizeof(volc_token));

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
   * that nesting is the router's format, not a mistake.  volc_appkey/
   * volc_token are read directly by volc_asr.c/volc_tts_ws.c's own
   * claw_config_get() calls, at the top level, not nested like the router
   * slot.
   */

  fprintf(f, "{");

  if (have_mimo)
    {
      fprintf(f,
              "\"llm_router_profile\":\"auto\","
              "\"llm_backend_0\":\""
              "{\\\"host\\\":\\\"%s\\\","
              "\\\"path\\\":\\\"%s\\\","
              "\\\"port\\\":\\\"%s\\\","
              "\\\"api_key\\\":\\\"%s\\\","
              "\\\"model\\\":\\\"%s\\\","
              "\\\"priority\\\":0,"
              "\\\"cost_tier\\\":1}\"%s",
              host, AGENT_DEFAULT_PATH, AGENT_DEFAULT_PORT,
              credentials.api_key, model, have_volc ? "," : "");
    }

  if (have_volc)
    {
      fprintf(f,
              "\"volc_appkey\":\"%s\","
              "\"volc_token\":\"%s\"",
              volc_appid, volc_token);
    }

  fprintf(f, "}");
  fclose(f);

  printf("nand: ai_agent configured from %s (mimo host=%s model=%s key "
         "%zu bytes, volc app_id %zu bytes token %zu bytes)\n",
         CONFIG_VELASIGHT_PROVISION_STORE, have_mimo ? host : "(none)",
         have_mimo ? model : "(none)", strlen(credentials.api_key),
         strlen(credentials.volc_appid), strlen(credentials.volc_token));
}

bool bk7258_ai_config_ready(void)
{
  struct velasight_prov_credentials_s credentials;

  return velasight_provisioning_load(&credentials) == 0 &&
         credentials.api_key[0] != '\0';
}
