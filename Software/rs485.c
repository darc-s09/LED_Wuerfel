/*
  "THE BEER-WARE LICENSE" :
  Uwe Liedtke and Joerg Wunsch wrote this file.  As long as you retain this notice you
  can do whatever you want with this stuff. If we meet some day, and you think
  this stuff is worth it, you can buy us a beer in return.
*/
/*
  rs485.c

  RS485 ueber MAX481 (Halbduplex), 9600 Baud 8N1.
  Senden und Empfangen laufen interruptgesteuert ueber Ringpuffer, die
  Hauptschleife wird nicht blockiert. Der Sender (PC5) wird erst nach dem
  letzten Stoppbit (TX Complete) wieder auf Empfang geschaltet.

  Rahmenformat fuer das Spiel (lesbar mit jedem Terminalprogramm):
      ':' TYP  HEX-Daten  HEX-CRC8  CR LF
  TYP ist ein Buchstabe, die CRC8 (Dallas/iButton) laeuft ueber TYP und Daten.
 */

#include "thermo.h"

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include <util/crc16.h>
#include <util/delay.h>

#define BAUD            9600
#define UBRR_WERT       ((F_CPU + 8UL * BAUD) / (16UL * BAUD) - 1)
#define RX_GROESSE      32             // Zweierpotenz
#define TX_GROESSE      64             // Zweierpotenz
#define RAHMEN_MAX      8              // max. Datenbytes je Rahmen (ohne CRC)

static volatile uint8_t rx_puffer[RX_GROESSE];
static volatile uint8_t rx_schreib, rx_lese;
static volatile uint8_t tx_puffer[TX_GROESSE];
static volatile uint8_t tx_schreib, tx_lese;

/*****************************************************************************************
initialisieren des UART
Baudrate einstellen, Senden und Empfangen aktivieren, Empfangs- und TX-Complete-Interrupt
*****************************************************************************************/
void rs485_init(void)
{
    UBRR0  = UBRR_WERT;
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0) | _BV(TXCIE0);
}

/******************************************************************************
Sendet ein Byte ueber die RS485 (wartet nur, wenn der Sendepuffer voll ist)
******************************************************************************/
void rs485_putc(char c)
{
    uint8_t naechster = (tx_schreib + 1) & (TX_GROESSE - 1);

    while (naechster == tx_lese)
        ;
    if (!qbi(5, PORTC))
        _delay_us(200);                 // Gegenstelle Zeit geben, ihren Sender abzuschalten
    tx_puffer[tx_schreib] = c;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        tx_schreib = naechster;
        SENDEN_AKTIV;                   // RX > TX Umschaltung
        // Empfaenger aus: der MAX481 gibt beim Senden RO nicht aus
        UCSR0B = (UCSR0B & ~_BV(RXEN0)) | _BV(UDRIE0);
    }
}

void rs485_puts_P(const char *s)
{
    char c;

    while ((c = pgm_read_byte(s++)) != 0)
        rs485_putc(c);
}

void rs485_zahl(uint16_t zahl)
{
    char ziffern[5];
    uint8_t n = 0;

    do
    {
        ziffern[n++] = '0' + zahl % 10;
        zahl /= 10;
    }
    while (zahl);
    while (n)
        rs485_putc(ziffern[--n]);
}

static void hexziffer(uint8_t w)
{
    rs485_putc(w < 10 ? '0' + w : 'A' - 10 + w);
}

void rs485_hex(uint8_t wert)
{
    hexziffer(wert >> 4);
    hexziffer(wert & 0x0F);
}

void rs485_rahmen_senden(char typ, const uint8_t *daten, uint8_t laenge)
{
    uint8_t crc = _crc_ibutton_update(0, typ);

    rs485_putc(':');
    rs485_putc(typ);
    while (laenge--)
    {
        crc = _crc_ibutton_update(crc, *daten);
        rs485_hex(*daten++);
    }
    rs485_hex(crc);
    rs485_putc('\r');
    rs485_putc('\n');
}

