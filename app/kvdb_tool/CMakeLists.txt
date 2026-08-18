# Persistent configuration in the AP's own flash partition
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_KVDB_TOOL)
  nuttx_add_application(
    NAME kvdb
    SRCS kvdb_tool_main.c
    STACKSIZE 4096)
endif()
