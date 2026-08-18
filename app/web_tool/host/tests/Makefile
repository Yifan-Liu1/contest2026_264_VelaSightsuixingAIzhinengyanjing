############################################################################
# app/web_tool/host/tests/Makefile
#
# Compiles the board-side pure logic for the host and runs its unit tests.
# This is why wt_protocol.c has no NuttX includes and why wt_queue.c hides its
# one platform dependency behind a macro: the framing edge cases and the drop
# policy can be provoked here in milliseconds, instead of on a board where a
# desynced stream shows up as a corrupted picture and a dropped response shows
# up as a hang.
#
#   make            build and run everything
#   make protocol   just the framing tests
#   make queue      just the queue and ring tests
#   make e2e        the host side against mock_board.py, no hardware needed
#   make tls        the TLS path: pin, token, HTTPS/WSS, full chain
#   make keystore   the browser's encrypted key store (needs node)
#   make wiring     every id app.js touches exists in index.html
#   make clean
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

CC      ?= gcc
CFLAGS  += -std=c11 -O1 -g -I. -I.. -I../..
CFLAGS  += -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined -pthread

HDRS    := ../../wt_protocol.h ../../wt_queue.h

.PHONY: all run protocol queue mailbox kvdb-secret jpeg-header e2e tls keystore wiring provisioning clean

all: run

test_wt_protocol: test_wt_protocol.c ../../wt_protocol.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_wt_protocol.c ../../wt_protocol.c $(LDFLAGS)

test_wt_queue: test_wt_queue.c ../../wt_queue.c ../../wt_protocol.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_wt_queue.c ../../wt_queue.c \
	      ../../wt_protocol.c $(LDFLAGS)

protocol: test_wt_protocol
	./test_wt_protocol

queue: test_wt_queue
	./test_wt_queue

test_board_mbox_abi: test_board_mbox_abi.c \
	../../../../board/beken/chips/bk7258/hardware/bk7258_mbox.h
	$(CC) $(CFLAGS) -o $@ test_board_mbox_abi.c $(LDFLAGS)

mailbox: test_board_mbox_abi
	./test_board_mbox_abi

test_kvdb_secret: test_kvdb_secret.c \
	../../../../app/kvdb_tool/kvdb_secret.h
	$(CC) $(CFLAGS) -o $@ test_kvdb_secret.c $(LDFLAGS)

kvdb-secret: test_kvdb_secret
	./test_kvdb_secret

test_jpeg_header: test_jpeg_header.c \
	../../../../board/beken/chips/bk7258/bk7258_jpeg_entropy.c \
	../../../../board/beken/chips/bk7258/include/bk7258_jpeg_enc.h
	$(CC) $(CFLAGS) \
	      -I../../../../board/beken/chips/bk7258/include \
	      -o $@ test_jpeg_header.c \
	      ../../../../board/beken/chips/bk7258/bk7258_jpeg_entropy.c \
	      $(LDFLAGS)

jpeg-header: test_jpeg_header
	./test_jpeg_header

# The host side against mock_board.py: the bootstrap's four paths, reconnection
# after a reboot, the dropped counter, a refused connection, rejected frames,
# and the backend's own HTTP/WebSocket surface.  Needs no board and no serial
# port, which is the point -- the real board is shared and its port is
# exclusive, so tests that needed it would not get run.
e2e:
	python3 test_e2e.py

# The production transport: the board dials in over TLS and both ends check the
# other -- certificate pin one way, shared token the other.  Also covers the
# browser hop being HTTPS/WSS and the whole chain end to end.
tls:
	python3 test_tls_e2e.py

# Runs in node because that is where WebCrypto lives outside a browser, and the
# key store must not be the one piece nobody tested.
keystore:
	node test_keystore.mjs

# One missing element kills every button wired after it, and the symptom is
# "some buttons do not work" with no hint which edit did it.
wiring:
	python3 test_page_wiring.py

# The provisioning module owns its own tests, but the full regression stays one
# command: a suite nobody remembers to run is a suite that does not exist.
provisioning:
	$(MAKE) -C ../../../provisioning_web/tests run

run: wiring protocol queue mailbox kvdb-secret jpeg-header provisioning e2e tls keystore

clean:
	rm -f test_wt_protocol test_wt_queue test_board_mbox_abi test_kvdb_secret \
	      test_jpeg_header
	rm -rf *.dSYM __pycache__ ../__pycache__
	$(MAKE) -C ../../../provisioning_web/tests clean