static uint8_t hexwert(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 0x20;                          // Kleinbuchstaben
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return 0xFF;
}

/******************************************************************************
Auswertung der empfangenen Zeichen, Aufruf aus der Hauptschleife.
Gueltige Spielrahmen gehen an netz_empfang().
Im Debug-Build zusaetzlich das alte Steuerwort:
    '>' Adresse Kommando Wert CRC   (4 Binaerbytes nach '>', CRC = Adresse)
******************************************************************************/
void rs485_empfang(void)
{
    static uint8_t daten[RAHMEN_MAX + 1];   // + CRC
    static int8_t  pos = -1;                // -1 = kein Rahmen aktiv
    static char    typ;
    static uint8_t nibble;                  // 0xFF = keine halbe Hexzahl offen
#if UART_DEBUG
    static uint8_t cw[4];
    static int8_t  cw_pos = -1;
#endif

    while (rx_lese != rx_schreib)
    {
        uint8_t c = rx_puffer[rx_lese];
        rx_lese = (rx_lese + 1) & (RX_GROESSE - 1);

#if UART_DEBUG
        if (cw_pos >= 0)
        {
            cw[cw_pos++] = c;
            if (cw_pos == sizeof cw)
            {
                cw_pos = -1;
                debug_befehl(cw);
            }
            continue;
        }
        if (c == '>')
        {
            cw_pos = 0;
            continue;
        }
#endif
        if (c == ':')
        {
            pos = 0;
            typ = 0;
            nibble = 0xFF;
            continue;
        }
        if (pos < 0)
            continue;
        if (!typ)
        {
            typ = c;
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            if (nibble == 0xFF && pos > 0)
            {
                uint8_t crc = _crc_ibutton_update(0, typ);
                for (uint8_t i = 0; i < pos; i++)
                    crc = _crc_ibutton_update(crc, daten[i]);
                if (crc == 0)           // CRC ueber Daten + CRC-Byte ergibt 0
                    netz_empfang(typ, daten, pos - 1);
            }
            pos = -1;
            continue;
        }

        uint8_t w = hexwert(c);
        if (w > 15)
        {
            pos = -1;                   // ungueltiges Zeichen: Rahmen verwerfen
        }
        else if (nibble == 0xFF)
        {
            nibble = w;
        }
        else if (pos < (int8_t)sizeof daten)
        {
            daten[pos++] = (nibble << 4) | w;
            nibble = 0xFF;
        }
        else
        {
            pos = -1;                   // zu lang
        }
    }
}

/******************************************************************************
INTERRUPT Empfang: Zeichen in den Ringpuffer (fehlerhafte Zeichen verwerfen)
******************************************************************************/
ISR(USART_RX_vect)
{
    uint8_t status = UCSR0A;
    uint8_t c = UDR0;
    uint8_t naechster = (rx_schreib + 1) & (RX_GROESSE - 1);

    if (!(status & (_BV(FE0) | _BV(DOR0))) && naechster != rx_lese)
    {
        rx_puffer[rx_schreib] = c;
        rx_schreib = naechster;
    }
}

/******************************************************************************
INTERRUPT Senderegister leer: naechstes Zeichen senden
******************************************************************************/
ISR(USART_UDRE_vect)
{
    if (tx_lese != tx_schreib)
    {
        // TXC loeschen, damit der TX-Complete-Interrupt erst nach diesem Zeichen kommt
        UCSR0A = (UCSR0A & (_BV(U2X0) | _BV(MPCM0))) | _BV(TXC0);
        UDR0 = tx_puffer[tx_lese];
        tx_lese = (tx_lese + 1) & (TX_GROESSE - 1);
    }
    else
    {
        UCSR0B &= ~_BV(UDRIE0);
    }
}

/******************************************************************************
INTERRUPT Senden komplett: zurueck auf Empfang
******************************************************************************/
ISR(USART_TX_vect)
{
    if (tx_lese == tx_schreib)
    {
        SENDEN_INAKTIV;
        UCSR0B |= _BV(RXEN0);
    }
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
