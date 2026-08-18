############################################################################
# development-machine web console service - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = web_tool
PRIORITY  = 100

# Above the default so the accept loop and the command path are not starved by
# whatever the operator started through shell.exec; below the camera
# subsystem's own workers, which must keep producing frames.

STACKSIZE = 8192
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL)

CSRCS   = wt_protocol.c wt_queue.c wt_command.c wt_selftest.c wt_io.c

# mbedTLS headers, as packages/ai_agent does it.  The library is already in
# libapps; only the include path is missing.
CFLAGS += -I$(APPDIR)/crypto/mbedtls/mbedtls/include
MAINSRC = web_tool_main.c

include $(APPDIR)/Application.mk
