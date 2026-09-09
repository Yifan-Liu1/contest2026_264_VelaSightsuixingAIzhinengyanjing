/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_i2c1.h
 *
 * Software bit-banged (GPIO simulated) I2C master-write driver for the
 * GC2145 camera sensor, on GPIO42 (SCL) / GPIO43 (SDA).
 *
 * CORRECTION (see bk7258_i2c1.c's file header for the full account):
 * this file previously documented and implemented a hardware I2C1
 * peripheral driver for these pins, based on the assumption that this
 * board's GC2145 camera I2C bus is the BK7258's hardware I2C1 block
 * (GPIO42/43 do support an I2C1_SCL/I2C1_SDA pinmux function). On real
 * hardware, every write timed out waiting for the I2C1 peripheral's
 * interrupt, despite its registers reading back exactly as configured.
 *
 * Cross-checking bk_solution_ai's beken_genie and volc_rtc projects
 * (both targeting this exact AIDK board, unlike the generic dvp_example
 * reference project) resolved this: GPIO42/43 are correct, but they are
 * meant to be driven as plain bit-banged GPIO (matching
 * bk_avdk_smp/ap/middleware/driver/i2c/sim_i2c_driver.c's software I2C,
 * which CONFIG_DVP_CAMERA_I2C_ID=2 always resolves to for GC2145 across
 * every bk_avdk_smp project that uses it), not the hardware I2C1
 * register block. Those same two board-specific projects' static GPIO
 * tables leave GPIO42/43 at an unrelated default function
 * (GPIO_DEV_LCD_G2/GPIO_DEV_LCD_B7), confirming the I2C1 hardware
 * pinmux function was never meant to be selected for this bus at all.
 * CORRECTION (re-verified directly against
 * AIDK_AI玩具开发板_原理图.pdf sheet 2/6's chip pinout fan-out): this
 * header previously cited the schematic net labels at pins 68/67
 * (P42/P43) as "IIC1_SCL"/"IIC1_SDA"; the schematic actually shows
 * "IIC2_SCL"/"IIC2_SDA" at those two pins (fan-out order at pins
 * 70/69/68/67 is LED1/LED2/IIC2_SCL/IIC2_SDA), and sheet 4/6's 24-pin
 * DVP connector (H1) confirms its SDA/SCL pins (3/5) are wired to nets
 * "IIC2_SDA"/"IIC2_SCL" -- i.e. GPIO42/43 are this board's IIC2 bus,
 * not IIC1. This does not change which GPIOs this driver uses
 * (GPIO42/43 remain correct); it only corrects the net-name citation.
 * The distinct "IIC1_SCL"/"IIC1_SDA" labels seen elsewhere on sheet
 * 2/6 (pins 85/84) feed the G-Sensor (SC7A20HTR, U6) over its own
 * bit-banged bus via VDDGPIO, unrelated to both the camera and to the
 * SoC's hardware I2C1 peripheral. The gyroscope is separately
 * confirmed wired to hardware I2C0 (GPIO20/21), matching
 * bk_avdk_smp's own gsensor_sc7a20.c reference driver.
 *
 * TIMING CAVEAT: this driver's bit-bang half-period
 * (BK7258_I2C1_HALF_PERIOD_US in bk7258_i2c1.c) depends on
 * up_udelay(), which in turn depends on this board's
 * CONFIG_BOARD_LOOPSPERMSEC being calibrated for its actual 120MHz
 * Cortex-M33 clock. That option was previously unset (silently using
 * NuttX's generic, far-too-low default of 5000 loops/ms), which would
 * have made every up_udelay() call -- including this bus's SCL
 * half-period and bk7258_camera_imgsensor.c's power/reset settle
 * delays -- run for much less real time than requested, independent
 * of any GPIO/pin/protocol correctness. See this board's defconfig
 * (CONFIG_BOARD_LOOPSPERMSEC=12000, an unverified estimate pending
 * on-hardware scope/logic-analyzer calibration) for the fix.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/* Idles the bus (SDA and SCL both driven high) so the first
 * bk7258_i2c1_write_reg() call's START condition begins from a clean
 * state.  No pinmux/clock-gate/interrupt setup is needed or performed
 * here -- this is plain GPIO bit-banging, not a hardware peripheral.
 * Must be called once before any bk7258_i2c1_write_reg() call. */
void bk7258_i2c1_init(void);

/* Writes a single 8-bit value to an 8-bit register address on the 7-bit
 * I2C device address i2c_addr, via a software bit-banged I2C
 * START/address/register-address/data/STOP sequence on GPIO42 (SCL) /
 * GPIO43 (SDA).  Returns true if every byte (device address, register
 * address, data) was ACKed by the slave; false on the first NACK
 * encountered (mirroring this function's previous hardware-I2C1-backed
 * return convention, so callers such as
 * bk7258_camera_imgsensor.c's register-table writer do not need to
 * change). */
bool bk7258_i2c1_write_reg(uint8_t i2c_addr, uint8_t reg, uint8_t value);

/* Diagnostic: with the bus idle, release SDA (input + internal
 * pull-up, the same mechanism used mid-transaction to sample ACK) and
 * log 8 consecutive read-backs with no clock/slave activity at all.
 * Returns true only if every read-back was high.  Use this BEFORE
 * trusting any real transaction's NACK as evidence about the sensor:
 * if this returns false, the ACK-sampling mechanism itself cannot
 * reliably read a high level (no working pull-up -- internal or the
 * board's external R42 -- or SDA stuck low/shorted), which would make
 * every subsequent "NACK" meaningless as sensor-behavior evidence. */
/* Reads a single 8-bit register from the 7-bit I2C device address
 * i2c_addr, using the standard write-pointer / repeated-START / read
 * sequence, and NACKing the single data byte so the slave releases SDA
 * before STOP.  Returns true and stores the byte in *value on success;
 * false if any address/register byte was NACKed.  Needed for sensor
 * identity checks (GC2145 reports 0x21/0x45 in registers 0xF0/0xF1),
 * which a write-only driver cannot perform. */
bool bk7258_i2c1_read_reg(uint8_t i2c_addr, uint8_t reg,
                          FAR uint8_t *value);

bool bk7258_i2c1_sda_idle_diag(void);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H */
