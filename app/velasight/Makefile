include $(APPDIR)/Make.defs

PROGNAME  = velasight
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 8192
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)

CSRCS   = vs_app.c vs_display.c vs_input.c vs_config.c vs_network.c \
          velasight_font_16_ui.c
CFLAGS += -I$(APPDIR)/packages/demos/contest2026_264_provision_web/include
MAINSRC = velasight_main.c

include $(APPDIR)/Application.mk
