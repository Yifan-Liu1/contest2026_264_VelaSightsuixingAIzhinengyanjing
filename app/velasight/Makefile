include $(APPDIR)/Make.defs

PROGNAME  = velasight
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = 8192
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)

CSRCS   = vs_app.c vs_display.c vs_input.c vs_config.c vs_network.c \
          vs_voice.c vs_media.c vs_history.c \
          velasight_font_16_ui.c
CFLAGS += -I$(APPDIR)/packages/demos/contest2026_264_provision_web/include
CFLAGS += -I$(APPDIR)/packages/ai_agent/include
CFLAGS += -I$(APPDIR)/packages/ai_agent/src
CFLAGS += -I$(APPDIR)/netutils/cjson/cJSON
MAINSRC = velasight_main.c

include $(APPDIR)/Application.mk
