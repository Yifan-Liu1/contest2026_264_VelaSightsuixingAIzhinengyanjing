if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_VELASIGHT)
  nuttx_add_application(
    NAME velasight
     SRCS velasight_main.c vs_app.c vs_display.c vs_input.c vs_config.c vs_network.c
          velasight_font_16_ui.c
     INCLUDE_DIRECTORIES ${NUTTX_APPS_DIR}/packages/ai_agent/include
                         ${NUTTX_APPS_DIR}/packages/ai_agent/src
     DEPENDS lvgl
    STACKSIZE 8192)
endif()
