/**
 * @file    BuzzerPIO_RP2040.h
 * @brief   Zero-jitter tone generation for passive buzzers using the RP2040
 *          PIO coprocessor with hardware-timed melody sequencing.
 * @version 1.0.0
 * @license MIT
 *
 * @details
 *   This library generates square waves entirely in the PIO hardware,
 *   freeing the CPU from any timing-critical work. Features include:
 *
 *   - **PIO-based tone generation**: A 4-instruction PIO program produces
 *     a square wave with zero CPU jitter. Frequency is set via the PIO
 *     clock divider; volume is controlled by patching instruction delays
 *     to adjust the duty cycle in real time.
 *
 *   - **Hardware alarm melody sequencing**: Melodies (arrays of notes)
 *     are sequenced by the RP2040 hardware alarm timer in IRQ context.
 *     Each note transition happens with < 10 µs jitter, regardless of
 *     what the main loop is doing (Wi-Fi, display, flash I/O, etc.).
 *
 *   - **Non-blocking API**: `tone()`, `playMelody()`, and `playMelodyLoop()`
 *     return immediately. No `delay()`, no polling, no `update()` needed.
 *
 *   - **No PWM conflicts**: Unlike `tone()` or analogWrite, this library
 *     uses a PIO state machine instead of a PWM slice, so it won't
 *     interfere with PWM-based servos, motors, or LED dimming.
 *
 * @par Resources used
 *   - 1 PIO state machine (auto-claimed from the specified PIO block)
 *   - 4 instruction slots in PIO instruction memory
 *   - 1 hardware alarm (from the Pico SDK alarm pool)
 *   - 1 GPIO pin (configurable)
 *
 * @par Frequency range
 *   With the default 125 MHz system clock:
 *   - Minimum: ~15 Hz  (divider = 65535)
 *   - Maximum: ~976 kHz (divider = 1.0)
 *   - Audible sweet spot: 200 Hz – 8 kHz
 *
 * @par Compatibility
 *   - Raspberry Pi Pico / Pico W (RP2040)
 *   - Arduino-Pico core (Earle Philhower)
 *   - PlatformIO with platform = raspberrypi
 *
 * @par Example
 * @code
 *   #include <BuzzerPIO_RP2040.h>
 *
 *   BuzzerPIO buzzer(22);  // GPIO 22
 *
 *   void setup() {
 *       buzzer.begin();
 *       buzzer.setVolume(80);
 *       buzzer.tone(1000, 500);  // 1 kHz for 500 ms (non-blocking)
 *   }
 *
 *   void loop() {
 *       // CPU is 100% free — the PIO handles the tone
 *   }
 * @endcode
 */

#ifndef BUZZER_PIO_RP2040_H
#define BUZZER_PIO_RP2040_H

#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

/**
 * @brief  Maximum frequency in Hz (~976 kHz at 125 MHz sys_clk).
 */
#define BUZZER_PIO_MAX_FREQ 976000

/**
 * @brief  Minimum frequency in Hz (~15 Hz at 125 MHz sys_clk).
 */
#define BUZZER_PIO_MIN_FREQ 15

/**
 * @brief  A single note in a melody sequence.
 *
 * @param  freqHz     Frequency in Hz. Use 0 for a silent pause.
 * @param  durationMs Duration in milliseconds.
 */
struct BuzzerNote {
    uint16_t freqHz;
    uint16_t durationMs;
};


/**
 * @class  BuzzerPIO
 * @brief  PIO-driven passive buzzer with non-blocking melody playback.
 *
 * @details
 *   Instantiate one BuzzerPIO per physical buzzer. Call `begin()` once
 *   in `setup()`. All other methods are non-blocking and can be called
 *   from any context (loop, ISR, timer callback, second core).
 *
 *   The destructor calls `end()` automatically, releasing all PIO and
 *   alarm resources.
 */
class BuzzerPIO {
public:
    // ── Construction ─────────────────────────────────────────────────

    /**
     * @brief  Construct a BuzzerPIO on the specified GPIO pin.
     * @param  pin  GPIO number connected to the passive buzzer (0-28).
     * @param  pio  PIO block to use: `pio0` or `pio1`. Default: `pio0`.
     */
    explicit BuzzerPIO(uint8_t pin, PIO pio = pio0);

    /**
     * @brief  Destructor. Calls `end()` to release all resources.
     */
    ~BuzzerPIO();

    // ── Lifecycle ────────────────────────────────────────────────────

    /**
     * @brief  Initialize PIO hardware. Must be called once in `setup()`.
     * @return `true` if PIO state machine and instruction memory were
     *         allocated successfully, `false` if no resources available.
     */
    bool begin();

    /**
     * @brief  Release all PIO and alarm resources. The GPIO is returned
     *         to standard digital output LOW. Safe to call multiple times.
     */
    void end();

