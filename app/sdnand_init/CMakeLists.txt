# BK7258 manual SD-NAND initialization
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_SDNAND_INIT)
  nuttx_add_application(
    NAME sdnand_init
    SRCS sdnand_init_main.c
    STACKSIZE 4096)
endif()
