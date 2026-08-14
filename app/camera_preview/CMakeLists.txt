# GC2145 -> GC9D01 live camera preview
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_CAMERA_PREVIEW)
  nuttx_add_application(
    NAME camera_preview
    SRCS camera_preview_main.c preview_face.c preview_jpeg.c
    STACKSIZE 4096)
endif()
