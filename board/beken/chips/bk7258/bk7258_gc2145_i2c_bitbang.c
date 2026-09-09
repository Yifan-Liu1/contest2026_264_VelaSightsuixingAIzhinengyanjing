/****************************************************************************
 * board/beken/chips/bk7258/bk7258_gc2145_i2c_bitbang.c
 *
 * Bit-banged I2C1 for the GC2145 sensor.
 *
 * The hardware I2C1 block is claimed by the CP, so the AP drives the two pins
 * directly to reach the sensor's register map.  Timing is generated from
 * up_udelay() with the delays chosen against a scope rather than from the
 * datasheet's minimums, which the pin loading on this board does not meet.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <nuttx/clock.h>

#include "bk7258_gpio.h"

/* This driver's own API declarations.  It used to include
 * "bk7258_i2c1.h", which resolves to hardware/bk7258_i2c1.h -- the
 * register map of the unused hardware I2C1 block -- so the
 * implementation was never checked against its declarations.
 */

#include "bk7258_gc2145_i2c_bitbang.h"

/* GPIO42/GPIO43: plain GPIO bit-bang, NOT the I2C1_SCL/I2C1_SDA hardware
 * pinmux function -- see this file's header comment.  No
 * bk7258_gpio_set_function() call is made for either pin: leaving them
 * at their default/static function (whatever board bring-up left them
 * as, e.g. GPIO_DEV_LCD_G2/GPIO_DEV_LCD_B7 in the reference projects'
 * static tables) is fine because bk7258_gpio_output()/
 * bk7258_gpio_write() below unconditionally clear
 * BK7258_GPIO_SECOND_FUNCTION on every call, which is this repo's
 * equivalent of sim_i2c_driver.c's explicit gpio_dev_unmap() calls in
 * bk_i2c_init_v2(). */
#define BK7258_I2C1_SCL_PIN       42u
#define BK7258_I2C1_SDA_PIN       43u

/* Busy-wait spin count per SCL half-period.  sim_i2c_driver.c's
 * SCL_DELAY for 100kHz (CLK_DELAY_100K=25) is calibrated for that SDK's
 * own busy-wait loop body and CPU clock, which do not necessarily match
 * this NuttX build's optimization level or up_udelay() call overhead;
 * using up_udelay() with an explicit microsecond value instead of a
 * bare spin count is more portable across compilers/optimization
 * levels, at the cost of being slower than a tightly-tuned spin loop.
 * 5us per half-period is standard-mode I2C's *minimum* SCL high/low
 * time (100kHz nominal = 5us half-period exactly); GC2145 is not
 * timing-critical beyond standard-mode I2C compliance, so exact
 * matching to the reference driver's spin count is not required. */
#define BK7258_I2C1_HALF_PERIOD_US  5u

static void bk7258_i2c1_delay(void)
{
  up_udelay(BK7258_I2C1_HALF_PERIOD_US);
}

static void bk7258_i2c1_scl_high(void)
{
  bk7258_gpio_output(BK7258_I2C1_SCL_PIN, true);
}

static void bk7258_i2c1_scl_low(void)
{
  bk7258_gpio_output(BK7258_I2C1_SCL_PIN, false);
}

static void bk7258_i2c1_sda_high(void)
{
  bk7258_gpio_output(BK7258_I2C1_SDA_PIN, true);
}

static void bk7258_i2c1_sda_low(void)
{
  bk7258_gpio_output(BK7258_I2C1_SDA_PIN, false);
}

/* Release SDA to input (with the board's external 4.7K pull-up doing
 * the "release high" work, per this file's header comment on this
 * board's schematic-provided pull-ups) so this master can sample the
 * slave's ACK/NACK response during the 9th clock cycle -- matching
 * sim_i2c_driver.c's I2cSetSdaInput() before I2cReadSda(). */
static void bk7258_i2c1_sda_release(void)
{
  bk7258_gpio_input_pullup(BK7258_I2C1_SDA_PIN);
}

static bool bk7258_i2c1_sda_read(void)
{
  return bk7258_gpio_read(BK7258_I2C1_SDA_PIN);
}

/* I2C START condition: SDA high-to-low transition while SCL is high.
 * Matches sim_i2c_driver.c's I2cStart(). */
static void bk7258_i2c1_start(void)
{
  bk7258_i2c1_sda_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_sda_low();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();
}

/* I2C STOP condition: SDA low-to-high transition while SCL is high.
 * Matches sim_i2c_driver.c's I2cStop(). */
static void bk7258_i2c1_stop(void)
{
  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();

  bk7258_i2c1_sda_low();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_sda_high();
  bk7258_i2c1_delay();
}

/* Clock out one bit (MSB-first) and pulse SCL, matching
 * sim_i2c_driver.c's per-bit loop inside I2cSend(). */
