# SPDX-License-Identifier: Apache-2.0

set(SRCS
    bk7258_start.c
    bk7258_vectors.c
    bk7258_irq.c
    bk7258_timerisr.c
    bk7258_gpio.c
    bk7258_ldo33.c
    bk7258_pwm.c
    bk7258_lowputc.c
    bk7258_syslog.c
    bk7258_serial.c
    bk7258_mbox0.c
    bk7258_mailbox_channel.c
    bk7258_ipc_heartbeat.c
    bk7258_wifi.c
    bk7258_mb_uart.c
    bk7258_pm_pwc.c
    bk7258_psram.c
    bk7258_allocateheap.c
    bk7258_qspi.c
    bk7258_dma.c
    bk7258_i2c_sim.c
    bk7258_gc2145_i2c_bitbang.c
    bk7258_yuv_buf.c
    bk7258_jpeg_enc.c
    bk7258_camera_imgdata.c
    bk7258_aud.c
    bk7258_cpuindex.c
    bk7258_idle.c
    bk7258_reset.c
    bk7258_flash_client.c
    bk7258_kvdb.c)

if(CONFIG_BK7258_SDIO)
  list(APPEND SRCS bk7258_sdio.c)
endif()

if(CONFIG_BK7258_TRNG)
  list(APPEND SRCS bk7258_trng.c)
endif()

if(CONFIG_BK7258_BLUETOOTH)
  list(APPEND SRCS bk7258_bt_transport.c bk7258_bt_driver.c)

  if(CONFIG_BK7258_BT_RAW_SELFTEST)
    list(APPEND SRCS bk7258_bt_selftest.c)
  endif()
endif()

add_library(bk7258_chip OBJECT ${SRCS})
add_dependencies(bk7258_chip nuttx_context)
nuttx_add_library_internal(bk7258_chip)

target_compile_options(
  bk7258_chip
  PRIVATE $<GENEX_EVAL:$<TARGET_PROPERTY:nuttx,NUTTX_KERNEL_COMPILE_OPTIONS>>
          -mcpu=cortex-m33+nodsp -mthumb -mfpu=fpv5-sp-d16
          -mfloat-abi=hard -mcmse)
target_compile_definitions(
  bk7258_chip
  PRIVATE $<GENEX_EVAL:$<TARGET_PROPERTY:nuttx,NUTTX_KERNEL_DEFINITIONS>>)
target_include_directories(
  bk7258_chip
  PRIVATE $<GENEX_EVAL:$<TARGET_PROPERTY:nuttx,NUTTX_KERNEL_INCLUDE_DIRECTORIES>>
          ${NUTTX_CHIP_ABS_DIR}
          ${NUTTX_CHIP_ABS_DIR}/include
          ${NUTTX_CHIP_ABS_DIR}/include
          ${NUTTX_CHIP_ABS_DIR}/hardware
          ${CMAKE_SOURCE_DIR}/sched)
target_link_libraries(bk7258_chip PRIVATE arch)

if(TARGET nuttx)
  add_dependencies(nuttx bk7258_chip)
  target_link_libraries(nuttx PRIVATE $<TARGET_OBJECTS:bk7258_chip>)
  target_link_options(nuttx PRIVATE "LINKER:--build-id=none"
                      "LINKER:--entry=__start")
endif()

set_property(TARGET nuttx_target_interface APPEND PROPERTY ALL_TARGETS
             bk7258_chip)
file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/targets "bk7258_chip\n")

execute_process(
  COMMAND sh ${NUTTX_CHIP_ABS_DIR}/check_swap_layout.sh
  RESULT_VARIABLE BK7258_SWAP_CHECK_RESULT)
if(NOT BK7258_SWAP_CHECK_RESULT EQUAL 0)
  message(FATAL_ERROR "BK7258 app_ab SWAP layout validation failed")
endif()
