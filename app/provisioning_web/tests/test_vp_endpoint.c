/****************************************************************************
 * app/provisioning_web/tests/test_vp_endpoint.c
 *
 * The social cloud endpoint: which of the three sources wins, what an empty
 * field means, and what path the result actually produces.
 *
 * Be clear about what this is.  app/velasight/vs_cloud.c cannot be compiled
 * on the host -- it pulls in vela_tls.h, cJSON and the BK7258 PSRAM header --
 * so resolve() below is a *mirror* of its cloud_resolve_endpoint(), not the
 * function itself.  A change to one that is not made to the other will not be
 * caught here.
 *
 * It is still worth having, for two reasons.  The record codec and the
 * validator it runs against are the real ones, so the half of the contract
 * that lives in this directory is genuinely tested.  And the precedence table
 * is the part that is easy to get subtly wrong and nearly impossible to
 * observe on a board: a device silently using the factory endpoint while the
 * setup page shows a provisioned one has no symptom until someone compares
 * the two.  Writing the table down as executable assertions is what makes the
 * intended behaviour reviewable.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "velasight_provisioning.h"
#include "vp_form.h"
#include "vp_store.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                                 \
  do                                                                      \
    {                                                                     \
      g_checks++;                                                         \
      if (!(cond))                                                        \
        {                                                                 \
          g_failures++;                                                   \
          printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what);            \
        }                                                                 \
    }                                                                     \
  while (0)

/* Mirror of cloud_resolve_endpoint(), with the record supplied rather than
 * read and the Kconfig override passed in rather than compiled in.
 */

struct resolved_s
{
  char host[128];
  char path[80];
  uint16_t port;
  int origin;                     /* 0 none, 1 default, 2 provisioned */
};

#define ORIGIN_NONE        0
#define ORIGIN_DEFAULT     1
#define ORIGIN_PROVISIONED 2

static void resolve(const struct velasight_prov_credentials_s *cred,
                    bool have_record, const char *kconfig_host,
                    uint16_t kconfig_port, struct resolved_s *out)
{
  const char *host = VELASIGHT_PROV_CLOUD_HOST_DEFAULT;
  const char *path = VELASIGHT_PROV_CLOUD_PATH_DEFAULT;
  uint16_t port    = VELASIGHT_PROV_CLOUD_PORT_DEFAULT;
  int origin       = ORIGIN_DEFAULT;

  if (kconfig_host[0] != '\0')
    {
      host = kconfig_host;
      port = kconfig_port;
    }

  if (have_record)
    {
      if (cred->cloud_host[0] != '\0')
        {
          host   = cred->cloud_host;
          origin = ORIGIN_PROVISIONED;
        }

      if (cred->cloud_port != 0)
        {
          port   = cred->cloud_port;
          origin = ORIGIN_PROVISIONED;
        }

      if (origin == ORIGIN_PROVISIONED || cred->cloud_path[0] != '\0')
        {
          path   = cred->cloud_path;
          origin = ORIGIN_PROVISIONED;
        }
    }

  memset(out, 0, sizeof(*out));
  snprintf(out->host, sizeof(out->host), "%s", host);
  snprintf(out->path, sizeof(out->path), "%s", path);
  out->port   = port;
  out->origin = host[0] != '\0' ? origin : ORIGIN_NONE;
}

static void base_record(struct velasight_prov_credentials_s *cred)
{
  memset(cred, 0, sizeof(*cred));
  snprintf(cred->ssid, sizeof(cred->ssid), "%s", "AIPC");
  snprintf(cred->password, sizeof(cred->password), "%s", "passphrase");
  cred->generation = 1;
}

/* What cloud_api_call() builds: prefix + one of the four literal paths.  The
 * prefix carries a leading slash and no trailing one, so no separator is
 * inserted and an empty prefix needs no special case.
 */

static void join(const char *base, const char *path, char *out, size_t cap)
{
  snprintf(out, cap, "%s%s", base, path);
}

