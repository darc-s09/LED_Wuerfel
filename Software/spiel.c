/*
  "THE BEER-WARE LICENSE" :
  Uwe Liedtke and Joerg Wunsch wrote this file.  As long as you retain this notice you
  can do whatever you want with this stuff. If we meet some day, and you think
  this stuff is worth it, you can buy us a beer in return.
*/
/*
  spiel.c

  Wuerfel (Einzelspieler 1-6 / 1-7), Zweispieler mit einem Wuerfel und
  Zweispieler mit zwei Wuerfeln ueber RS485 ("20 gewinnt").

  RS485-Spiel, Nachrichten (Rahmenformat siehe rs485.c):
    'Z' Zug      Spiel-ID, Zug-Nr, Absender, letzter Spieler, Wurf, Punkte 1, Punkte 2
                 wird wiederholt, bis die Quittung kommt
    'Q' Quittung Spiel-ID, Zug-Nr
    'A' Anfrage  nach dem Einschalten: "laeuft schon ein Spiel?"
    'S' Zustand  Antwort auf 'A', gleicher Inhalt wie 'Z', ohne Quittung
  Jede Nachricht enthaelt den kompletten Spielstand. Wer ein neues Spiel
  startet, ist Spieler 1, die Gegenstelle wird automatisch Spieler 2.
 */

#include "thermo.h"

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>

#define SPIELER_1       0
#define SPIELER_2       1
#define KEINER          2              // noch kein Wurf im Spiel

#define DREH_START      5              // erste Schrittzeit der Animation in Ticks
#define DREH_BREMSE     1              // jeder weitere Schritt dauert 1 Tick laenger
#define DREH_MIN        8              // Animation 8..15 Schritte (0,7 .. 1,8 s)

#define NETZ_ZUG        'Z'
#define NETZ_QUITTUNG   'Q'
#define NETZ_ANFRAGE    'A'
#define NETZ_ZUSTAND    'S'
#define WIEDERHOL_TICKS 25             // Zug wiederholen nach 250..400 ms
#define VERSUCHE_FEHLER 5              // danach Anzeige "keine Verbindung"

/******************************************************************************
Zufallsgenerator
Jede Flanke am Taster (auch Prellen) addiert den Stand des frei laufenden
Timer 1 (12 MHz). Der Zeitpunkt des Tastendrucks ist damit echter Zufall.
******************************************************************************/
static volatile uint16_t zufall_pool;

ISR(PCINT1_vect)
{
    zufall_pool += TCNT1;
}

uint8_t zufallszahl(uint8_t max)
{
    uint16_t z;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        z = zufall_pool + TCNT1;
        zufall_pool = z * 75 + 74;      // weitermischen fuer den naechsten Aufruf
    }
    return z % max + 1;
}

/******************************************************************************
Wuerfelanimation: ein drehender Balken durch die Mitte, der langsamer wird
und dann das Ergebnis zeigt.
******************************************************************************/
static const uint8_t dreh_muster[3] PROGMEM =
{
    0x07,                               // LED 11, 12, 13  -
    0x51,                               // LED 11, 15, 17  \ .
    0x29,                               // LED 11, 14, 16  /
};

static uint8_t dreh_rest;              // verbleibende Schritte, 0 = Wuerfel steht
static uint8_t dreh_warte;
static uint8_t dreh_zeit;
static uint8_t dreh_pos;
static uint8_t dreh_ergebnis;

static void wuerfel_starten(uint8_t zahl, uint8_t schritte)
{
    dreh_ergebnis = zahl;
    dreh_rest = schritte;
    dreh_zeit = DREH_START;
    dreh_warte = 1;
    dreh_pos = 0;
}

static bool wuerfel_rollt(void)
{
    return dreh_rest != 0;
}

void wuerfel_tick(void)
{
    if (!dreh_rest || --dreh_warte)
        return;
    if (--dreh_rest == 0)
    {
        led_wuerfel(dreh_ergebnis);
        return;
    }
    led_feld(pgm_read_byte(&dreh_muster[dreh_pos]));
    if (++dreh_pos == sizeof dreh_muster)
        dreh_pos = 0;
    dreh_warte = dreh_zeit;
    dreh_zeit += DREH_BREMSE;
}

/******************************************************************************
Einzelspieler
******************************************************************************/
void einzel_taste(uint8_t max)
{
    if (wuerfel_rollt())
        return;
    uint8_t zahl = zufallszahl(max);
    wuerfel_starten(zahl, DREH_MIN - 1 + zufallszahl(8));
#if UART_DEBUG
    rs485_puts_P(PSTR("Wurf "));
    rs485_zahl(zahl);
    rs485_puts_P(PSTR("\r\n"));
#endif
}