static void bk7258_i2c1_write_bit(bool bit_value)
{
  if (bit_value)
    {
      bk7258_i2c1_sda_high();
    }
  else
    {
      bk7258_i2c1_sda_low();
    }

  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();
}

/* Send one byte, MSB-first, then release SDA and sample the slave's ACK
 * bit on the 9th clock.  Returns true if the slave pulled SDA low
 * (ACK), false on NACK (SDA stayed high) -- matching sim_i2c_driver.c's
 * I2cSend() return value convention. */
static bool bk7258_i2c1_send_byte(uint8_t data)
{
  int bit;
  bool acked;

  for (bit = 7; bit >= 0; bit--)
    {
      bk7258_i2c1_write_bit((data & (1u << bit)) != 0);
    }

  bk7258_i2c1_sda_release();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  acked = !bk7258_i2c1_sda_read();

  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();

  /* Drive SDA again (it was left in input/released state above) before
   * the next write_bit()/stop() call assumes output control -- matches
   * sim_i2c_driver.c's I2cSend() calling I2cSetSdaOutput() right after
   * sampling ACK, for the same reason (I2cSetSdaHigh/Low() alone do not
   * return the pin to output mode once it has been released to input
   * for the ACK sample). */
  bk7258_i2c1_sda_high();

  return acked;
}

void bk7258_i2c1_init(void)
{
  /* No clock-gate/pinmux/interrupt setup needed: this is plain GPIO
   * bit-banging, not the hardware I2C1 peripheral (see this file's
   * header comment).  Idle the bus (both lines high) so the first
   * bk7258_i2c1_write_reg() call's START condition begins from a clean
   * idle state, matching sim_i2c_driver.c's I2cInit() (SetSdaOutput()
   * + SetSclOutput(), which this repo's bk7258_gpio_output(pin, true)
   * calls below achieve directly by driving both pins high). */
  bk7258_i2c1_sda_high();
  bk7258_i2c1_scl_high();
}

/* Diagnostic: with the bus idle (both lines driven high by init()),
 * release SDA (switch to input+internal-pull-up, exactly as
 * bk7258_i2c1_sda_release() does mid-transaction) and check whether it
 * reads back high.  This isolates one specific failure mode from the
 * "every I2C write NACKs" symptom: if this reads back LOW here, with
 * no slave/clock activity at all, the ACK-sampling mechanism itself is
 * unreliable (either no functioning pull-up -- internal or the
 * board's external 4.7K R42 -- or SDA is stuck low/shorted), which
 * would make every real transaction's NACK meaningless as evidence
 * about the sensor, since the "false" return could come from this
 * pre-existing condition rather than the sensor actually declining to
 * ACK.  If this reads back HIGH reliably, that failure mode is ruled
 * out and a real transaction's NACK is more likely a genuine
 * electrical/protocol issue (sensor not powered/reset, wrong address,
 * bad timing, etc). */
bool bk7258_i2c1_sda_idle_diag(void)
{
  int i;
  bool all_high = true;
  clock_t t0;
  clock_t t1;
  clock_t elapsed_ticks;

  /* Diagnostic: measure the *actual* wall-clock cost of 2000 calls to
   * bk7258_i2c1_delay() (the same function every SCL/SDA transition in
   * this driver waits on) against clock_systime_ticks() (SysTick-
   * backed, 1 tick = 1ms per CONFIG_USEC_PER_TICK, independent of the
   * up_udelay() busy-loop calibration being measured here).  At the
   * nominal 5us-per-call target, 2000 calls should take ~10ms (10
   * ticks).  This directly answers whether up_udelay(5) -- a small
   * value that only ever enters arm_udelay.c's innermost
   * "while (microseconds > 0)" branch, 5 iterations of a
   * CONFIG_BOARD_LOOPSPERMSEC-derived 12-iteration empty loop -- holds
   * up at this magnitude the same way the previously-verified 120ms
   * power/reset delays did, or whether compiler optimization/call
   * overhead at this small a loop count makes the actual SCL bit-bang
   * frequency far higher (or lower) than intended, independent of any
   * GPIO/pin/protocol correctness already ruled out. */
  t0 = clock_systime_ticks();
  for (i = 0; i < 2000; i++)
    {
      bk7258_i2c1_delay();
    }

  t1 = clock_systime_ticks();
  elapsed_ticks = t1 - t0;
  printf("bk7258_i2c1: 2000x bk7258_i2c1_delay() calls measured %lu "
         "ticks (~%lu ms); nominal target at 5us/call is ~10ms -- if "
         "this is far below 10ms, up_udelay(5) is running much faster "
         "than intended (SCL bit-bang far exceeds I2C standard-mode "
         "timing); if far above, much slower than intended\n",
         (unsigned long)elapsed_ticks, (unsigned long)elapsed_ticks);

  bk7258_i2c1_sda_high();
  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_sda_release();
  bk7258_i2c1_delay();

  for (i = 0; i < 8; i++)
    {
      bool level = bk7258_i2c1_sda_read();
      printf("bk7258_i2c1: idle-bus SDA read-back #%d = %d\n",
             i, (int)level);
      if (!level)
        {
          all_high = false;
        }

      bk7258_i2c1_delay();
    }

  bk7258_i2c1_sda_high();

  return all_high;
}

