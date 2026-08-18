############################################################################
# AP-mode provisioning web entry - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = provision_web
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_PROVISION_WEB)

CSRCS   = vp_server.c vp_http.c vp_form.c vp_store.c

CFLAGS += -I$(APPDIR)/packages/demos/contest2026_264_provision_web/include
MAINSRC = provisioning_web_main.c

include $(APPDIR)/Application.mk