    /**
     * @brief  Check if `begin()` succeeded and the PIO is ready.
     * @return `true` if initialized and operational.
     */
    bool isReady() const { return _ready; }

    // ── Single tone ──────────────────────────────────────────────────

    /**
     * @brief  Start a continuous tone at the given frequency.
     *         Non-blocking — returns immediately. The tone plays until
     *         `noTone()`, `tone()` with another frequency, or `playMelody()`
     *         is called.
     *
     * @param  freqHz  Frequency in Hz (15–976000). 0 is treated as silence.
     */
    void tone(uint16_t freqHz);

    /**
     * @brief  Start a tone that auto-stops after a given duration.
     *         Non-blocking — the hardware alarm handles the shutoff.
     *
     * @param  freqHz     Frequency in Hz.
     * @param  durationMs Duration in milliseconds (1–65535).
     */
    void tone(uint16_t freqHz, uint16_t durationMs);

    /**
     * @brief  Stop any tone immediately. Silences the buzzer.
     */
    void noTone();

    // ── Volume ───────────────────────────────────────────────────────

    /**
     * @brief  Set the volume (duty cycle) of the square wave.
     *         Can be changed while a tone is playing — takes effect
     *         immediately via PIO instruction patching.
     *
     * @param  volume  0 (silent) to 100 (maximum, 50% duty cycle).
     *                 Default after `begin()` is 100.
     */
    void setVolume(uint8_t volume);

    /**
     * @brief  Get the current volume setting.
     * @return Volume percentage (0–100).
     */
    uint8_t getVolume() const { return _volume; }

    // ── Melody playback ──────────────────────────────────────────────

    /**
     * @brief  Play a melody (array of BuzzerNote) once, non-blocking.
     *         Each note is sequenced by hardware alarms in IRQ context
     *         with < 10 µs jitter. Any currently playing tone or melody
     *         is interrupted.
     *
     * @param  notes  Pointer to an array of BuzzerNote (must remain
     *                valid for the entire duration of playback — use
     *                `const` arrays in flash or global scope).
     * @param  len    Number of notes in the array (1–255).
     */
    void playMelody(const BuzzerNote* notes, uint8_t len);

    /**
     * @brief  Play a melody in an infinite loop until `stopMelody()`
     *         is called. Useful for alarm sirens, ringtones, etc.
     *
     * @param  notes  Pointer to the note array (must remain valid).
     * @param  len    Number of notes.
     */
    void playMelodyLoop(const BuzzerNote* notes, uint8_t len);

    /**
     * @brief  Stop melody playback immediately (both one-shot and loop).
     *         Silences the buzzer.
     */
    void stopMelody();

    /**
     * @brief  Check if a melody is currently playing.
     * @return `true` if a melody (one-shot or loop) is in progress.
     */
    bool isPlaying() const { return _melody != nullptr; }

    /**
     * @brief  Check if a melody is playing in loop mode.
     * @return `true` if `playMelodyLoop()` is active.
     */
    bool isLooping() const { return _looping; }

private:
    // ── PIO resources ────────────────────────────────────────────────
    PIO      _pio;
    uint8_t  _pin;
    uint     _sm        = 0;
    uint     _offset    = 0;
    bool     _ready     = false;
    uint8_t  _volume    = 100;

    // ── Internal tone control ────────────────────────────────────────
    void     _toneStart(uint16_t freqHz);
    void     _toneStop();

    // ── Hardware alarm for timed tone / melody sequencing ────────────
    alarm_id_t _alarm = 0;
    void     _cancelAlarm();

    // Timed single tone callback
    static int64_t _timedToneCallback(alarm_id_t id, void* user);

    // ── Melody state (accessed from IRQ context → volatile) ──────────
    volatile const BuzzerNote* _melody    = nullptr;
    volatile uint8_t           _melodyLen = 0;
    volatile uint8_t           _noteIndex = 0;
    volatile bool              _looping   = false;

    // Alarm chain callback for melody sequencing
    static int64_t _melodyCallback(alarm_id_t id, void* user);

    // ── Singleton-per-instance pointer for IRQ callbacks ─────────────
    // Each instance stores itself here during begin(). The static
    // callbacks cast the user_data pointer back to the instance.
    // Thread-safe for single-instance use (typical for a buzzer).
    // For multiple instances, each callback receives its own `this`.

    // ── PIO program (shared across instances, loaded once per block) ─
    static const uint16_t _programInstructions[4];
    static const struct pio_program _program;

    // ── Low-level PIO helpers ────────────────────────────────────────
    void _setFreq(uint16_t freqHz);
    void _setDuty();
    void _stopIrqSafe();
};

#endif // BUZZER_PIO_RP2040_H