/* Read one bit: SDA must already be released to input.  Samples while
 * SCL is high, mirroring bk7258_i2c1_send_byte()'s ACK sampling. */

static bool bk7258_i2c1_read_bit(void)
{
  bool bit_value;

  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bit_value = bk7258_i2c1_sda_read();

  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();

  return bit_value;
}

/* Receive one byte MSB-first, then drive the master's ACK (SDA low) or
 * NACK (SDA high) on the 9th clock.  A single-byte read must NACK, which
 * is how the slave is told to stop driving the bus before STOP.
 */

static uint8_t bk7258_i2c1_recv_byte(bool ack)
{
  uint8_t data = 0;
  int bit;

  bk7258_i2c1_sda_release();

  for (bit = 7; bit >= 0; bit--)
    {
      if (bk7258_i2c1_read_bit())
        {
          data |= (uint8_t)(1u << bit);
        }
    }

  /* Take SDA back to output for the ACK/NACK bit -- same reason
   * bk7258_i2c1_send_byte() re-drives SDA after sampling ACK.
   */

  if (ack)
    {
      bk7258_i2c1_sda_low();
    }
  else
    {
      bk7258_i2c1_sda_high();
    }

  bk7258_i2c1_delay();

  bk7258_i2c1_scl_high();
  bk7258_i2c1_delay();

  bk7258_i2c1_scl_low();
  bk7258_i2c1_delay();

  bk7258_i2c1_sda_high();

  return data;
}

bool bk7258_i2c1_write_reg(uint8_t i2c_addr, uint8_t reg, uint8_t value)
{
  bool acked;

  bk7258_i2c1_start();

  acked = bk7258_i2c1_send_byte((uint8_t)(i2c_addr << 1));
  if (!acked)
    {
      bk7258_i2c1_stop();
      printf("bk7258_i2c1: NACK on address byte 0x%02x "
             "(i2c_addr=0x%02x, reg=0x%02x, value=0x%02x)\n",
             (unsigned int)(i2c_addr << 1), i2c_addr, reg, value);
      return false;
    }

  acked = bk7258_i2c1_send_byte(reg);
  if (!acked)
    {
      bk7258_i2c1_stop();
      printf("bk7258_i2c1: NACK on register-address byte 0x%02x "
             "(i2c_addr=0x%02x, value=0x%02x)\n",
             reg, i2c_addr, value);
      return false;
    }

  acked = bk7258_i2c1_send_byte(value);
  if (!acked)
    {
      printf("bk7258_i2c1: NACK on data byte 0x%02x "
             "(i2c_addr=0x%02x, reg=0x%02x)\n",
             value, i2c_addr, reg);
    }

  bk7258_i2c1_stop();

  return acked;
}

bool bk7258_i2c1_read_reg(uint8_t i2c_addr, uint8_t reg,
                          FAR uint8_t *value)
{
  if (value == NULL)
    {
      return false;
    }

  /* Write phase: address + register pointer, no STOP. */

  bk7258_i2c1_start();

  if (!bk7258_i2c1_send_byte((uint8_t)(i2c_addr << 1)))
    {
      bk7258_i2c1_stop();
      printf("bk7258_i2c1: read NACK on write-address byte 0x%02x "
             "(reg=0x%02x)\n", (unsigned int)(i2c_addr << 1), reg);
      return false;
    }

  if (!bk7258_i2c1_send_byte(reg))
    {
      bk7258_i2c1_stop();
      printf("bk7258_i2c1: read NACK on register-address byte 0x%02x\n",
             reg);
      return false;
    }

  /* Repeated START, then the same device address with the R/W bit set. */

  bk7258_i2c1_start();

  if (!bk7258_i2c1_send_byte((uint8_t)((i2c_addr << 1) | 1u)))
    {
      bk7258_i2c1_stop();
      printf("bk7258_i2c1: read NACK on read-address byte 0x%02x "
             "(reg=0x%02x)\n", (unsigned int)((i2c_addr << 1) | 1u), reg);
      return false;
    }

  /* Single byte: the master must NACK it so the slave releases SDA. */

  *value = bk7258_i2c1_recv_byte(false);

  bk7258_i2c1_stop();

  return true;
}