/******************************************************************************
Gemeinsamer Spielstand fuer beide Zweispieler-Varianten
******************************************************************************/
static struct
{
    uint8_t letzter;                    // Spieler des letzten Wurfs oder KEINER
    uint8_t wurf;
    uint8_t punkte[2];
    // nur RS485-Spiel
    bool    aktiv;                      // Spielstand bekannt
    uint8_t ich;                        // eigene Rolle SPIELER_1 / SPIELER_2
    uint8_t id;                         // Spielkennung (zufaellig)
    uint8_t zug;                        // Zugnummer, steigt mit jedem Wurf
} spiel;

static void spiel_neu(void)
{
    spiel.letzter = KEINER;
    spiel.wurf = 0;
    spiel.punkte[SPIELER_1] = 0;
    spiel.punkte[SPIELER_2] = 0;
}

static bool spiel_ende(void)
{
    return spiel.punkte[SPIELER_1] >= ZIEL_PUNKTE ||
           spiel.punkte[SPIELER_2] >= ZIEL_PUNKTE;
}

static uint8_t am_zug(void)
{
    return spiel.letzter == SPIELER_1 ? SPIELER_2 : SPIELER_1;
}

static void wurf_buchen(uint8_t spieler)
{
    spiel.wurf = zufallszahl(6);
    spiel.punkte[spieler] += spiel.wurf;
    spiel.letzter = spieler;
}

/******************************************************************************
Punktestand auf dem LED Band (wie bisher):
  bis 10 Punkte    LED 1 blinkt
  11 .. 19 Punkte  Balken mit (Punkte - 10) LEDs
  ab 20 Punkte     gewonnen, ganzes Band blinkt
******************************************************************************/
static void punkte_anzeigen(uint8_t punkte)
{
    bool phase = blink(BLINK_NORMAL);

    if (punkte >= ZIEL_PUNKTE)
    {
        led_band_balken(phase ? LED_BAND : 0);
    }
    else if (punkte > ZIEL_PUNKTE - LED_BAND)
    {
        led_band_balken(punkte - (ZIEL_PUNKTE - LED_BAND));
    }
    else
    {
        led_band_balken(0);
        led_an[1] = phase;
    }
}

static void spieler_leds(bool spieler1, bool spieler2)
{
    led_an[LED_SPIELER1] = spieler1;
    led_an[LED_SPIELER2] = spieler2;
}

/******************************************************************************
Zweispieler mit einem Wuerfel
Die blaue LED zeigt, wer zuletzt gewuerfelt hat, das Band dessen Punktestand.
Nach Spielende startet der naechste Tastendruck ein neues Spiel.
******************************************************************************/
void zweispieler_init(void)
{
    spiel_neu();
}

void zweispieler_taste(void)
{
    if (wuerfel_rollt())
        return;
    if (spiel_ende())
        spiel_neu();
    uint8_t spieler = am_zug();
    wurf_buchen(spieler);
    wuerfel_starten(spiel.wurf, DREH_MIN - 1 + zufallszahl(8));
#if UART_DEBUG
    rs485_putc(spieler == SPIELER_1 ? 'A' : 'B');
    rs485_zahl(spiel.punkte[spieler]);
    rs485_puts_P(PSTR("\r\n"));
#endif
}

void zweispieler_tick(void)
{
    wuerfel_tick();

    if (spiel.letzter == KEINER)
    {
        // bereit: beide blauen LEDs blinken langsam
        bool phase = blink(BLINK_LANGSAM);
        spieler_leds(phase, phase);
        led_band_balken(0);
        return;
    }

    bool an = (spiel_ende() && !wuerfel_rollt()) ? blink(BLINK_NORMAL) : true;
    spieler_leds(spiel.letzter == SPIELER_1 && an, spiel.letzter == SPIELER_2 && an);

    if (wuerfel_rollt())
        led_band_balken(0);
    else
        punkte_anzeigen(spiel.punkte[spiel.letzter]);
}

/******************************************************************************
Zweispieler mit zwei Wuerfeln ueber RS485
Jedes Board ist der Wuerfel eines Spielers.
  eigene blaue LED blinkt    : du bist dran
  eigene blaue LED leuchtet  : Gegenspieler ist dran / Spielende
  beide blinken langsam      : kein Spiel, Taste startet ein neues Spiel
  beide blinken im Wechsel   : keine Verbindung zum anderen Wuerfel
  LED Band                   : eigener Punktestand
  Wuerfelfeld                : letzter Wurf (auch der des Gegenspielers)
******************************************************************************/
static bool    quittung_offen;
static uint8_t versuche;
static uint8_t wiederhol_warte;

static void zug_senden(char typ)
{
    uint8_t d[7] =
    {
        spiel.id, spiel.zug, spiel.ich, spiel.letzter, spiel.wurf,
        spiel.punkte[SPIELER_1], spiel.punkte[SPIELER_2]
    };
    rs485_rahmen_senden(typ, d, sizeof d);
}

