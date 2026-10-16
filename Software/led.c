/*
  "THE BEER-WARE LICENSE" :
  Uwe Liedtke and Joerg Wunsch wrote this file.  As long as you retain this notice you
  can do whatever you want with this stuff. If we meet some day, and you think
  this stuff is worth it, you can buy us a beer in return.
*/
/*
  led.c

  Ansteuerung der 19 LEDs im Multiplex-Verfahren.
  Timer 0 (Vorteiler 8, 5,86 kHz): der Ueberlauf schaltet die naechste LED ein,
  Compare A schaltet sie wieder aus. OCR0A bestimmt damit die Helligkeit.
  Ein kompletter Durchlauf aller 19 LEDs dauert 3,2 ms (310 Hz, flimmerfrei).
 */

#include "thermo.h"

#include <avr/interrupt.h>
#include <avr/pgmspace.h>

volatile uint8_t led_an[LED_ANZAHL + 1];  // Index 0 unbenutzt

/* Augenbilder 0..7 fuer das Wuerfelfeld, Bit 0 = LED 11 ... Bit 6 = LED 17
   LED 11 = Mitte, 12/13 = Mitte links/rechts, 14..17 = Ecken */
static const uint8_t augen[8] PROGMEM =
{
    0x00,                               // aus
    0x01,                               // 1: LED 11
    0x28,                               // 2: LED 14, 16
    0x29,                               // 3: LED 11, 14, 16
    0x78,                               // 4: LED 14, 15, 16, 17
    0x79,                               // 5: LED 11, 14, 15, 16, 17
    0x7E,                               // 6: LED 12 .. 17
    0x7F,                               // 7: LED 11 .. 17
};

void led_init(void)
{
    TCCR0A = 0;                         // Normal Mode
    OCR0A  = LED_HELLIGKEIT;
    TCCR0B = _BV(CS01);                 // Vorteiler 8
    TIMSK0 = _BV(TOIE0) | _BV(OCIE0A);
}

void led_alle(bool an)
{
    for (uint8_t i = 1; i <= LED_ANZAHL; i++)
        led_an[i] = an;
}

void led_band_balken(uint8_t anzahl)
{
    for (uint8_t i = 1; i <= LED_BAND; i++)
        led_an[i] = (i <= anzahl);
}

void led_band_punkt(uint8_t pos)
{
    for (uint8_t i = 1; i <= LED_BAND; i++)
        led_an[i] = (i == pos);
}

void led_feld(uint8_t maske)
{
    for (uint8_t i = 0; i < 7; i++, maske >>= 1)
        led_an[LED_WUERFEL + i] = maske & 1;
}

void led_wuerfel(uint8_t zahl)
{
    led_feld(zahl < sizeof augen ? pgm_read_byte(&augen[zahl]) : 0);
}

/******************************************************************************
INTERRUPT Timer 0 Ueberlauf: naechste LED einschalten
Zeile (Kathode): LED 1-5 PB2, 6-10 PB1, 11-15 PB0, 16-19 PD7
Spalte (Anode):  LED 1,6,11,16 PD6 ... LED 5,10,15 PD2
******************************************************************************/
ISR(TIMER0_OVF_vect)
{
    static uint8_t nr = LED_ANZAHL, zeile, spalte;

    if (++spalte == 5)
    {
        spalte = 0;
        zeile++;
    }
    if (++nr > LED_ANZAHL)
    {
        nr = 1;
        zeile = 0;
        spalte = 0;
    }

    if (led_an[nr])
    {
        if (zeile == 3)
            DDRD |= _BV(PD7);
        else
            DDRB |= (uint8_t)(_BV(PB2) >> zeile);
        PORTD |= (uint8_t)(_BV(PD6) >> spalte);
    }
}

/******************************************************************************
INTERRUPT Timer 0 Vergleich A: alle LEDs aus (Ende der Pulsweite)
******************************************************************************/
ISR(TIMER0_COMPA_vect)
{
    PORTD &= ~ANODEN;
    DDRB  &= ~KATHODEN_B;
    DDRD  &= ~_BV(PD7);
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
