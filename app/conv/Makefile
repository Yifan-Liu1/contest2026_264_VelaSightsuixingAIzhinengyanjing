############################################################################
# VelaSight conversation history - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = conv
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 8192
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_CONV)

CSRCS = conv_store.c conv_net.c conv_ws.c conv_serve.c
MAINSRC = conv_main.c

include $(APPDIR)/Application.mk
