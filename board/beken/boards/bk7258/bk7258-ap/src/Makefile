# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/Make.defs

CFLAGS += -I$(TOPDIR)/arch/arm/src/chip/include
CFLAGS += -I$(TOPDIR)/arch/arm/src/chip/hardware

CSRCS = bk7258_boot.c
CSRCS += bk7258_bringup.c
CSRCS += bk7258_buttons.c
CSRCS += bk7258_motor.c
CSRCS += bk7258_appinit.c
CSRCS += bk7258_leds.c
CSRCS += bk7258_gc9d01.c

ifeq ($(CONFIG_BK7258_GC9D01_FB),y)
CSRCS += bk7258_gc9d01_fb.c hello_paint.c hello_font.c
endif
CSRCS += bk7258_ramdisk.c
CSRCS += bk7258_camera_imgsensor.c
CSRCS += bk7258_camera_bringup.c

ifeq ($(CONFIG_BK7258_AUDIO),y)
CSRCS += bk7258_audio_bringup.c
CSRCS += bk7258_audio_dev.c
endif

ifeq ($(CONFIG_BK7258_JPEG_ENC),y)
CSRCS += bk7258_jpeg_enc.c
endif

ifeq ($(CONFIG_BK7258_BT_GATT_TEST),y)
CSRCS += bk7258_bt_gatt_test.c
endif

include $(TOPDIR)/boards/Board.mk
