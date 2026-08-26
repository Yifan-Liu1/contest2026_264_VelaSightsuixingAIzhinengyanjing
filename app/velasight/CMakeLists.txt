if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  nuttx_add_application(
    NAME velasight
     SRCS velasight_main.c vs_app.c vs_display.c vs_input.c vs_config.c vs_network.c
          vs_voice.c vs_audio.c vs_media.c vs_history.c vs_settings.c
          velasight_font_16_ui.c
     INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/packages/ai_agent/include
                          ${NUTTX_APPS_DIR}/packages/ai_agent/src
                          ${NUTTX_APPS_DIR}/packages/demos/contest2026_264_provision_web/include
                          ${NUTTX_APPS_DIR}/netutils/cjson/cJSON
      DEPENDS lvgl provision_web_core
    STACKSIZE 8192)
endif()
