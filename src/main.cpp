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
#include <button.h>
#include <utils.h>

#define UART_ID uart1
#define MIDI_BAUD_RATE 31250
#define GP_MIDI_IN 9
#define GP_MIDI_OUT 8
#define GP_BTN_OCTAVE_UP 4
#define GP_BTN_OCTAVE_DOWN 5
#define GP_BTN_CHORDIFY 27

#define MIDI_BUFFER_SIZE 128
#define NO_OF_MIDI_NOTES 128

#define GP_STATUS_LED 16

// High byte of status messages
#define NOTE_OFF            0x80
#define NOTE_ON             0x90
#define POLY_AFTERTOUCH     0xa0
#define CTRL_CHANGE         0xb0
#define PROG_CHANGE         0xc0
#define CH_AFTERTOUCH       0xd0
#define PITCH_BEND          0xe0
#define SYSEX               0xf0

// Octave
#define MAX_OCTAVES_UP 3
#define MAX_OCTAVES_DOWN 3

// Pitch
#define GP_PITCH 26
#define PITCH_TRESHOLD 15
#define PITCH_ZERO 8192
#define PITCH_RESET_TRESHOLD 80 // 8192 means zero pitch bend, however the ADC
                                // of the pico is _very_ inaccurate so if the
                                // actual value gets in the bounds of this
                                // treshold value then it resets to 8192.

// Chordifier
#define BLINK_TIME_MS 350
#define MAX_CHORD_SIZE 10
#define MIDDLE_C 60


/**
 * Variables
*/

// MIDI processing
int transpose_by = 0;
RingBuffer midi_buffer;
uint8_t buffer_var[MIDI_BUFFER_SIZE];
bool notes_on[NO_OF_MIDI_NOTES];
uint8_t running_status;
uint8_t parse_data[3];
uint8_t received_data_bytes;
uint8_t expected_data_size;

// Octave handler
Button btn_octave_up = Button(GP_BTN_OCTAVE_UP);
Button btn_octave_down = Button(GP_BTN_OCTAVE_DOWN);

// Hardware pitch bend
int last_pitch_value = 0;

// Chordifier
int note_history[MAX_CHORD_SIZE];
int notes_played = 0;
bool led_on = false;
bool chordify_on = false;
uint32_t millis = Utils::millis();
Button btn_chordify = Button(GP_BTN_CHORDIFY);
int no_of_chord_notes = 0;
bool chord_set = false;
int chord[MAX_CHORD_SIZE];
void chord_on(uint8_t note, uint8_t velocity);
void chord_off(uint8_t note);
int note_on_status = 0b10010000;
// uint8_t last_note_played = 0;

/**
 * MIDI processor
*/
void process_midi(uint8_t status, uint8_t data1, uint8_t data2) {

    // printf("Status: %d\n", status);
    // printf("Data1: %d\n", data1);
    // printf("Data2: %d\n", data2);

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

    // Maintain played note buffer to be able to clear it on octave switch
    if ((status & 0xF0) == NOTE_ON && data2 > 0) {
        notes_on[data1] = true;

        if (!chordify_on) {
            // Clear history if this is the first note after releasing all notes
            if (notes_played == 0) {
                for (int i = 0; i < MAX_CHORD_SIZE; i++) {
                    note_history[i] = -1;
                }
            }
            note_history[notes_played] = data1;
        } else {
            // if (notes_played != 0) {
            //     chord_off(last_note_played);
            // }
            chord_on(data1, data2);
            // last_note_played = data1;
        }
        notes_played++;
    }

    if (((status & 0xF0) == NOTE_ON && data2 == 0) || (status & 0xF0) == NOTE_OFF) {
        notes_on[data1] = false;
        if (chordify_on) {
            chord_off(data1);
        }
        notes_played--;
    }

    if (notes_played < 0) {
        notes_played = 0;
    }

    if (notes_played > MAX_CHORD_SIZE) {
        notes_played = MAX_CHORD_SIZE;
    }

    // Transmit MIDI data, only if chordify is off or (when it's on) it's not
    // note related data
    if (!chordify_on || (chordify_on && (status & 0xF0) != NOTE_ON && (status & 0xF0) != NOTE_OFF)) {
        if (uart_is_writable(UART_ID)) {
            uart_putc(UART_ID, status);
            uart_putc(UART_ID, data1);
            uart_putc(UART_ID, data2);
        }
    }
}

/**
 * Parse MIDI data
*/
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
 * Handling octave buttons
*/
void transpose(bool up) {
    if (up) {
        if (transpose_by + 12 < 12 * MAX_OCTAVES_UP) {
            transpose_by += 12;
        }
    } else {
        if (transpose_by - 12 > -1 * 12 * MAX_OCTAVES_DOWN) {
            transpose_by -= 12;
        }
    }

    // Send a note off message for all notes. Paraszt™
    if (uart_is_writable(UART_ID)) {
        for (uint8_t i = 0; i < NO_OF_MIDI_NOTES; i++) {
            if (notes_on[i]) {
                uart_putc(UART_ID, 0b10010000);
                uart_putc(UART_ID, i);
                uart_putc(UART_ID, 0);
            }
        }
    }
}

