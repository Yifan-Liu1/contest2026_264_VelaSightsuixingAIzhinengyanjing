# VelaSight conversation history on the SD-NAND
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_CONV)
  nuttx_add_application(
    NAME conv
    SRCS conv_main.c conv_store.c conv_net.c conv_ws.c conv_serve.c
    # The seeded transcripts and the summaries are held as string literals and
    # a query keeps one index entry plus a scan window on the stack; 8 KiB is
    # the same figure the other demo applications use and leaves room for the
    # snprintf of a request header.
    STACKSIZE 8192)
endif()
