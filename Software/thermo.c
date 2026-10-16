/*
  "THE BEER-WARE LICENSE" :
  Uwe Liedtke and Joerg Wunsch wrote this file.  As long as you retain this notice you
  can do whatever you want with this stuff. If we meet some day, and you think
  this stuff is worth it, you can buy us a beer in return.
*/
/*
  Thermometer.c

  Created: 01.11.2019 14:00:00
  Author:  Uwe Liedtke und Joerg Wunsch
  Projekt: Temperaturanzeige und Wuerfel
  Funktionen (Auswahl per Jumper beim Einschalten, siehe thermo.h):
  1. Temperaturanzeige (DS18B20/DS1820 an JP2 oder interner Sensor), Ausgabe auch ueber RS485
  2. Wuerfel mit 6 oder 7 Augen
  3. Zweispielermode "20 gewinnt" mit einem Wuerfel
  4. Zweispielermode "20 gewinnt" mit zwei Wuerfeln ueber RS485

  fuer Atmega 48A/88A/168A, 12 MHz

  Zeitablauf:
  Timer 0  LED-Multiplexer (led.c)
  Timer 1  frei laufend mit 12 MHz, Zufallsquelle fuer den Wuerfel
  Timer 2  Tick alle 10 ms, steuert die Hauptschleife
 */

#include "thermo.h"

#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <util/delay.h>

#define MESS_TAKT       500            // Temperaturmessung alle 5 s (Ticks)
#define MESS_TAKT_KALIB 50             // im Kalibriermodus alle 0,5 s

#if F_CPU < 10000000
#   define ADC_VORTEILER (_BV(ADPS2) | _BV(ADPS0))  // 32 => 115 kHz bei 3,68 MHz
#else
#   define ADC_VORTEILER (_BV(ADPS2) | _BV(ADPS1))  // 64 => 187 kHz bei 12 MHz
#endif

uint8_t betriebsart;
volatile uint16_t ticks;
static volatile bool tick_neu;

// Temperaturanzeige
static bool     sensor_extern;          // DS18B20 / DS1820 an JP2 gefunden
static bool     kalibrierung;           // Kalibriermodus interner Sensor (Poti an JP2)
static uint8_t  romcode[8];             // ROM code of 1wire device
static uint16_t temp_offset = 290;      // Temperatur-Offset, Kalibrierwert
static uint16_t mess_warte;             // Ticks bis zur naechsten Messung
static uint8_t  ow_warte;               // Ticks bis die 1-Wire-Wandlung fertig ist

#if UART_DEBUG
static bool taste_simuliert;
#endif

/***************************************************************************************
INIT der Ein und Ausgabeports
***************************************************************************************/
static void ports_init(void)
{
/* PORTB
PB0..PB2 = Kathoden LED-Zeilen, werden vom Multiplexer ueber DDRB geschaltet
PB3..PB5 = MOSI/MISO/SCK, Jumper Konfig (Pullups nur waehrend der Abfrage)
PB6, PB7 = Quarz */
    DDRB  = 0;
    PORTB = 0;

/* PORTC
PC0, PC1 = USB D-/D+
PC2 = 1-Wire DATA (4,7k nach PC3) / ADC2 Poti fuer Kalibrierung
PC3 = 1-Wire Power
PC4 = Taster Start Wuerfel, Eingang mit internem Pullup
PC5 = RX/TX Umschaltung fuer RS485 HI=TX LOW=RX */
    DDRC  = _BV(PC5);
    PORTC = _BV(PC4);

/* PORTD
PD0, PD1 = RXD/TXD (485), Pullup an RXD solange der Empfaenger aus ist
PD2..PD6 = Anoden LED-Spalten, PD7 = Kathode LED 16..19 (ueber DDRD) */
    DDRD  = ANODEN;
    PORTD = _BV(PD0);
}