// Animationslaenge aus dem Zug, damit beide Wuerfel gleich lange drehen
static uint8_t netz_schritte(void)
{
    return DREH_MIN + ((spiel.zug * 5 + spiel.wurf) & 7);
}

void netz_init(void)
{
    spiel_neu();
    spiel.aktiv = false;
    quittung_offen = false;
    rs485_rahmen_senden(NETZ_ANFRAGE, 0, 0);
}

void netz_taste(void)
{
    if (wuerfel_rollt() || quittung_offen)
        return;

    if (!spiel.aktiv || spiel_ende())
    {
        // neues Spiel: wer startet, ist Spieler 1 und wuerfelt zuerst
        uint8_t alte_id = spiel.id;
        spiel_neu();
        spiel.aktiv = true;
        spiel.ich = SPIELER_1;
        spiel.zug = 0;
        do
            spiel.id = zufallszahl(255);
        while (spiel.id == alte_id);
    }

    if (am_zug() != spiel.ich)
        return;                         // Gegenspieler ist dran

    spiel.zug++;
    wurf_buchen(spiel.ich);
    wuerfel_starten(spiel.wurf, netz_schritte());

    zug_senden(NETZ_ZUG);
    quittung_offen = true;
    versuche = 1;
    wiederhol_warte = WIEDERHOL_TICKS;
}

void netz_empfang(char typ, const uint8_t *d, uint8_t laenge)
{
    if (betriebsart != BA_RS485_SPIEL)
        return;

    switch (typ)
    {
    case NETZ_QUITTUNG:
        if (laenge == 2 && quittung_offen && d[0] == spiel.id && d[1] == spiel.zug)
            quittung_offen = false;
        break;

    case NETZ_ANFRAGE:
        if (spiel.aktiv)
            zug_senden(NETZ_ZUSTAND);
        break;

    case NETZ_ZUG:
    case NETZ_ZUSTAND:
        if (laenge != 7 || d[2] > SPIELER_2 || d[3] > KEINER)
            break;

        if (spiel.aktiv && d[0] == spiel.id)
        {
            if ((int8_t)(d[1] - spiel.zug) <= 0)
            {
                // bereits bekannt (Wiederholung): nur quittieren
                if (typ == NETZ_ZUG)
                    rs485_rahmen_senden(NETZ_QUITTUNG, d, 2);
                break;
            }
        }
        else if (quittung_offen && spiel.zug == 1 && spiel.id > d[0])
        {
            // beide haben gleichzeitig ein Spiel gestartet: die hoehere
            // Spielkennung gewinnt, die Gegenstelle uebernimmt unser Spiel
            break;
        }

        // Spielstand uebernehmen
        spiel.aktiv = true;
        spiel.id = d[0];
        spiel.zug = d[1];
        spiel.ich = d[2] == SPIELER_1 ? SPIELER_2 : SPIELER_1;
        spiel.letzter = d[3];
        spiel.wurf = d[4];
        spiel.punkte[SPIELER_1] = d[5];
        spiel.punkte[SPIELER_2] = d[6];
        quittung_offen = false;

        if (typ == NETZ_ZUG)
        {
            rs485_rahmen_senden(NETZ_QUITTUNG, d, 2);
            wuerfel_starten(spiel.wurf, netz_schritte());
        }
        else
        {
            dreh_rest = 0;              // laufende Animation abbrechen
            led_wuerfel(spiel.wurf);
        }
        break;
    }
}

void netz_tick(void)
{
    wuerfel_tick();

    if (quittung_offen && --wiederhol_warte == 0)
    {
        zug_senden(NETZ_ZUG);
        if (versuche < 255)
            versuche++;
        // etwas Zufall, damit sich zwei Sender nicht dauernd ueberlagern
        wiederhol_warte = WIEDERHOL_TICKS + ((uint8_t)zufall_pool & 0x0F);
    }

    if (quittung_offen && versuche > VERSUCHE_FEHLER)
    {
        bool phase = blink(BLINK_SCHNELL);
        spieler_leds(phase, !phase);
    }
    else if (!spiel.aktiv)
    {
        bool phase = blink(BLINK_LANGSAM);
        spieler_leds(phase, phase);
    }
    else
    {
        bool dran = !spiel_ende() && am_zug() == spiel.ich && !wuerfel_rollt();
        bool an = dran ? blink(BLINK_NORMAL) : true;
        spieler_leds(spiel.ich == SPIELER_1 && an, spiel.ich == SPIELER_2 && an);
    }

    if (!spiel.aktiv || wuerfel_rollt())
        led_band_balken(0);
    else
        punkte_anzeigen(spiel.punkte[spiel.ich]);
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
