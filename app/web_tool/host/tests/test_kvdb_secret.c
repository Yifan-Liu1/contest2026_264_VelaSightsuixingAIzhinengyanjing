/****************************************************************************
 * Secret-key classification used by the kvdb CLI.
 ****************************************************************************/

#include <stdio.h>

#include "../../../../app/kvdb_tool/kvdb_secret.h"

static int checks;
static int failures;

#define CHECK(cond, text)                    \
  do                                         \
    {                                        \
      checks++;                              \
      if (!(cond))                           \
        {                                    \
          failures++;                        \
          printf("  FAIL %s\n", text);      \
        }                                    \
    }                                        \
  while (0)

int main(void)
{
  CHECK(kvdb_key_is_secret("llm.key"), ".key is secret");
  CHECK(kvdb_key_is_secret("wifi.psk"), ".psk is secret");
  CHECK(kvdb_key_is_secret("web.token"), ".token is secret");
  CHECK(!kvdb_key_is_secret("web.host"), "host is not secret");
  CHECK(!kvdb_key_is_secret("tokenizer"), "only a token suffix is secret");
  CHECK(!kvdb_key_is_secret("monkey"), "key substring is not a suffix");

  printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