/******************************************************************************
TIMER INIT
Timer 1: frei laufend ohne Vorteiler (Zufallsquelle)
Timer 2: CTC, Vorteiler 1024, Tick alle 10 ms
Taster:  Pin-Change-Interrupt (Zufallsquelle, siehe spiel.c)
******************************************************************************/
static void timer_init(void)
{
    TCCR1B = _BV(CS10);

    TCCR2A = _BV(WGM21);
    OCR2A  = (F_CPU / 1024 * TICK_MS + 500) / 1000 - 1;
    TCCR2B = _BV(CS22) | _BV(CS21) | _BV(CS20);
    TIMSK2 = _BV(OCIE2A);

    PCMSK1 = _BV(PCINT12);
    PCICR  = _BV(PCIE1);
}

/***************************************************************************************
Abfrage der Jumper am ISP-Stecker
  kein Jumper            Temperaturanzeige
  4-6 (MOSI - GND)       Wuerfel 1 - 6
  3-4 (SCK  - MOSI)      Wuerfel 1 - 7
  1-3 (MISO - SCK)       Zweispieler mit einem Wuerfel
  4-6 und 1-3            Zweispieler mit zwei Wuerfeln ueber RS485
***************************************************************************************/
static uint8_t jumper_lesen(void)
{
    JMP_PORT |= _BV(JMP_MOSI) | _BV(JMP_MISO);      // Pullups an
    _delay_us(10);
    bool mosi_gnd = !(JMP_PIN & _BV(JMP_MOSI));

    JMP_DDR |= _BV(JMP_SCK);                        // SCK aktiv low
    _delay_us(10);
    bool sck_miso = !(JMP_PIN & _BV(JMP_MISO));
    bool sck_mosi = !mosi_gnd && !(JMP_PIN & _BV(JMP_MOSI));

    JMP_DDR  &= ~_BV(JMP_SCK);
    JMP_PORT &= ~(_BV(JMP_MOSI) | _BV(JMP_MISO));

    if (mosi_gnd)
        return sck_miso ? BA_RS485_SPIEL : BA_WUERFEL_6;
    if (sck_mosi)
        return BA_WUERFEL_7;
    if (sck_miso)
        return BA_ZWEISPIELER;
    return BA_TEMPERATUR;
}

/******************************************************************************
Taster entprellen (Aufruf jeden Tick): true beim Druecken
******************************************************************************/
static bool taste_abfragen(void)
{
    static uint8_t verlauf;
    static bool gedrueckt;

    verlauf = (verlauf << 1) | !qbi(SW_WUERFEL, SW_PORT);
    if ((verlauf & 0x07) == 0x07)
    {
        if (!gedrueckt)
        {
            gedrueckt = true;
            return true;
        }
    }
    else if ((verlauf & 0x07) == 0)
    {
        gedrueckt = false;
    }
#if UART_DEBUG
    if (taste_simuliert)
    {
        taste_simuliert = false;
        return true;
    }
#endif
    return false;
}

/******************************************************************************
AD-Wandler
Normal: interne Referenz 1,1 V, Kanal 8 (interner Temperatursensor)
Kalibrierung: kurz auf AVcc und ADC2 (Poti an JP2) umschalten
******************************************************************************/
static void adc_temp_init(void)
{
    ADMUX  = _BV(REFS1) | _BV(REFS0) | _BV(MUX3);
    ADCSRA = _BV(ADEN) | ADC_VORTEILER;
}

static uint16_t adc_wandeln(void)
{
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC))
        ;
    return ADC;
}

static uint8_t adc2_lesen(void)
{
    ADMUX  = _BV(REFS0) | _BV(ADLAR) | 2;   // AVcc, ADC2, nur 8 Bit
    ADCSRA = _BV(ADEN) | ADC_VORTEILER;
    adc_wandeln();                          // erste Wandlung verwerfen
    uint8_t wert = adc_wandeln() >> 8;
    adc_temp_init();
    return wert;
}