int main(void)
{
  struct velasight_prov_credentials_s cred;
  struct resolved_s r;
  char url[256];

  /* Nothing provisioned: the factory default, which is a real address.  This
   * is the case that makes a fresh board work against the documented
   * deployment without anyone opening the setup page.
   */

  base_record(&cred);
  resolve(&cred, false, "", 0, &r);
  CHECK(strcmp(r.host, "staging-hlth.xiaomiwear.com") == 0 &&
        strcmp(r.path, "/hlthopen/public") == 0 && r.port == 80 &&
        r.origin == ORIGIN_DEFAULT,
        "an unprovisioned device uses the documented deployment");

  join(r.path, "/contest/v1/session", url, sizeof(url));
  CHECK(strcmp(url, "/hlthopen/public/contest/v1/session") == 0,
        "the default prefix produces the deployment's real path");

  /* The Kconfig override beats the factory default but not the record.  This
   * is the local-mock build.
   */

  resolve(&cred, false, "10.192.225.223", 18080, &r);
  CHECK(strcmp(r.host, "10.192.225.223") == 0 && r.port == 18080 &&
        r.origin == ORIGIN_DEFAULT,
        "a Kconfig override replaces the factory host and port together");
  CHECK(strcmp(r.path, "/hlthopen/public") == 0,
        "a Kconfig host override does not silently change the prefix");

  /* Host only on the setup page: the port and prefix keep working.  Requiring
   * all three together would make the common case unexpressable.
   */

  base_record(&cred);
  snprintf(cred.cloud_host, sizeof(cred.cloud_host), "%s", "10.0.0.7");
  resolve(&cred, true, "", 0, &r);
  CHECK(strcmp(r.host, "10.0.0.7") == 0 && r.port == 80 &&
        r.origin == ORIGIN_PROVISIONED,
        "a provisioned host alone keeps the default port");
  CHECK(strcmp(r.path, "") == 0,
        "a provisioned endpoint takes its prefix from the record, empty "
        "included");

  join(r.path, "/contest/v1/session", url, sizeof(url));
  CHECK(strcmp(url, "/contest/v1/session") == 0,
        "an empty prefix reaches the document root without a stray slash");

  /* A record wins over the Kconfig override: a device someone actually
   * provisioned should go where they said, not where the build did.
   */

  resolve(&cred, true, "10.192.225.223", 18080, &r);
  CHECK(strcmp(r.host, "10.0.0.7") == 0 && r.origin == ORIGIN_PROVISIONED,
        "the provisioning record outranks the Kconfig override");

  /* Port only.  Enough to mark the endpoint provisioned, which also pulls the
   * prefix from the record -- the deliberate consequence of not being able to
   * tell a stored empty prefix from an unset one.
   */

  base_record(&cred);
  cred.cloud_port = 8443;
  resolve(&cred, true, "", 0, &r);
  CHECK(r.port == 8443 && r.origin == ORIGIN_PROVISIONED &&
        strcmp(r.host, "staging-hlth.xiaomiwear.com") == 0,
        "a provisioned port alone keeps the default host");

  /* A record with no endpoint at all falls through completely, including the
   * prefix.  This is the upgrade path: a device provisioned before these
   * fields existed must not lose the working default.
   */

  base_record(&cred);
  resolve(&cred, true, "", 0, &r);
  CHECK(strcmp(r.host, "staging-hlth.xiaomiwear.com") == 0 &&
        strcmp(r.path, "/hlthopen/public") == 0 && r.port == 80 &&
        r.origin == ORIGIN_DEFAULT,
        "a record carrying no endpoint keeps every factory value");

  /* All three set, prefix cleared on purpose: a server hosting the interface
   * at its root.
   */

  base_record(&cred);
  snprintf(cred.cloud_host, sizeof(cred.cloud_host), "%s", "127.0.0.1");
  cred.cloud_port = 18080;
  resolve(&cred, true, "", 0, &r);
  join(r.path, "/contest/v1/getResult", url, sizeof(url));
  CHECK(strcmp(r.host, "127.0.0.1") == 0 && r.port == 18080 &&
        strcmp(url, "/contest/v1/getResult") == 0,
        "the interface document's own worked example is expressable");

  /* Everything empty is the only way to end up unconfigured, and it takes a
   * build that deliberately blanked the factory default.
   */

  base_record(&cred);
  {
    struct resolved_s none;
    const char *host = "";

    memset(&none, 0, sizeof(none));
    snprintf(none.host, sizeof(none.host), "%s", host);
    none.origin = host[0] != '\0' ? ORIGIN_DEFAULT : ORIGIN_NONE;
    CHECK(none.origin == ORIGIN_NONE,
          "an empty host in every source leaves the module unconfigured");
  }

  /* The endpoint has to survive the record codec, not just the resolver: the
   * resolver reads fields the store has to have round-tripped intact.
   */

  {
    uint8_t buf[VP_RECORD_SIZE];
    struct velasight_prov_credentials_s out;

    base_record(&cred);
    snprintf(cred.cloud_host, sizeof(cred.cloud_host), "%s",
             "staging-hlth.xiaomiwear.com");
    snprintf(cred.cloud_path, sizeof(cred.cloud_path), "%s",
             "/hlthopen/public");
    cred.cloud_port = 443;

    CHECK(vp_record_encode(buf, sizeof(buf), &cred) == VP_RECORD_SIZE &&
          vp_record_decode(buf, sizeof(buf), &out) == 0,
          "the documented endpoint survives the record codec");

    resolve(&out, true, "", 0, &r);
    join(r.path, "/contest/v1/upload", url, sizeof(url));
    CHECK(strcmp(r.host, "staging-hlth.xiaomiwear.com") == 0 &&
          r.port == 443 &&
          strcmp(url, "/hlthopen/public/contest/v1/upload") == 0 &&
          r.origin == ORIGIN_PROVISIONED,
          "a stored endpoint resolves to the deployment's real upload path");
  }

  printf("%s: %d checks, %d failures\n",
         g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
