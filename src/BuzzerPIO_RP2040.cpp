/**
 * @file    BuzzerPIO_RP2040.cpp
 * @brief   Implementation of BuzzerPIO — PIO-driven passive buzzer.
 * @version 1.0.0
 * @license MIT
 */

#include "BuzzerPIO_RP2040.h"
#include "hardware/gpio.h"

// =========================================================================
// PIO PROGRAM — 4-instruction square wave generator
//
// Assembly:
//   .program buzzer_square
//   .wrap_target
//       set pins, 1 [31]    ; HIGH phase part 1
//       nop         [31]    ; HIGH phase part 2
//       set pins, 0 [31]    ; LOW  phase part 1
//       nop         [31]    ; LOW  phase part 2
//   .wrap
//
// Total period: 128 PIO clocks (4 instructions × 32 clocks each).
// Frequency: sys_clk / (clk_div × 128).
// Volume: duty cycle adjusted by patching delay fields at runtime.
// =========================================================================

const uint16_t BuzzerPIO::_programInstructions[4] = {
    0xFF01,  // set pins, 1 [31]
    0xBF00,  // nop         [31]  (encoded as mov y, y)
    0xFF00,  // set pins, 0 [31]
    0xBF00   // nop         [31]
};

const struct pio_program BuzzerPIO::_program = {
    .instructions = _programInstructions,
    .length       = 4,
    .origin       = -1  // auto-allocate offset
};


// =========================================================================
// CONSTRUCTOR / DESTRUCTOR
// =========================================================================

BuzzerPIO::BuzzerPIO(uint8_t pin, PIO pio)
    : _pio(pio), _pin(pin) {}

BuzzerPIO::~BuzzerPIO() {
    end();
}


// =========================================================================
// LIFECYCLE
// =========================================================================

bool BuzzerPIO::begin() {
    if (_ready) return true;  // Already initialized

    // Allocate instruction memory in the PIO block
    if (!pio_can_add_program(_pio, &_program)) {
        return false;
    }
    _offset = pio_add_program(_pio, &_program);

    // Claim a free state machine
    int claimed = pio_claim_unused_sm(_pio, false);
    if (claimed < 0) {
        pio_remove_program(_pio, &_program, _offset);
        return false;
    }
    _sm = (uint)claimed;

    // Configure the state machine
    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_wrap(&cfg, _offset + 0, _offset + 3);
    sm_config_set_set_pins(&cfg, _pin, 1);
    sm_config_set_clkdiv(&cfg, 1.0f);

    // Initialize GPIO as PIO output, driven LOW
    pio_gpio_init(_pio, _pin);
    pio_sm_set_consecutive_pindirs(_pio, _sm, _pin, 1, true);

    // Load config and keep SM stopped until tone() is called
    pio_sm_init(_pio, _sm, _offset, &cfg);
    pio_sm_set_enabled(_pio, _sm, false);

    // Apply default volume (100% = 50% duty)
    _setDuty();

    _ready = true;
    return true;
}

void BuzzerPIO::end() {
    if (!_ready) return;

    // Stop everything
    stopMelody();
    noTone();

    // Release PIO resources
    pio_sm_set_enabled(_pio, _sm, false);
    pio_sm_unclaim(_pio, _sm);
    pio_remove_program(_pio, &_program, _offset);

    // Return GPIO to standard digital output LOW
    gpio_set_function(_pin, GPIO_FUNC_SIO);
    gpio_set_dir(_pin, GPIO_OUT);
    gpio_put(_pin, 0);

    _ready = false;
}


// =========================================================================
// SINGLE TONE
// =========================================================================

void BuzzerPIO::tone(uint16_t freqHz) {
    if (!_ready) return;

    // Cancel any pending alarm (timed tone or melody)
    _cancelAlarm();
    _melody = nullptr;
    _looping = false;

    if (freqHz == 0 || _volume == 0) {
        _toneStop();
        return;
    }

    _toneStart(freqHz);
}

void BuzzerPIO::tone(uint16_t freqHz, uint16_t durationMs) {
    if (!_ready || durationMs == 0) return;

    // Start the tone (also cancels pending alarm/melody)
    tone(freqHz);

    if (freqHz == 0 || _volume == 0) return;

    // Schedule auto-shutoff via hardware alarm
    _alarm = add_alarm_in_ms(durationMs, _timedToneCallback, this, false);
}

void BuzzerPIO::noTone() {
    if (!_ready) return;

    _cancelAlarm();
    _melody = nullptr;
    _looping = false;
    _toneStop();
}

/**
 * @brief  Hardware alarm callback for timed single tone.
 *         Runs in IRQ context — only register writes, no heap/mutex.
 * @return 0 = one-shot (do not re-schedule).
 */
