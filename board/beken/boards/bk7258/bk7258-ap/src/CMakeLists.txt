set(SRCS
    bk7258_boot.c
    bk7258_bringup.c
    bk7258_buttons.c
    bk7258_leds.c
    bk7258_motor.c
    bk7258_appinit.c
    bk7258_gc9d01.c
    bk7258_ramdisk.c
    bk7258_camera_imgsensor.c
    bk7258_camera_bringup.c)

if(CONFIG_BK7258_SDIO)
  list(APPEND SRCS bk7258_mmcsd.c)
endif()

if(CONFIG_BK7258_AUDIO)
  list(APPEND SRCS bk7258_audio_bringup.c bk7258_audio_dev.c)
endif()

if(CONFIG_BK7258_GC9D01_FB)
  list(APPEND SRCS bk7258_gc9d01_fb.c
    bk7258_status_screen.c
    hello_paint.c
    hello_font.c)
endif()

if(CONFIG_BK7258_JPEG_ENC)
  list(APPEND SRCS bk7258_jpeg_enc.c)

  # libjpeg-turbo publishes its headers with nuttx_export_header(), which lands
  # them under the apps binary dir.  This driver is kernel-side board code and
  # does not get that path by default, so add it explicitly rather than
  # reaching into external/ with a hand-written relative path.
  target_include_directories(board
                             PRIVATE ${NUTTX_APPS_BINDIR}/include/turbojpeg-static)
endif()

if(CONFIG_BK7258_BT_GATT_TEST)
  list(APPEND SRCS bk7258_bt_gatt_test.c)
endif()

target_sources(board PRIVATE ${SRCS})
target_include_directories(board PRIVATE
  ${NUTTX_BOARD_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}
  ${NUTTX_CHIP_ABS_DIR}/include
  ${NUTTX_CHIP_ABS_DIR}/hardware)
