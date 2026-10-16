/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <joerg@FreeBSD.ORG> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.        Joerg Wunsch
 * ----------------------------------------------------------------------------
 */

/* $Id: 1wire.c 99 2019-12-02 23:21:02Z dl8dtl $ */

#include <stdint.h>
#include <stdbool.h>

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>

#include "thermo.h"

#include <util/crc16.h>
#include <util/delay.h>

#define concat(a, b) a##b
#define indir(a, b) concat(a, b)
#define ow_port indir(PORT, OW_PORT)
#define ow_pin  indir(PIN, OW_PORT)
#define ow_ddr  indir(DDR, OW_PORT)

static inline void
clear_dq(void)
{
  ow_port &= ~(1 << OW_PIN);
  ow_ddr  |= (1 << OW_PIN);
}

static inline void
set_dq(void)
{
  ow_ddr  &= ~(1 << OW_PIN);
  ow_port |= (1 << OW_PIN);
}

static inline void
strong_pullup(bool on)
{
  if (on)
  {
    ow_port |= (1 << OW_PIN);
    ow_ddr |= (1 << OW_PIN);
  }
  else
  {
    ow_port &= ~(1 << OW_PIN);
    ow_ddr &= ~(1 << OW_PIN);
  }
}

static inline uint8_t
get_dq(void)
{
  return (ow_pin & (1 << OW_PIN)) == 0;
}

void ow_power(bool on)
{
  ow_ddr |= (1 << OW_POWER_PIN);
  if (on)
    ow_port |= (1 << OW_POWER_PIN);
  else
    ow_port &= ~(1 << OW_POWER_PIN);
}

bool
ow_reset(void)
{
  bool presence;
  uint8_t saved_sreg = SREG;

  cli();
  clear_dq();
  _delay_ms(0.48);
  set_dq();
  _delay_ms(0.07);
  presence = get_dq();
  SREG = saved_sreg;
  _delay_ms(0.24);

  return presence;
}

static uint8_t
ow_read_bit(void)
{
  bool res;
  uint8_t saved_sreg = SREG;

  cli();
  clear_dq();
  set_dq();
  _delay_us(15);

  res = get_dq();
  SREG = saved_sreg;
  _delay_us(60);

  return res? 0: 1;
}

static void
ow_write_bit(bool bit)
{
  uint8_t saved_sreg = SREG;

  cli();
  clear_dq();
  _delay_us(1);
  if (bit)
    set_dq();
  _delay_ms(0.1);
  set_dq();
  _delay_us(1);
  SREG = saved_sreg;
}

static uint8_t
ow_read_byte(void)
{
  uint8_t i;
  uint8_t val = 0, mask;

  for (i = 0, mask = 1; i < 8; i++, mask <<= 1)
    {
      if (ow_read_bit())
        val |= mask;
      _delay_ms(0.1);
    }

  return val;
}

static void
ow_write_byte(uint8_t val)
{
  uint8_t i;
  uint8_t mask;

  for (i = 0, mask = 1; i < 8; i++, mask <<= 1)
    {
      ow_write_bit(val & mask);
    }
}

static uint8_t
ow_checkcrc(uint8_t *data, uint8_t nbytes)
{
  uint8_t crc = 0, i;

  for (i = 0; i < nbytes; i++)
    crc = _crc_ibutton_update(crc, data[i]);

  return crc; // must be 0
}

/*
 * Temperaturmessung ohne Warteschleife:
 * ow_messung_starten() startet die Wandlung und schaltet den Strong Pullup ein,
 * ow_messung_lesen() wird nach der Wandlungszeit aufgerufen (DS18B20 9 Bit:
 * 94 ms, DS1820: 750 ms) und liefert die Temperatur in 1/16 Grad C.
 */
bool
ow_messung_starten(void)
{
  if (!ow_reset())
    return false;

  ow_write_byte(0xCC); // Skip ROM
  cli();
  ow_write_byte(0x44); // Start Conversion

  // strong pullup must be enabled within 10 µs after "Start
  // Conversion" => F_CPU = 8 MHz recommendable
  strong_pullup(true);
  sei();

  return true;
}

bool
ow_messung_lesen(uint8_t familie, int16_t *t16)
{
  uint8_t get[9];
  uint8_t k;

  strong_pullup(false);
  if (!ow_reset())
    return false;

  ow_write_byte(0xCC); // Skip ROM
  ow_write_byte(0xBE); // Read Scratch Pad

  for (k = 0; k < 9; k++){
    get[k] = ow_read_byte();
  }
  // Byte 7 ist bei DS18B20 und DS1820 immer 0x10; faengt eine dauerhaft
  // auf Low liegende Leitung ab (lauter Nullen haetten ebenfalls CRC 0)
  if (ow_checkcrc(get, 9) != 0 || get[7] != 0x10)
    return false;

  int16_t roh = (int16_t)((get[1] << 8) | get[0]);
  if (familie == DS18B20)
    roh &= ~7;   // 9 Bit Aufloesung: untere Bits undefiniert
  else
    roh *= 8;    // DS1820: 1/2 Grad -> 1/16 Grad
  *t16 = roh;

  return true;
}

bool
Get_ROMCode(uint8_t *dat)
{
  uint8_t n;

  ow_reset();

  ow_write_byte(0x33);

#if UART_DEBUG
  rs485_puts_P(PSTR("ROM Code: "));
#endif
  for (n = 0; n < 8; n++) {
    dat[n] = ow_read_byte();
#if UART_DEBUG
    rs485_hex(dat[n]);
    rs485_putc(' ');
#endif
  }
  n = ow_checkcrc(dat, 8);
#if UART_DEBUG
  rs485_puts_P(n ? PSTR("CRC fail\r\n") : PSTR("CRC OK\r\n"));
#endif
  return n == 0;
}

void
Configure_DS18B20(void)
{
  uint8_t get[9];
  uint8_t k;

  ow_reset();

  ow_write_byte(0xCC); // Skip ROM
  ow_write_byte(0xBE); // Read Scratch Pad

  for (k = 0; k < 9; k++) {
    get[k] = ow_read_byte();
  }
  k = ow_checkcrc(get, 9);
  if (k != 0)
    return;

  get[4] = 0x1F; // configuration register, R1 = R0 = 0, 9-bit operation

  ow_write_byte(0x4E); // Write Scratch Pad
  for (k = 2; k < 5; k++) {
    ow_write_byte(get[k]);
  }

  ow_reset();
}
