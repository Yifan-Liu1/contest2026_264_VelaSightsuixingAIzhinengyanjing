/****************************************************************************
 * Host regression test for the BK7258 mailbox ACK wire ABI.
 *
 * CP mailbox_channel.c accepts only CHNL_STATE_COM_FAIL in hdr.state.
 * ACK_STATE_COMPLETE belongs to word 3 (ack_state), not hdr.state.  Putting
 * value 2 in hdr.state leaves the peer's IPC socket stuck RX_IN_PROCESS.
 ****************************************************************************/

#include <stdio.h>
#include <stdint.h>

#include "../../../../board/beken/chips/bk7258/hardware/bk7258_mbox.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, text)                         \
  do                                              \
    {                                             \
      g_checks++;                                 \
      if (!(cond))                                \
        {                                         \
          g_failures++;                           \
          printf("  FAIL %s\n", text);           \
        }                                         \
    }                                             \
  while (0)

int main(void)
{
  struct bk7258_mb_wire_message request = {0};
  struct bk7258_mb_wire_message ack = {0};

  request.header = bk7258_mb_make_header(0x82u, 0u, 0u, 0x5au,
                                         BK7258_MB_CHAN_IPC_RX);

  ack.header = bk7258_mb_make_ack_header(&request, false);
  ack.reserved = BK7258_MB_ACK_STATE_COMPLETE;

  CHECK(bk7258_mb_header_state(&ack) == 0u,
        "successful IPC ACK leaves hdr.state zero");
  CHECK(bk7258_mb_header_ctrl(&ack) == BK7258_MB_CTRL_ACK_BOX,
        "ACK uses the ACK box");
  CHECK(bk7258_mb_header_cmd(&ack) == 0x82u,
        "ACK echoes command");
  CHECK(bk7258_mb_header_seq(&ack) == 0x5au,
        "ACK echoes sequence");
  CHECK(bk7258_mb_header_channel(&ack) == BK7258_MB_CHAN_IPC_RX,
        "ACK echoes logical channel");
  CHECK(ack.reserved == BK7258_MB_ACK_STATE_COMPLETE,
        "ACK_STATE_COMPLETE is carried in word 3");

  ack.header = bk7258_mb_make_ack_header(&request, true);
  CHECK(bk7258_mb_header_state(&ack) == BK7258_MB_STATE_COM_FAIL,
        "failed ACK sets only COM_FAIL in hdr.state");

  request.header = bk7258_mb_make_header(0x02u, 0u, 0u, 0x11u,
                                         BK7258_MB_CHAN_IPC_RX);
  request.payload_address = 0x005a0150u; /* dst AP 0x50, src CP 0x01, tag 0x5a */
  request.payload_length = 16u;
  request.flags = 0xacu;
  request.crc8 = 3u;
  request.reserved = 0x28070000u;

  bk7258_mb_ipc_make_response(&request, &ack);
  CHECK(bk7258_mb_header_cmd(&ack) == 0x82u,
        "IPC response sets the response bit");
  CHECK(ack.payload_address == 0x005a5001u,
        "IPC response swaps endpoints, preserves tag and clears status");
  CHECK(ack.payload_length == request.payload_length &&
        ack.flags == request.flags && ack.crc8 == request.crc8 &&
        ack.reserved == request.reserved,
        "IPC response echoes descriptor fields");
  CHECK(bk7258_mb_ipc_tag(&ack) == 0x5au,
        "IPC tag extraction matches param1 byte 2");
  CHECK(bk7258_mb_ipc_is_response(&ack, 0x02u, 0x5au),
        "IPC response matcher accepts command and tag");
  CHECK(!bk7258_mb_ipc_is_response(&ack, 0x00u, 0x5au),
        "IPC response matcher rejects another command");

  printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
