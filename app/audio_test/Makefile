############################################################################
# BK7258 internal-DAC audio bring-up test - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = audio_test
PRIORITY  = SCHED_PRIORITY_DEFAULT

# libopus is built with VAR_ARRAYS, so the encoder puts sizeable working
# arrays on the caller's stack rather than in its state object.  8 KiB was not
# enough for it.

STACKSIZE = 32768
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_AUDIO_TEST)

CSRCS = audio_test_ogg.c audio_test_stream.c
MAINSRC = audio_test_main.c

include $(APPDIR)/Application.mk
