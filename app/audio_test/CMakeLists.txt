# BK7258 internal-DAC audio bring-up test
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_AUDIO_TEST)
  nuttx_add_application(
    NAME audio_test
    SRCS audio_test_main.c audio_test_ogg.c audio_test_stream.c
    # libopus is built with VAR_ARRAYS, so the encoder puts sizeable working
    # arrays on the caller's stack rather than in its state object.  8 KiB was
    # not enough for it.
    STACKSIZE 32768
    DEPENDS libopus)
endif()
