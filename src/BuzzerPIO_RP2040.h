/**
 * @file    BuzzerPIO_RP2040.h
 * @brief   Ultrasonic PWM tone generation using dual PIO state machines.
 * @version 2.1.0
 * @license MIT
 *
 * @details
 *   **Dual-SM AND gate architecture:**
 *
 *   SM1 (PWM carrier): generates ultrasonic PWM (~50–120 kHz) whose duty
 *   cycle sets the equivalent amplitude (volume). Outputs via `set pins`
 *   → controls GPIO VALUE register.
 *
 *   SM2 (tone gate): square wave at the audible frequency. Outputs via
 *   `set pindirs` → controls GPIO OUTPUT ENABLE register.
 *
 *   The RP2040 PIO combines per-SM outputs with OR:
 *     final_value = SM1_value | 0      = SM1 PWM
 *     final_OE    = 0        | SM2_oe  = SM2 gate
 *
 *   With GPIO pull-down enabled:
 *     OE=1 → pin outputs SM1 PWM (buzzer hears ultrasonic carrier)
 *     OE=0 → pin hi-Z → pulled LOW (silence)
 *
 *   **Result: output = SM1_pwm AND SM2_gate** — amplitude-modulated tone.
 *
 *   - 4 PIO instructions (fits in 5 free slots of either block)
 *   - 2 PIO state machines (auto-probed, same block)
 *   - Zero DMA, zero IRQ handlers, zero FIFO
 *   - Volume = duty cycle patching (same proven mechanism as v1.0)
 *   - Frequency = clock divider (same proven mechanism as v1.0)
 *   - Melody = hardware alarm chain (same proven mechanism as v1.0)
 *
 * @par Resources
 *   - 2 PIO state machines + 4 instruction slots (auto-probed)
 *   - 1 hardware alarm (melody sequencing)
 *   - ~40 bytes RAM per instance
 *   - 0 DMA channels
 */

#ifndef BUZZER_PIO_RP2040_H
#define BUZZER_PIO_RP2040_H

#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/time.h"

// =========================================================================
// Configuration
// =========================================================================

/** SM1 (PWM) clock divider. Controls ultrasonic carrier frequency.
 *  Carrier = sys_clk / (CLKDIV × 33) ≈ 94.7 kHz at CLKDIV=40.
 *  The carrier is CONSTANT regardless of volume (period fixed at 33). */
#ifndef BUZZER_PWM_CLKDIV
#define BUZZER_PWM_CLKDIV 40
#endif

#define BUZZER_PIO_MAX_FREQ 976000
#define BUZZER_PIO_MIN_FREQ 15

// =========================================================================
// Types
// =========================================================================

struct BuzzerNote {
    uint16_t freqHz;
    uint16_t durationMs;
};

// =========================================================================
// Class
// =========================================================================

class BuzzerPIO {
public:
    explicit BuzzerPIO(uint8_t pin, PIO pio = pio0);
    ~BuzzerPIO();

    BuzzerPIO(const BuzzerPIO&) = delete;
    BuzzerPIO& operator=(const BuzzerPIO&) = delete;

    bool begin();
    void end();
    bool isReady() const { return _ready; }

    void tone(uint16_t freqHz);
    void tone(uint16_t freqHz, uint16_t durationMs);
    void noTone();

    void    setVolume(uint8_t volume);
    uint8_t getVolume() const { return _volume; }

    void playMelody(const BuzzerNote* notes, uint8_t len);
    void playMelodyLoop(const BuzzerNote* notes, uint8_t len);
    void stopMelody();
    bool isPlaying() const { return _melody != nullptr; }
    bool isLooping()  const { return _looping; }

    PIO getActivePio() const { return _pio; }

private:
    PIO      _pio;
    PIO      _preferredPio;
    uint8_t  _pin;
    uint     _smPwm  = 0;    ///< SM1: ultrasonic PWM (value)
    uint     _smGate = 0;    ///< SM2: audio frequency gate (OE)
    uint     _offset = 0;
    bool     _ready  = false;
    uint8_t  _volume = 100;

    bool _tryAllocPio(PIO pio);
    void _freePio();

    void _toneStart(uint16_t freqHz);
    void _toneStop();
    void _stopIrqSafe();
    void _setDuty();
    void _setFreq(uint16_t freqHz);

    alarm_id_t _alarm = 0;
    void _cancelAlarm();
    static int64_t _timedToneCallback(alarm_id_t id, void* user);

    volatile const BuzzerNote* _melody    = nullptr;
    volatile uint8_t           _melodyLen = 0;
    volatile uint8_t           _noteIndex = 0;
    volatile bool              _looping   = false;
    static int64_t _melodyCallback(alarm_id_t id, void* user);

    static const uint16_t      _programInstructions[4];
    static const struct pio_program _program;
};

#endif // BUZZER_PIO_RP2040_H