/**
 * Handling pitch bend
*/
void set_pitch(uint16_t pitch) {

    int adj_pitch = pitch;
    // printf("pitch: %d\n", pitch);
    // printf("diff: %d\n", abs(pitch - PITCH_ZERO));

    if (abs(pitch - PITCH_ZERO) <= PITCH_RESET_TRESHOLD) {
        adj_pitch = PITCH_ZERO;
    }

    // printf("adjusted pitch: %d\n", adj_pitch);
    // printf("---\n");

    if (uart_is_writable(UART_ID)) {
        uart_putc(UART_ID, 0xE0);
        uart_putc(UART_ID, adj_pitch & 0x7F);
        uart_putc(UART_ID, (adj_pitch >> 7) & 0x7F);
    }
}


/**
 * Chordifier: remembers the last played notes and plays as a chord when ON
*/
void chordify() {
    if (btn_chordify.is_released()) {
        chordify_on = !chordify_on;
    }

    if (chordify_on) {
        if (!chord_set) {
            // bool all_notes_released = true; // Save only if all notes are released
            no_of_chord_notes = 0;

            // There are two ways to save a chord: if there are any notes played
            // then those should build the chord, or if there are no notes
            // played: in this case use the last played chord.
            if (notes_played) {
                for (int i = 0; i < NO_OF_MIDI_NOTES; i++) {
                    if (notes_on[i]) {
                        chord[no_of_chord_notes] = i;
                        no_of_chord_notes++;
                    }
                    if (no_of_chord_notes >= MAX_CHORD_SIZE) {
                        break;
                    }
                }
            } else {
                for (int i = 0; i < MAX_CHORD_SIZE; i++) {
                    if (note_history[i] != -1) {
                        chord[no_of_chord_notes] = note_history[i];
                        no_of_chord_notes++;
                    }
                    if (no_of_chord_notes >= MAX_CHORD_SIZE) {
                        break;
                    }
                }
            }

            // Only turn on chordify if there is actually a chord set
            if (no_of_chord_notes != 0) {
                // printf("New chord\n");
                // for (int i = 0; i < no_of_chord_notes; i++) {
                //     printf("note %d: %d\n", i, chord[i]);
                // }
                chord_set = true;
            } else {
                chordify_on = false;
            }
        }

        // Blink the status LED
        if (Utils::millis() - millis > BLINK_TIME_MS) {
            led_on = !led_on;
            gpio_put(GP_STATUS_LED, led_on);
            millis = Utils::millis();
        }
    } else {
        for (int i = 0; i < MAX_CHORD_SIZE; i++) {
            chord[i] = -1;
        }
        no_of_chord_notes = 0;
        chord_set = false;

        // Constant status LED
        if (!led_on) {
            gpio_put(GP_STATUS_LED, 1);
            led_on = true;
        }
    }
}

// Plays chord instead of note
void chord_on(uint8_t note, uint8_t velocity) {
    if (!chord_set) return;
    uint8_t data1 = 0;
    if (uart_is_writable(UART_ID)) {
        for (int i = 0; i < no_of_chord_notes; i++) {
            int transformed_note = chord[i] + note - MIDDLE_C;
            if (chord[i] != -1 && transformed_note < NO_OF_MIDI_NOTES) {
                if (transformed_note >= 0) {
                    data1 = transformed_note;
                    uart_putc(UART_ID, note_on_status);
                    uart_putc(UART_ID, data1);
                    uart_putc(UART_ID, velocity);
                }
            } else {
                break;
            }
        }
    }
}

// Release all played chord notes (which is practically calling chord_on with
// zero velocity)
void chord_off(uint8_t note) {
    chord_on(note, 0);
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
    midi_buffer.init(buffer_var, MIDI_BUFFER_SIZE);

    // Init buttons
    btn_octave_up.init_gpio();
    btn_octave_down.init_gpio();
    btn_chordify.init_gpio();

    // Init played notes array
    for (int i = 0; i < NO_OF_MIDI_NOTES; i++) {
        notes_on[i] = false;
    }

    // Init history and chord array
    for (int i = 0; i < MAX_CHORD_SIZE; i++) {
        note_history[i] = -1;
        chord[i] = -1;
    }

    // Init pitch bend input
    adc_init();
    adc_gpio_init(GP_PITCH);
    adc_select_input(0);

    // Init status LED
    gpio_init(GP_STATUS_LED);
    gpio_set_dir(GP_STATUS_LED, GPIO_OUT);

    sleep_ms(1000);

    while (true) {
        if (uart_is_readable(UART_ID)) {
            uint8_t data = uart_getc(UART_ID);
            midi_buffer.write_byte(data);
        }

        // Process incoming midi data
        while (!midi_buffer.is_empty()) {
            uint8_t byte = 0;
            midi_buffer.read_byte(byte);
            parse_midi(byte);
        }

        // Handle octave buttons
        if (btn_octave_up.is_released()) {
            transpose(true);
        }

        if (btn_octave_down.is_released()) {
            transpose(false);
        }

        // Handle pitch bend hardware. Fucken MIDI keyboard didn't have one
        uint16_t pot_value = adc_read();
        uint16_t pot_value_lores = pot_value >> 3;
        if (abs(last_pitch_value - pot_value_lores) > PITCH_TRESHOLD) {
            last_pitch_value = pot_value_lores;
            uint16_t pitch = Utils::map(pot_value, 0, 4096, 0, 16383);
            set_pitch(pitch);
        }

        // Chordifier
        chordify();
    }

    return 0;
}
