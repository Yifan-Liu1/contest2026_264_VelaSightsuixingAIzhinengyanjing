# development-machine web console service.
#
# SRCS is spelled out, which is also what keeps host/ out of the firmware:
# console.py and the page live in this directory but are never compiled.
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL)
  # mbedTLS headers, the same way packages/ai_agent reaches them.  The library
  # itself is part of libapps, so only the include path is needed here.
  set(MBEDTLS_INC ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include)

  nuttx_add_application(
    NAME web_tool
    SRCS web_tool_main.c wt_protocol.c wt_queue.c wt_command.c
         wt_selftest.c wt_io.c
    INCLUDE_DIRECTORIES ${MBEDTLS_INC}
    STACKSIZE 8192
    PRIORITY 100)
endif()
