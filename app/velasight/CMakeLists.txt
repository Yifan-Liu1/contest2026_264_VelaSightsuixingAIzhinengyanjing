if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  nuttx_add_application(
    NAME velasight
     SRCS velasight_main.c vs_app.c vs_display.c vs_input.c vs_config.c vs_network.c
          vs_voice.c vs_audio.c vs_media.c vs_history.c vs_settings.c
          vs_cloud.c vs_social.c
          # The Ogg Opus encoder the session's audio chunks go through.
          #
          # Compiled from audio_test rather than copied or rewritten.  It is
          # self-contained (opus.h, the PSRAM allocator and libc, nothing from
          # its own application), it is the encoder already exercised on this
          # board by "audio_test opus", and reimplementing Ogg page framing
          # would risk producing files the cloud cannot decode for no gain.
          # velasight and audio_test are separate applications, so the same
          # translation unit in both links cleanly.
          ${CMAKE_CURRENT_SOURCE_DIR}/../contest2026_264_audio_test/audio_test_ogg.c
          velasight_font_16_ui.c
     INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/packages/ai_agent/include
                          ${NUTTX_APPS_DIR}/packages/ai_agent/src
                          ${NUTTX_APPS_DIR}/packages/demos/contest2026_264_provision_web/include
                          ${NUTTX_APPS_DIR}/packages/demos/contest2026_264_audio_test
                          ${NUTTX_APPS_DIR}/netutils/cjson/cJSON
      DEPENDS lvgl provision_web_core libopus
    STACKSIZE 8192)
endif()