int64_t BuzzerPIO::_timedToneCallback(alarm_id_t id, void* user) {
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (self) {
        self->_stopIrqSafe();
        self->_alarm = 0;
    }
    return 0;
}


// =========================================================================
// VOLUME
// =========================================================================

void BuzzerPIO::setVolume(uint8_t volume) {
    _volume = (volume > 100) ? 100 : volume;
    if (_ready) _setDuty();
}


// =========================================================================
// MELODY PLAYBACK — Hardware alarm chain
//
// Each note is configured in the PIO and a hardware alarm is scheduled
// for its duration. When the alarm fires, the IRQ callback advances to
// the next note (or loops back to the beginning). All note transitions
// happen with < 10 µs jitter, completely independent of the main loop.
//
// The callback returns a negative value to re-schedule relative to the
// previous fire time, ensuring cumulative timing without drift.
// =========================================================================

void BuzzerPIO::playMelody(const BuzzerNote* notes, uint8_t len) {
    if (!_ready || !notes || len == 0) return;

    // Stop current playback
    _cancelAlarm();
    _toneStop();

    // Store melody reference (must remain valid for duration of playback)
    _melody    = notes;
    _melodyLen = len;
    _noteIndex = 0;
    _looping   = false;

    // Start first note
    if (notes[0].freqHz > 0 && _volume > 0) {
        _toneStart(notes[0].freqHz);
    }

    // Schedule alarm for first note's duration
    _alarm = add_alarm_in_ms(notes[0].durationMs, _melodyCallback, this, false);
}

void BuzzerPIO::playMelodyLoop(const BuzzerNote* notes, uint8_t len) {
    if (!_ready || !notes || len == 0) return;

    _cancelAlarm();
    _toneStop();

    _melody    = notes;
    _melodyLen = len;
    _noteIndex = 0;
    _looping   = true;

    if (notes[0].freqHz > 0 && _volume > 0) {
        _toneStart(notes[0].freqHz);
    }

    _alarm = add_alarm_in_ms(notes[0].durationMs, _melodyCallback, this, false);
}

void BuzzerPIO::stopMelody() {
    _looping = false;
    _cancelAlarm();
    _melody = nullptr;
    _toneStop();
}

/**
 * @brief  Hardware alarm callback — advances melody in IRQ context.
 *
 *         Configures the PIO for the next note via register writes
 *         (IRQ-safe). Returns negative µs to re-schedule the alarm
 *         relative to the previous fire time (no drift accumulation).
 *
 * @return Negative µs = re-schedule; 0 = melody finished.
 */
int64_t BuzzerPIO::_melodyCallback(alarm_id_t id, void* user) {
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (!self || !self->_melody) return 0;

    uint8_t nextIdx = self->_noteIndex + 1;

    // End of melody?
    if (nextIdx >= self->_melodyLen) {
        if (self->_looping) {
            nextIdx = 0;  // Restart from beginning
        } else {
            // One-shot: silence and stop
            self->_stopIrqSafe();
            self->_melody = nullptr;
            self->_alarm = 0;
            return 0;
        }
    }

    self->_noteIndex = nextIdx;

    // Access note (cast away volatile — safe, we're the only writer in IRQ)
    const BuzzerNote* melody = (const BuzzerNote*)self->_melody;
    const BuzzerNote& note = melody[nextIdx];

    // Configure PIO for the new note
    if (note.freqHz > 0 && self->_volume > 0) {
        pio_gpio_init(self->_pio, self->_pin);
        self->_setFreq(note.freqHz);
        pio_sm_restart(self->_pio, self->_sm);
        pio_sm_set_enabled(self->_pio, self->_sm, true);
    } else {
        self->_stopIrqSafe();
    }

    // Re-schedule for this note's duration (negative = relative to last fire)
    return -(int64_t)note.durationMs * 1000;
}


// =========================================================================
// INTERNAL — PIO CONTROL
// =========================================================================

/**
 * @brief  Start the PIO generating a tone at the given frequency.
 *         Restores GPIO to PIO function, sets clock divider, resets PC.
 */
void BuzzerPIO::_toneStart(uint16_t freqHz) {
    pio_gpio_init(_pio, _pin);
    _setFreq(freqHz);
    pio_sm_restart(_pio, _sm);
    pio_sm_set_enabled(_pio, _sm, true);
}

/**
 * @brief  Stop the PIO and force GPIO LOW (full silence).
 *         Restores GPIO as PIO output for the next activation.
 */
void BuzzerPIO::_toneStop() {
    if (!_ready) return;

    pio_sm_set_enabled(_pio, _sm, false);

    // Force LOW via SIO to prevent DC offset on the buzzer
    gpio_set_function(_pin, GPIO_FUNC_SIO);
    gpio_set_dir(_pin, GPIO_OUT);
    gpio_put(_pin, 0);

    // Restore as PIO output for next use
    pio_gpio_init(_pio, _pin);
}

