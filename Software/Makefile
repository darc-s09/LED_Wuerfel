# LED Wuerfel / Temperaturanzeige
#
#   make                      Firmware fuer ATmega168P bauen (MCU=atmega328p fuer neue Platinen)
#   make MCU=atmega88a        anderer Controller (ab 8 KB Flash, ATmega48 ist zu klein)
#   make UART_DEBUG=1         mit Debug-Befehlen und -Ausgaben ueber RS485
#   make flash                mit avrdude ueber USBasp programmieren
#
# Mit Atmel Studio: Dateien thermo.c, led.c, spiel.c, rs485.c und 1wire.c
# in das Projekt aufnehmen (uart.c entfaellt) und F_CPU=12000000UL definieren.

MCU        ?= atmega168p
F_CPU      ?= 12000000UL
UART_DEBUG ?= 0
PROGRAMMER ?= usbasp

TARGET = led_wuerfel
SRC    = thermo.c led.c spiel.c rs485.c 1wire.c
BUILD  = build

CC      = avr-gcc
OBJCOPY = avr-objcopy
SIZE    = avr-size

CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DUART_DEBUG=$(UART_DEBUG)
CFLAGS += -Os -std=gnu99 -Wall -Wextra
CFLAGS += -funsigned-char -ffunction-sections -fdata-sections -mrelax
LDFLAGS = -mmcu=$(MCU) -mrelax -Wl,--gc-sections

OBJ = $(SRC:%.c=$(BUILD)/%.o)

all: $(BUILD)/$(TARGET).hex size

$(BUILD)/%.o: %.c thermo.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

$(BUILD):
	mkdir $(BUILD)

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

flash: $(BUILD)/$(TARGET).hex
	avrdude -c $(PROGRAMMER) -p $(MCU) -U flash:w:$<:i

clean:
	rm -rf $(BUILD)

.PHONY: all size flash clean
