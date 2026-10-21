/*
 * HEADER FILE Temperaturanzeige und Wuerfel
 * Projekt fuer Bausatz DARC S09 > Wuerfel und Temperaturanzeige
 * Created: 01.11.2019 18:00:00
 *  Author: Uwe Liedtke und Joerg Wunsch
 * Ueberarbeitet: 2026 (Ganzzahlarithmetik, Tick-Steuerung, RS485-Spiel)
 */

#ifndef THERMO_H
#define THERMO_H

#ifndef F_CPU
#   define F_CPU        12000000UL     // Quarz 12 MHz
#endif

#ifndef UART_DEBUG                     // Kann auf Kommandozeile mit -DUART_DEBUG=1
#   define UART_DEBUG   0              // ueberschrieben werden (Debug-Befehle ueber RS485)
#endif

#include <inttypes.h>
#include <stdbool.h>
#include <avr/io.h>

#define VERSION         "1.0"

// Constants definitions
#define NODE_ID         2              // NODE ID vom Board
#define ADR485          13             // Adresse fuer Debug-Befehle
#define TICK_MS         10             // Zeitbasis der Hauptschleife (Timer 2)

/* Betriebsarten, Auswahl per Jumper am ISP-Stecker beim Einschalten
   (Pin 1 = MISO, 2 = VCC, 3 = SCK, 4 = MOSI, 5 = RESET, 6 = GND) */
#define BA_TEMPERATUR   0              // kein Jumper
#define BA_WUERFEL_6    1              // Jumper 4-6 (MOSI - GND)
#define BA_WUERFEL_7    2              // Jumper 3-4 (SCK - MOSI)
#define BA_ZWEISPIELER  3              // Jumper 1-3 (MISO - SCK)
#define BA_RS485_SPIEL  4              // Jumper 4-6 und 1-3 gleichzeitig

/* LED-Belegung
   LED  1..10  LED Band (Temperatur / Punktestand)
   LED 11..17  Wuerfelfeld
   LED 18, 19  blaue LEDs Spieler 1 / Spieler 2 */
#define LED_ANZAHL      19
#define LED_BAND        10             // Anzahl LEDs im Band (LED 1..10)
#define LED_WUERFEL     11             // erste LED im Wuerfelfeld
#define LED_SPIELER1    18
#define LED_SPIELER2    19
#define LED_HELLIGKEIT  85             // Pulsweite je LED 0..255 (85 = bisherige Helligkeit)

// Temperaturanzeige auf dem LED Band
#define TEMP_UNTEN      12             // LED 1 bis einschliesslich dieser Temperatur (Grad C)
#define TEMP_SCHRITT    2              // Grad C je LED

// Spiel
#define ZIEL_PUNKTE     20             // "20 gewinnt"

// DS18B20
#define OW_POWER_PIN    3              // one-wire IC powerup
#define OW_PORT         C              // one-wire port DS18B20
#define OW_PIN          2
#define DS18B20         0x28           // Family codes DS18xxx
#define DS1820          0x10

// Makros
#define sbi(TEST, PORT) PORT |= (1 << TEST)
#define cbi(TEST, PORT) PORT &= ~(1 << TEST)
#define qbi(TEST, PORT) ( (PORT & (0x01 << TEST))>>TEST )

// RS485: PC5 schaltet den MAX481 zwischen Senden (HI) und Empfang (LOW) um
#define SENDEN_AKTIV        sbi(5, PORTC)
#define SENDEN_INAKTIV      cbi(5, PORTC)

/* Definition LED Matrix
PD2 = Anode LED  5,10,15
PD3 = Anode LED  4,9,14,19
PD4 = Anode LED  3,8,13,18
PD5 = Anode LED  2,7,12,17
PD6 = Anode LED  1,6,11,16
PD7 = Kathode LED 16,17,18,19

PB0 = Kathode LED 11,12,13,14,15
PB1 = Kathode LED  6, 7, 8, 9,10
PB2 = Kathode LED  1, 2, 3, 4, 5
*/
#define ANODEN          (_BV(PD2) | _BV(PD3) | _BV(PD4) | _BV(PD5) | _BV(PD6))
#define KATHODEN_B      (_BV(PB0) | _BV(PB1) | _BV(PB2))