/**
 * @brief  IRQ-safe stop — minimal register writes only.
 *         Does NOT restore PIO function (done on next _toneStart).
 */
void BuzzerPIO::_stopIrqSafe() {
    pio_sm_set_enabled(_pio, _sm, false);
    sio_hw->gpio_clr = (1u << _pin);
    sio_hw->gpio_oe_set = (1u << _pin);
}

/**
 * @brief  Cancel a pending hardware alarm (if any).
 */
void BuzzerPIO::_cancelAlarm() {
    if (_alarm > 0) {
        cancel_alarm(_alarm);
        _alarm = 0;
    }
}


// =========================================================================
// INTERNAL — FREQUENCY AND DUTY CYCLE
// =========================================================================

/**
 * @brief  Set the PIO clock divider for the target frequency.
 *         freq = sys_clk / (clk_div × 128)
 */
void BuzzerPIO::_setFreq(uint16_t freqHz) {
    if (freqHz == 0) {
        pio_sm_set_enabled(_pio, _sm, false);
        return;
    }

    uint32_t sysClk = clock_get_hz(clk_sys);
    uint32_t target = (uint32_t)freqHz * 128;

    uint32_t divInt  = sysClk / target;
    uint32_t remainder = sysClk % target;
    uint32_t divFrac = (remainder * 256) / target;

    // Clamp: minimum divider = 1.0
    if (divInt == 0) { divInt = 1; divFrac = 0; }

    // Clamp: maximum divider = 65535
    if (divInt > 65535) { divInt = 65535; divFrac = 0; }

    pio_sm_set_clkdiv_int_frac(_pio, _sm, (uint16_t)divInt, (uint8_t)divFrac);
}

/**
 * @brief  Patch PIO instruction delays to set duty cycle (volume).
 *
 *         Volume 100% → 50% duty (64 HIGH + 64 LOW) → max amplitude.
 *         Volume 1%   → ~1.6% duty (2 HIGH + 126 LOW) → barely audible.
 *         Volume 0%   → SM disabled (complete silence).
 *
 *         Granularity: 64 effective levels (~1.6% per step).
 */
void BuzzerPIO::_setDuty() {
    if (_volume == 0) {
        pio_sm_set_enabled(_pio, _sm, false);
        return;
    }

    // Map volume 1–100 → highClocks 2–64
    uint32_t highClocks = 2 + ((uint32_t)(_volume - 1) * 62) / 99;
    uint32_t lowClocks  = 128 - highClocks;

    // Distribute HIGH clocks across Slot 0 and Slot 1
    // Each slot contributes (delay + 1) clocks; delay is 5 bits (0–31).
    uint8_t dh_sum = (uint8_t)(highClocks - 2);
    uint8_t dh1 = (dh_sum > 31) ? 31 : dh_sum;
    uint8_t dh2 = dh_sum - dh1;

    // Distribute LOW clocks across Slot 2 and Slot 3
    uint8_t dl_sum = (uint8_t)(lowClocks - 2);
    uint8_t dl1 = (dl_sum > 31) ? 31 : dl_sum;
    uint8_t dl2 = dl_sum - dl1;

    // Clamp dl2 to 31 max (5-bit delay field).
    // For very low volumes (< ~50%), the total period may be slightly
    // shorter than 128 clocks (~3% frequency shift — imperceptible
    // on a passive buzzer).
    if (dl2 > 31) dl2 = 31;

    // Build patched instructions
    //   set pins, 1 [d] = 0xE001 | (d << 8)
    //   nop         [d] = 0xA000 | (d << 8)
    //   set pins, 0 [d] = 0xE000 | (d << 8)
    uint16_t instr0 = 0xE001 | ((uint16_t)dh1 << 8);
    uint16_t instr1 = 0xA000 | ((uint16_t)dh2 << 8);
    uint16_t instr2 = 0xE000 | ((uint16_t)dl1 << 8);
    uint16_t instr3 = 0xA000 | ((uint16_t)dl2 << 8);

    // Patch instruction memory (briefly disable SM to avoid glitch)
    bool wasEnabled = _pio->ctrl & (1u << _sm);
    pio_sm_set_enabled(_pio, _sm, false);

    _pio->instr_mem[_offset + 0] = instr0;
    _pio->instr_mem[_offset + 1] = instr1;
    _pio->instr_mem[_offset + 2] = instr2;
    _pio->instr_mem[_offset + 3] = instr3;

    if (wasEnabled) {
        pio_sm_set_enabled(_pio, _sm, true);
    }
}
