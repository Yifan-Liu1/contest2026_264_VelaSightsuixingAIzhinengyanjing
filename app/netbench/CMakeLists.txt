# Network benchmark with inter-core mailbox instrumentation
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_NETBENCH)
  nuttx_add_application(
    NAME netbench
    SRCS netbench_main.c
    STACKSIZE 4096)
endif()