/******************************************************************************
Interner Sensor, Mittelwert aus 8 Wandlungen, Ergebnis in 1/16 Grad C
Formel wie bisher: t = adc * 1100/1024 - temp_offset * 1100/1024,
damit bleiben vorhandene Kalibrierwerte im EEPROM gueltig.
******************************************************************************/
static int16_t temp_intern(void)
{
    uint16_t summe = 0;

    adc_wandeln();                          // erste Wandlung verwerfen
    for (uint8_t i = 0; i < 8; i++)
        summe += adc_wandeln();

    int16_t offset = (uint32_t)temp_offset * 1100 / 1024;
    // summe/8 * 1100/1024 * 16 = summe * 275 / 128
    return (int16_t)((uint32_t)summe * 275 / 128) - offset * 16;
}

/******************************************************************************
Temperatur auf dem LED Band (eine LED je TEMP_SCHRITT Grad) und ueber RS485
Ausgabe: "T=23.5 ext" bzw. "T=23.5 int"
******************************************************************************/
static void temperatur_anzeigen(int16_t t16)
{
    // auf den Messbereich der Sensoren begrenzen (-55 .. 125 Grad C)
    if (t16 < -55 * 16)
        t16 = -55 * 16;
    if (t16 > 125 * 16)
        t16 = 125 * 16;

    int16_t pos = (t16 - TEMP_UNTEN * 16) / (TEMP_SCHRITT * 16) + 1;

    if (pos < 1)
        pos = 1;
    if (pos > LED_BAND)
        pos = LED_BAND;
    led_band_punkt(pos);

    int16_t z = (t16 * 10 + (t16 < 0 ? -8 : 8)) / 16;   // Zehntel Grad, gerundet
    rs485_puts_P(PSTR("T="));
    if (z < 0)
    {
        rs485_putc('-');
        z = -z;
    }
    rs485_zahl(z / 10);
    rs485_putc('.');
    rs485_putc('0' + z % 10);
    rs485_puts_P(sensor_extern ? PSTR(" ext\r\n") : PSTR(" int\r\n"));
}

static void temperatur_fehler(void)
{
    led_band_balken(0);
    led_an[1] = led_an[LED_BAND] = 1;       // LED 1 und 10: Sensorfehler
    rs485_puts_P(PSTR("T=Fehler\r\n"));
}

static void temperatur_init(void)
{
    uint16_t tmp = eeprom_read_word(EE_TEMP_KALIB);
    if (tmp != 0xFFFF &&                    // geloeschter EEPROM
        tmp > 200 && tmp < 350)             // Plausibilitaet
        temp_offset = tmp;

    // Abfrage, ob ein Potenziometer an ADC2 (JP2) steckt. Poti muss
    // Mittelstellung sein, damit das funktioniert. Befindet es sich
    // zu weit Richtung GND, wird der Bootloader aktiviert.  Befindet
    // es sich nahe am Vcc-seitigen Anschlag, ist es nicht von einem
    // unbeschalteten JP2 (Pullup nach PC3) unterscheidbar.
    if (adc2_lesen() < 240)
    {
        kalibrierung = true;
        rs485_puts_P(PSTR("Kalibriermodus aktiv\r\n"));
    }
    else if (ow_reset() && Get_ROMCode(romcode) &&
             (romcode[0] == DS18B20 || romcode[0] == DS1820))
    {
        sensor_extern = true;
        if (romcode[0] == DS18B20)
            Configure_DS18B20();
    }
    rs485_puts_P(sensor_extern ?
                 (romcode[0] == DS18B20 ? PSTR("DS18B20\r\n") : PSTR("DS1820\r\n")) :
                 PSTR("Interner Sensor\r\n"));

    adc_temp_init();
    mess_warte = 50;                        // Referenz einschwingen lassen
}

