# LED_Wuerfel

Bausatz DARC S09: LED-Temperaturanzeige und elektronischer Würfel mit
ATmega168A (12 MHz), 19 LEDs im Multiplex, Taster, DS18B20-Anschluss (JP2)
und RS485-Schnittstelle (MAX481).

## Funktionen und Jumper

Die Funktion wird **beim Einschalten** über Jumper am ISP-Stecker gewählt
(Pin 1 = MISO, 2 = VCC, 3 = SCK, 4 = MOSI, 5 = RESET, 6 = GND).
Nach dem Einschalten leuchten alle LEDs kurz (Lampentest), danach zeigt der
Würfel eine Sekunde lang die erkannte Einstellung als Augenzahl.

| Jumper                | Anzeige | Funktion                                   |
|-----------------------|:-------:|--------------------------------------------|
| keiner                | 1       | Temperaturanzeige                          |
| 4-6 (MOSI–GND)        | 2       | Würfel 1–6                                 |
| 3-4 (SCK–MOSI)        | 3       | Würfel 1–7                                 |
| 1-3 (MISO–SCK)        | 4       | Zweispieler mit einem Würfel               |
| 4-6 **und** 1-3       | 5       | Zweispieler mit zwei Würfeln über RS485    |

### 1. Temperaturanzeige

* Steckt ein DS18B20 oder DS1820 an JP2, wird er automatisch erkannt und
  verwendet, sonst der interne Sensor des ATmega.
* LED-Band: eine LED je 2 °C, LED 1 bis 12 °C, LED 10 ab 30 °C.
  LED 1 und LED 10 gleichzeitig: Sensorfehler.
* Messung alle 5 s, mit dem Taster sofort.
* RS485-Ausgabe nach jeder Messung, z. B. `T=21.5 ext` oder `T=22.0 int`.
* Kalibrierung des internen Sensors: Poti (Mittelstellung) an JP2 stecken und
  einschalten. Der Offset folgt dem Poti und wird als `Offset=…` ausgegeben.
  Mit dem Taster wird der Wert im EEPROM gespeichert.

### 2. Würfel für einen Spieler

Tastendruck würfelt. Die Animation wird langsamer und zeigt dann die Zahl.

### 3. Zweispieler mit einem Würfel („20 gewinnt“)

* Die Spieler drücken abwechselnd, Spieler 1 beginnt.
* Blaue LED 18 bzw. 19 zeigt, wer zuletzt gewürfelt hat, das LED-Band
  zeigt dessen Punktestand: bis 10 Punkte blinkt LED 1, danach ein Balken
  mit (Punkte − 10) LEDs.
* Ab 20 Punkten ist das Spiel gewonnen: Band und blaue LED blinken.
  Der nächste Tastendruck startet ein neues Spiel.
* Vor dem ersten Wurf blinken beide blauen LEDs langsam.

### 4. Zweispieler mit zwei Würfeln über RS485 (neu)

Zwei Boards werden über A/B verbunden (beide mit Jumper 4-6 und 1-3), jedes
Board ist der Würfel eines Spielers. Rollen werden automatisch vergeben:
Wer zuerst drückt, startet das Spiel und ist Spieler 1.

| Anzeige                               | Bedeutung                          |
|---------------------------------------|------------------------------------|
| eigene blaue LED blinkt               | du bist dran                       |
| eigene blaue LED leuchtet             | Gegenspieler ist dran / Spielende  |
| beide blauen LEDs blinken langsam     | kein Spiel – Taste startet eins    |
| blaue LEDs blinken im Wechsel         | keine Verbindung zum anderen Board |
| LED-Band                              | eigener Punktestand (wie Modus 3)  |
| Würfelfeld                            | letzter Wurf, auch der des Gegners |

Der Wurf des Gegenspielers dreht auf beiden Würfeln gleichzeitig. Wer 20
Punkte erreicht, gewinnt (Band blinkt). Danach startet ein Tastendruck an
einem der beiden Boards ein neues Spiel. Wird ein Board während eines
Spiels neu eingeschaltet, holt es sich den Spielstand vom anderen Board.

#### Protokoll

9600 Baud 8N1, Klartext. Mitlesen ist mit jedem USB-RS485-Adapter und einem
Terminalprogramm möglich:

```
':' Typ HEX-Daten HEX-CRC8 CR LF        CRC8 = Dallas/iButton über Typ + Daten
```

| Typ | Daten                                                           | Bedeutung                    |
|-----|-----------------------------------------------------------------|------------------------------|
| `Z` | Spiel-ID, Zug-Nr, Absender, letzter Spieler, Wurf, Punkte 1, Punkte 2 | Wurf, wird bis zur Quittung wiederholt |
| `Q` | Spiel-ID, Zug-Nr                                                | Quittung                     |
| `A` | –                                                               | Anfrage nach dem Einschalten |
| `S` | wie `Z`                                                         | aktueller Spielstand (Antwort auf `A`) |

## Software bauen

Im Ordner `Software`:

```
make                      # ATmega168A, build/led_wuerfel.hex
make MCU=atmega88a        # passt auch in 8 KB
make UART_DEBUG=1         # Debug-Befehle über RS485 ('>' Adresse Kommando Wert CRC)
make flash                # avrdude mit USBasp
```

Für Atmel Studio die Dateien `thermo.c`, `led.c`, `spiel.c`, `rs485.c` und
`1wire.c` ins Projekt aufnehmen (`uart.c` gibt es nicht mehr) und
`-mrelax` sowie `F_CPU=12000000UL` setzen.

| Datei      | Inhalt                                                    |
|------------|-----------------------------------------------------------|
| `thermo.c` | Start, Jumper, Taster, Hauptschleife, Temperaturanzeige   |
| `led.c`    | LED-Multiplexer (Timer 0 mit PWM), Würfelbilder, Band     |
| `spiel.c`  | Würfel, Zweispieler, RS485-Spiel, Zufallsgenerator        |
| `rs485.c`  | interruptgesteuertes Senden/Empfangen, Rahmenprotokoll    |
| `1wire.c`  | DS18B20/DS1820                                            |
