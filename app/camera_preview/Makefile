############################################################################
# GC2145 -> GC9D01 live camera preview - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = camera_preview
PRIORITY  = SCHED_PRIORITY_DEFAULT

# The libyuv conversion calls are the deep part of the call graph here; 4KB
# leaves room for them plus the V4L2/fb ioctl paths.

STACKSIZE = 4096
MODULE    = $(CONFIG_LVX_USE_DEMO_CONTEST2026_264_CAMERA_PREVIEW)

CSRCS   = preview_face.c preview_jpeg.c
MAINSRC = camera_preview_main.c

include $(APPDIR)/Application.mk
