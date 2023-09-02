/*
 * funk-unit-keyboard for Raspberry Pi Pico
 *
 * @version     1.0.0
 * @author      Peter Zimon
 * @copyright   2023
 * @licence     MIT
 *
 */
#include "main.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include <ringbuffer.h>

#define UART_ID uart1
#define MIDI_BAUD_RATE 31250
#define GP_MIDI_IN 9
#define GP_MIDI_OUT 8
#define MIDI_BUFFER_SIZE 32

// High byte of status messages
#define NOTE_OFF            0x80
#define NOTE_ON             0x90
#define POLY_AFTERTOUCH     0xa0
#define CTRL_CHANGE         0xb0
#define PROG_CHANGE         0xc0
#define CH_AFTERTOUCH       0xd0
#define PITCH_BEND          0xe0
#define SYSEX               0xf0

int transpose_by = -12;
RingBuffer midi_buffer;
uint8_t m_buffer_var[MIDI_BUFFER_SIZE];

/**
 * MIDI processor
*/
void process_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    // Transpose notes if needed
    if (transpose_by != 0) {
        // Check if status byte represents a NOTE ON or NOTE OFF message
        if ((status & 0xF0) == NOTE_ON || (status & 0xF0) == NOTE_OFF) {
            // Check if the transposed note is within range
            if ((transpose_by < 0 && data1 >= (-1 * transpose_by + 12)) || (transpose_by > 0 && data1 <= (127 - transpose_by))) {
                data1 += transpose_by;
            }
        }
    }

    // Transmit MIDI data
    if (uart_is_writable(UART_ID)) {
        uart_putc(UART_ID, status);
        uart_putc(UART_ID, data1);
        uart_putc(UART_ID, data2);
    }
}

/**
 * Parse MIDI data
*/
uint8_t running_status;
uint8_t parse_data[3];
uint8_t received_data_bytes;
uint8_t expected_data_size;

void parse_midi(uint8_t byte) {
    // Received status message
    if (byte >= 0x80) {

        // Reset tracking variables
        received_data_bytes = 0;
        expected_data_size = 1;  // Default data size
        running_status = byte;
        uint8_t hi = byte & 0xf0;
        uint8_t lo = byte & 0x0f;

        // Set expected data size
        switch (hi)
        {
        case NOTE_OFF:
        case NOTE_ON:
        case POLY_AFTERTOUCH:
        case CTRL_CHANGE:
        case PITCH_BEND:
            expected_data_size = 2;
            break;

        case SYSEX:
            if (lo > 0 && lo < 3) {
                expected_data_size = 2;
            } else if (lo >= 4) {
                expected_data_size = 0;
            }
            break;

        case PROG_CHANGE:
        case CH_AFTERTOUCH:
            break;
        }

    // Received channel data
    } else {

        parse_data[received_data_bytes] = byte;
        received_data_bytes++;

        if (received_data_bytes >= expected_data_size) {
            process_midi(running_status, parse_data[0], parse_data[1]);
            received_data_bytes = 0;      // Resetting received data
        }
    }
}

/**
 * Main loop
*/
int main() {
    // Use for debugging
    stdio_init_all();

    // Init UART for MIDI
    uart_init(UART_ID, MIDI_BAUD_RATE);
    gpio_set_function(GP_MIDI_IN, GPIO_FUNC_UART);
    gpio_set_function(GP_MIDI_OUT, GPIO_FUNC_UART);

    // Init buffer
    midi_buffer.init(m_buffer_var, MIDI_BUFFER_SIZE);

    while (true) {
        if (uart_is_readable(UART_ID)) {

            uint8_t data = uart_getc(UART_ID);
            midi_buffer.write_byte(data);

            while (!midi_buffer.is_empty()) {
                uint8_t byte = 0;
                midi_buffer.read_byte(byte);
                parse_midi(byte);
            }
        }
    }

    return 0;
}
