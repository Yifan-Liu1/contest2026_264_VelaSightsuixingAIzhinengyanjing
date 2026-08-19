# development-machine web console service.
#
# SRCS is spelled out, which is also what keeps host/ out of the firmware:
# console.py and the page live in this directory but are never compiled.
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_WEB_TOOL)
  # mbedTLS headers, the same way packages/ai_agent reaches them.  The library
  # itself is part of libapps, so only the include path is needed here.
  set(MBEDTLS_INC ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include)

  # app/conv's record store, for the conv.query and conv.get commands.
  #
  # Reached through REALPATH rather than a plain "../conv": both directories
  # arrive here as <linkfile> symlinks under packages/demos/ with names that do
  # not match their source directories, so "../conv" resolves to
  # packages/demos/conv, which does not exist.  Resolving this directory to its
  # real location first and then taking the sibling gives the same answer
  # whether CMake hands us the symlink or the real path.
  #
  # Compiled here rather than linked from the conv application on purpose: that
  # would make the page's history panel disappear whenever someone turns the
  # `conv` NSH command off, and the store is a few hundred bytes of code.
  get_filename_component(WT_REAL "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
  get_filename_component(WT_APPS_DIR "${WT_REAL}" DIRECTORY)
  set(CONV_DIR "${WT_APPS_DIR}/conv")

  if(NOT EXISTS "${CONV_DIR}/conv_store.c")
    # Fail here rather than at link time: a missing store shows up as an
    # undefined reference to conv_store_query in wt_command.c, which points at
    # the wrong file.
    message(FATAL_ERROR "web_tool: conv_store.c not found under ${CONV_DIR}")
  endif()

  nuttx_add_application(
    NAME web_tool
    SRCS web_tool_main.c wt_protocol.c wt_queue.c wt_command.c
         wt_selftest.c wt_io.c "${CONV_DIR}/conv_store.c"
    INCLUDE_DIRECTORIES ${MBEDTLS_INC} "${CONV_DIR}"
    STACKSIZE 8192
    PRIORITY 100)
endif()
