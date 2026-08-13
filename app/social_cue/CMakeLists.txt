# VelaSight social-cue state machine (mock closed loop)
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_SOCIAL_CUE)
  nuttx_add_application(
    NAME social_cue
    SRCS social_cue_main.c
    STACKSIZE 8192)
endif()