#define JMP_PORT        PORTB          // Jumper PORT PULLUP
#define JMP_DDR         DDRB           // Datenrichtungsregister
#define JMP_PIN         PINB           // PIN B
#define JMP_MOSI        3
#define JMP_MISO        4
#define JMP_SCK         5
#define SW_PORT         PINC           // PORT Wuerfelstart
#define SW_WUERFEL      4              // TASTER Wuerfel (PCINT12)

// EEPROM layout
                                       // Byte 0: frei lassen (kann leicht zerstoert werden)
#define EE_TEMP_KALIB ((uint16_t *)1)  // Byte 1/2: Kalibrierwert Offset Temperatursensor

// Blinktakte, Bitmaske auf den Tick-Zaehler
#define BLINK_SCHNELL   0x10           // ca. 3 Hz
#define BLINK_NORMAL    0x20           // ca. 1,5 Hz
#define BLINK_LANGSAM   0x40           // ca. 0,8 Hz

// Globale Variable
extern uint8_t betriebsart;                 // aktuelle Betriebsart (BA_...)
extern volatile uint16_t ticks;             // Tick-Zaehler, +1 alle TICK_MS
extern volatile uint8_t led_an[LED_ANZAHL + 1]; // LED 1..19 an (1) / aus (0)

static inline bool blink(uint8_t takt)
{
    return ((uint8_t)ticks & takt) != 0;
}

// led.c
void led_init(void);                      // Timer 0: Multiplexer
void led_alle(bool an);                   // alle LEDs an/aus
void led_band_balken(uint8_t anzahl);     // LED 1..anzahl im Band an, Rest aus
void led_band_punkt(uint8_t pos);         // nur LED pos (1..10) im Band an
void led_wuerfel(uint8_t zahl);           // Augenzahl 0..7 auf dem Wuerfelfeld
void led_feld(uint8_t maske);             // Wuerfelfeld direkt, Bit 0 = LED 11

// spiel.c
uint8_t zufallszahl(uint8_t max);         // Zufallszahl 1..max
void wuerfel_tick(void);                  // Wuerfelanimation, jeden Tick aufrufen
void einzel_taste(uint8_t max);           // Einzelspieler: wuerfeln 1..max
void zweispieler_init(void);
void zweispieler_taste(void);
void zweispieler_tick(void);
void netz_init(void);                     // RS485-Spiel mit zwei Wuerfeln
void netz_taste(void);
void netz_tick(void);
void netz_empfang(char typ, const uint8_t *daten, uint8_t laenge);

// rs485.c
void rs485_init(void);
void rs485_putc(char c);
void rs485_puts_P(const char *s);         // String aus dem Flash (PSTR)
void rs485_zahl(uint16_t zahl);           // Dezimalzahl
void rs485_hex(uint8_t wert);             // zwei Hex-Ziffern
void rs485_rahmen_senden(char typ, const uint8_t *daten, uint8_t laenge);
void rs485_empfang(void);                 // empfangene Zeichen auswerten (Hauptschleife)
#if UART_DEBUG
void debug_befehl(const uint8_t *cw);     // Debug-Befehl: Adresse, Kommando, Wert, CRC
#endif

// 1wire.c
void ow_power(bool);                      // One-wire sensor power on/off
bool ow_reset(void);                      // One-wire sensor reset => present?
bool Get_ROMCode(uint8_t *);              // Read ROM Code into buffer provided
void Configure_DS18B20(void);             // Configure DS18B20 for 9-bit operation
bool ow_messung_starten(void);            // Wandlung starten (Strong Pullup an)
bool ow_messung_lesen(uint8_t familie, int16_t *t16); // Ergebnis in 1/16 Grad C

#endif // THERMO_H

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