static void temperatur_tick(bool taste)
{
    if (taste)
    {
        if (kalibrierung)
        {
            // Taste beendet Kalibriermodus
            kalibrierung = false;
            eeprom_update_word(EE_TEMP_KALIB, temp_offset);
            rs485_puts_P(PSTR("Kalibrierung beendet\r\n"));
        }
        if (!ow_warte)
            mess_warte = 1;                 // sofort neu messen
    }

    if (ow_warte)
    {
        if (--ow_warte == 0)
        {
            int16_t t16;
            if (ow_messung_lesen(romcode[0], &t16))
                temperatur_anzeigen(t16);
            else
                temperatur_fehler();
        }
        return;
    }

    if (--mess_warte)
        return;
    mess_warte = kalibrierung ? MESS_TAKT_KALIB : MESS_TAKT;

    if (sensor_extern)
    {
        if (ow_messung_starten())
            ow_warte = romcode[0] == DS18B20 ? 10 : 80;     // 100 ms / 800 ms
        else
            temperatur_fehler();
    }
    else
    {
        temperatur_anzeigen(temp_intern());
        if (kalibrierung)
        {
            // Poti nach der Messung lesen, damit die Referenz bis zur
            // naechsten Messung wieder eingeschwungen ist
            uint8_t poti = adc2_lesen();
            temp_offset = 280 + 30u * poti / 255;
            rs485_puts_P(PSTR("Offset="));
            rs485_zahl(temp_offset);
            rs485_puts_P(PSTR("\r\n"));
        }
    }
}

/******************************************************************************
Betriebsart starten
******************************************************************************/
static void betriebsart_starten(uint8_t ba)
{
    betriebsart = ba;
    led_alle(false);

    switch (ba)
    {
    case BA_TEMPERATUR:
        temperatur_init();
        break;
    case BA_ZWEISPIELER:
        zweispieler_init();
        break;
    case BA_RS485_SPIEL:
        netz_init();
        break;
    }
}

#if UART_DEBUG
/******************************************************************************
Debug-Befehle ueber RS485:  '>' Adresse Kommando Wert CRC (CRC = Adresse)
  0  Ausgabe Software Version
  1  Betriebsart setzen (Wert = BA_...)
  3  Taster simulieren
******************************************************************************/
void debug_befehl(const uint8_t *cw)
{
    if (cw[0] != ADR485 || cw[3] != ADR485)
        return;

    switch (cw[1])
    {
    case 0:
        rs485_puts_P(PSTR("Version " VERSION "\r\n"));
        break;
    case 1:
        if (cw[2] <= BA_RS485_SPIEL)
        {
            rs485_puts_P(PSTR("Betriebsart:"));
            rs485_zahl(cw[2]);
            rs485_puts_P(PSTR("\r\n"));
            betriebsart_starten(cw[2]);
        }
        break;
    case 3:
        taste_simuliert = true;
        break;
    }
}
#endif

int main(void)
{
    ports_init();
    ow_power(true);
    led_init();
    timer_init();
    rs485_init();
    set_sleep_mode(SLEEP_MODE_IDLE);
    sei();                              // INTERRUPTS GLOBAL AN

    uint8_t ba = jumper_lesen();

    // Lampentest, danach die gewaehlte Betriebsart als Augenzahl (1..5)
    led_alle(true);
    _delay_ms(500);
    led_alle(false);
    led_wuerfel(ba + 1);
    _delay_ms(1000);

    rs485_puts_P(PSTR("\r\nTempanzeige und Wuerfel Ver " VERSION ", Betriebsart "));
    rs485_zahl(ba);
    rs485_puts_P(PSTR("\r\n"));

    betriebsart_starten(ba);

    while (1)
    {
        rs485_empfang();

        if (!tick_neu)
        {
            sleep_mode();               // aufwachen mit dem naechsten Interrupt
            continue;
        }
        tick_neu = false;

        bool taste = taste_abfragen();

        switch (betriebsart)
        {
        case BA_TEMPERATUR:
            temperatur_tick(taste);
            break;

        case BA_WUERFEL_6:
        case BA_WUERFEL_7:
            if (taste)
                einzel_taste(betriebsart == BA_WUERFEL_7 ? 7 : 6);
            wuerfel_tick();
            break;

        case BA_ZWEISPIELER:
            if (taste)
                zweispieler_taste();
            zweispieler_tick();
            break;

        case BA_RS485_SPIEL:
            if (taste)
                netz_taste();
            netz_tick();
            break;
        }
    }
}

/******************************************************************************
INTERRUPT Timer 2 Compare A, alle 10 ms: Zeitbasis der Hauptschleife
******************************************************************************/
ISR(TIMER2_COMPA_vect)
{
    ticks++;
    tick_neu = true;
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
