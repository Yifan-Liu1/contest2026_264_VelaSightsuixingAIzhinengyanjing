############################################################################
# app/provisioning_web/tests/Makefile
#
# The provisioning logic compiled for the host and run under the sanitizers.
# vp_form.c, vp_store.c and vp_http.c carry no NuttX includes precisely so
# this can happen: a rejected passphrase length or a corrupt record is a
# millisecond here, and a re-flash plus a phone plus a SoftAP switch there.
# vp_server.c only needs POSIX sockets, so even the accept loop, the callback
# ordering and one-shot mode are covered without hardware.
#
#   make          build and run everything
#   make form     URL decoding and credential validation
#   make store    the record format and the atomic replacement
#   make http     the request surface, escaping and the pages
#   make server   the listener over loopback, end to end
#   make clean
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

CC      ?= gcc
CFLAGS  += -std=c11 -O1 -g -I. -I.. -I../include
# sync(), mkdtemp() and friends are POSIX, and -std=c11 alone hides them.
CFLAGS  += -D_DEFAULT_SOURCE
CFLAGS  += -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined -pthread

HDRS    := ../vp_form.h ../vp_store.h ../vp_http.h \
           ../include/velasight_provisioning.h

.PHONY: all run form store http server clean

all: run

test_vp_form: test_vp_form.c ../vp_form.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_vp_form.c ../vp_form.c $(LDFLAGS)

test_vp_store: test_vp_store.c ../vp_store.c ../vp_form.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_vp_store.c ../vp_store.c ../vp_form.c \
	      $(LDFLAGS)

test_vp_http: test_vp_http.c ../vp_http.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_vp_http.c ../vp_http.c $(LDFLAGS)

test_vp_server: test_vp_server.c ../vp_server.c ../vp_http.c ../vp_form.c \
	../vp_store.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_vp_server.c ../vp_server.c ../vp_http.c \
	      ../vp_form.c ../vp_store.c $(LDFLAGS)

form: test_vp_form
	./test_vp_form

store: test_vp_store
	./test_vp_store

http: test_vp_http
	./test_vp_http

server: test_vp_server
	./test_vp_server

run: form store http server

clean:
	rm -f test_vp_form test_vp_store test_vp_http test_vp_server
	rm -rf *.dSYM
