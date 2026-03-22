/**
 * @file    BuzzerPIO_RP2040.cpp
 * @brief   BuzzerPIO v2.1 — Dual-SM AND gate: ultrasonic PWM × tone gate.
 * @version 2.1.0
 * @license MIT
 *
 * SM1 (2 instr): ultrasonic PWM via `set pins` → controls GPIO value.
 * SM2 (2 instr): audio gate via `set pindirs` → controls GPIO output enable.
 * PIO ORs per-SM registers: value = SM1_val, OE = SM2_oe.
 * With GPIO pull-down: output = SM1_pwm AND SM2_gate.
 *
 * Zero DMA. Zero IRQ handlers. Zero FIFO. Pure PIO hardware.
 */

#include "BuzzerPIO_RP2040.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"


// =========================================================================
// PIO PROGRAM — 4 instructions, two sub-programs in shared memory
//
// SM1 wraps [0..1] — Ultrasonic PWM carrier (controls amplitude/volume):
//   [0] set pins, 1  [dH]   ; HIGH for (dH+1) clocks — patched for volume
//   [1] set pins, 0  [31]   ; LOW  for 32 clocks     — fixed
//
//   Period = 33 + dH.  At dH=31: period=64 → 50% duty (max volume).
//   At dH=0: period=33 → 3% duty (min volume).
//   Carrier = sys_clk / (SM1_clkdiv × period) → always ultrasonic.
//
// SM2 wraps [2..3] — Audio frequency tone gate (on/off at tone freq):
//   [2] set pindirs, 1  [31]  ; OE=1 for 32 clocks (buzzer hears PWM)
//   [3] set pindirs, 0  [31]  ; OE=0 for 32 clocks (buzzer sees LOW)
//
//   Period = 64 clocks.  Tone freq = sys_clk / (SM2_clkdiv × 64).
//   SM2 only controls OE — it never touches the pin value.
//
// AND gate via hardware:
//   SM1 sets pins VALUE but never touches OE → SM1_oe stays 0.
//   SM2 sets PINDIRS (OE) but never touches value → SM2_val stays 0.
//   PIO combines with OR: final_val = SM1_val, final_oe = SM2_oe.
//   GPIO pull-down ensures pin = LOW when OE=0.
//   Result: pin = SM1_pwm when SM2_gate=HIGH, else LOW.
// =========================================================================

const uint16_t BuzzerPIO::_programInstructions[4] = {
    // SM1: ultrasonic PWM (value control)
    0xFF01,  // [0] set pins, 1 [31]  — HIGH phase (delay patched at runtime)
    0xFF00,  // [1] set pins, 0 [31]  — LOW phase  (delay fixed at 31)

    // SM2: audio frequency gate (OE control)
    0xFF81,  // [2] set pindirs, 1 [31]  — gate OPEN
    0xFF80   // [3] set pindirs, 0 [31]  — gate CLOSED
};

const struct pio_program BuzzerPIO::_program = {
    .instructions = _programInstructions,
    .length       = 4,
    .origin       = -1
};


// =========================================================================
// CONSTRUCTOR / DESTRUCTOR
// =========================================================================

BuzzerPIO::BuzzerPIO(uint8_t pin, PIO pio)
    : _pio(pio), _preferredPio(pio), _pin(pin) {}

BuzzerPIO::~BuzzerPIO() {
    end();
}


// =========================================================================
// PIO ALLOCATION — needs 4 instruction slots + 2 state machines
//
// IMPORTANT: Both SMs MUST be in the same PIO block. The RP2040 ORs
// per-SM outputs (value, OE) within a single PIO block. SMs in different
// blocks cannot interact — their outputs go to independent GPIO muxes.
// The auto-probe tries the preferred block first, then falls back to
// the other. It NEVER splits SMs across blocks.
// =========================================================================

bool BuzzerPIO::_tryAllocPio(PIO pio) {
    // Check instruction memory
    if (!pio_can_add_program(pio, &_program)) return false;
    _offset = pio_add_program(pio, &_program);

    // Claim SM1 (PWM carrier)
    int sm1 = pio_claim_unused_sm(pio, false);
    if (sm1 < 0) {
        pio_remove_program(pio, &_program, _offset);
        return false;
    }

    // Claim SM2 (tone gate)
    int sm2 = pio_claim_unused_sm(pio, false);
    if (sm2 < 0) {
        pio_sm_unclaim(pio, sm1);
        pio_remove_program(pio, &_program, _offset);
        return false;
    }

    _pio    = pio;
    _smPwm  = (uint)sm1;
    _smGate = (uint)sm2;
    return true;
}

void BuzzerPIO::_freePio() {
    pio_sm_set_enabled(_pio, _smPwm, false);
    pio_sm_set_enabled(_pio, _smGate, false);
    pio_sm_unclaim(_pio, _smPwm);
    pio_sm_unclaim(_pio, _smGate);
    pio_remove_program(_pio, &_program, _offset);
}


// =========================================================================
// LIFECYCLE
// =========================================================================

bool BuzzerPIO::begin() {
    if (_ready) return true;

    // ── 1. Auto-probe: try preferred PIO, fallback to the other ──────
    if (!_tryAllocPio(_preferredPio)) {
        PIO fallback = (_preferredPio == pio0) ? pio1 : pio0;
        if (!_tryAllocPio(fallback)) return false;
    }

    // ── 2. Configure SM1 (PWM carrier) ───────────────────────────────
    //   Wraps over instructions [0..1]. Controls pin VALUE via set pins.
    //   Clock divider fixed → ultrasonic carrier frequency.
    //   OE is NEVER set by SM1 → SM1_oe stays 0 after pio_sm_init.
    {
        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, _offset + 0, _offset + 1);
        sm_config_set_set_pins(&cfg, _pin, 1);
        sm_config_set_clkdiv_int_frac(&cfg, BUZZER_PWM_CLKDIV, 0);

        // Init SM1 — this resets output value and OE to 0 for all pins
        pio_sm_init(_pio, _smPwm, _offset + 0, &cfg);
        // Deliberately NOT calling pio_sm_set_consecutive_pindirs for SM1.
        // SM1's OE for the buzzer pin stays 0 → no direct output to pad.
        // SM1 only provides the VALUE; SM2 gates it via OE.
    }

    // ── 3. Configure SM2 (tone gate) ─────────────────────────────────
    //   Wraps over instructions [2..3]. Controls pin OE via set pindirs.
    //   Clock divider dynamic → audio frequency.
    //   VALUE is NEVER set by SM2 → SM2_val stays 0 after pio_sm_init.
    {
        pio_sm_config cfg = pio_get_default_sm_config();
        sm_config_set_wrap(&cfg, _offset + 2, _offset + 3);
        sm_config_set_set_pins(&cfg, _pin, 1);
        sm_config_set_clkdiv(&cfg, 65535.0f);  // Placeholder (very slow)

        // Init SM2 — output value and OE reset to 0
        pio_sm_init(_pio, _smGate, _offset + 2, &cfg);
        // SM2 will control OE via `set pindirs` instructions.
    }

    // ── 4. GPIO: mux to PIO + enable pull-down ───────────────────────
    //   Pull-down ensures pin = LOW when OE=0 (gate closed).
    pio_gpio_init(_pio, _pin);
    gpio_pull_down(_pin);

    // ── 5. Apply default volume (patches SM1 instruction delays) ─────
    _setDuty();

    // ── 6. Start SM1 (PWM runs continuously — inaudible without gate)
    pio_sm_set_enabled(_pio, _smPwm, true);

    // SM2 stays disabled until tone() or playMelody() is called.

    _ready = true;
    return true;
}


void BuzzerPIO::end() {
    if (!_ready) return;

    stopMelody();
    noTone();
    _freePio();

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

    _cancelAlarm();
    _melody  = nullptr;
    _looping = false;

    if (freqHz == 0 || _volume == 0) {
        _toneStop();
        return;
    }
    _toneStart(freqHz);
}

void BuzzerPIO::tone(uint16_t freqHz, uint16_t durationMs) {
    if (!_ready || durationMs == 0) return;

    tone(freqHz);
    if (freqHz == 0 || _volume == 0) return;

    _alarm = add_alarm_in_ms(durationMs, _timedToneCallback, this, false);
}

void BuzzerPIO::noTone() {
    if (!_ready) return;

    _cancelAlarm();
    _melody  = nullptr;
    _looping = false;
    _toneStop();
}

int64_t BuzzerPIO::_timedToneCallback(alarm_id_t id, void* user) {
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (self) {
        self->_stopIrqSafe();
        self->_alarm = 0;
    }
    return 0;
}


// =========================================================================
// VOLUME — patches SM1 instruction delays (same mechanism as v1.0)
// =========================================================================

void BuzzerPIO::setVolume(uint8_t volume) {
    _volume = (volume > 100) ? 100 : volume;
    if (_ready) _setDuty();
}

/**
 * @brief  Patch SM1 instruction delays to set PWM duty cycle.
 *
 *   Instruction [0]: set pins, 1 [dH]  → HIGH for (dH+1) clocks.
 *   Instruction [1]: set pins, 0 [dL]  → LOW  for (dL+1) clocks.
 *   Period = dH + dL + 2.  Duty = (dH+1) / (dH+dL+2).
 *
 *   Volume 100 → dH=31, dL=0  → duty 97% (period=33).
 *   Volume  50 → dH=15, dL=16 → duty 50% (period=33).
 *   Volume   1 → dH=0,  dL=31 → duty  3% (period=33).
 *   Volume   0 → SM2 disabled (silence via gate).
 *
 *   Period is always 33 clocks → carrier is fixed at
 *   125MHz / (BUZZER_PWM_CLKDIV × 33) regardless of volume.
 *   At CLKDIV=40: carrier ≈ 94.7 kHz (constant, ultrasonic).
 */
void BuzzerPIO::_setDuty() {
    if (_volume == 0) {
        // Mute: disable gate SM → OE=0 → pin pulled LOW via pull-down
        pio_sm_set_enabled(_pio, _smGate, false);
        pio_sm_exec(_pio, _smGate, pio_encode_set(pio_pindirs, 0));
        return;
    }

    // Map volume 1–100 → dH 0–31 (HIGH delay field)
    uint8_t dH = (uint8_t)(((uint32_t)(_volume - 1) * 31) / 99);
    // LOW delay = complement → total delay fields always sum to 31
    uint8_t dL = 31 - dH;

    // Build patched instructions:
    //   set pins, 1 [dH] = 0xE001 | (dH << 8)
    //   set pins, 0 [dL] = 0xE000 | (dL << 8)
    uint16_t instr0 = 0xE001 | ((uint16_t)dH << 8);
    uint16_t instr1 = 0xE000 | ((uint16_t)dL << 8);

    // Patch SM1 instruction memory (briefly disable to avoid glitch)
    bool wasEnabled = _pio->ctrl & (1u << _smPwm);
    pio_sm_set_enabled(_pio, _smPwm, false);

    _pio->instr_mem[_offset + 0] = instr0;
    _pio->instr_mem[_offset + 1] = instr1;

    if (wasEnabled) {
        pio_sm_set_enabled(_pio, _smPwm, true);
    }
}


// =========================================================================
// MELODY PLAYBACK — Hardware alarm chain (identical to v1.0)
// =========================================================================

void BuzzerPIO::playMelody(const BuzzerNote* notes, uint8_t len) {
    if (!_ready || !notes || len == 0) return;

    _cancelAlarm();
    _toneStop();

    _melody    = notes;
    _melodyLen = len;
    _noteIndex = 0;
    _looping   = false;

    if (notes[0].freqHz > 0 && _volume > 0) {
        _toneStart(notes[0].freqHz);
    }
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

int64_t BuzzerPIO::_melodyCallback(alarm_id_t id, void* user) {
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (!self || !self->_melody) return 0;

    uint8_t nextIdx = self->_noteIndex + 1;

    if (nextIdx >= self->_melodyLen) {
        if (self->_looping) {
            nextIdx = 0;
        } else {
            self->_stopIrqSafe();
            self->_melody = nullptr;
            self->_alarm  = 0;
            return 0;
        }
    }

    self->_noteIndex = nextIdx;

    const BuzzerNote* melody = (const BuzzerNote*)self->_melody;
    const BuzzerNote& note   = melody[nextIdx];

    if (note.freqHz > 0 && self->_volume > 0) {
        // _toneStart re-enables SM1 (PWM) if it was disabled by a
        // preceding silent note's _stopIrqSafe(), and re-muxes GPIO
        // back to PIO function. Critical for melody loop continuity.
        self->_toneStart(note.freqHz);
    } else {
        self->_stopIrqSafe();
    }

    return -(int64_t)note.durationMs * 1000;
}


// =========================================================================
// INTERNAL — TONE CONTROL
// =========================================================================

/**
 * @brief  Start a tone: set SM2 clock divider and enable gate.
 *         SM1 (PWM) runs continuously — only SM2 is started/stopped.
 */
void BuzzerPIO::_toneStart(uint16_t freqHz) {
    // Ensure GPIO is muxed to PIO
    pio_gpio_init(_pio, _pin);

    // Ensure SM1 (PWM) is running
    if (!(_pio->ctrl & (1u << _smPwm))) {
        pio_sm_restart(_pio, _smPwm);
        pio_sm_set_enabled(_pio, _smPwm, true);
    }

    // Set SM2 frequency and start gate
    _setFreq(freqHz);
    pio_sm_restart(_pio, _smGate);
    pio_sm_set_enabled(_pio, _smGate, true);
}

/**
 * @brief  Stop tone: disable SM2 gate, force OE=0.
 *         SM1 (PWM) keeps running (inaudible without gate).
 */
void BuzzerPIO::_toneStop() {
    if (!_ready) return;

    pio_sm_set_enabled(_pio, _smGate, false);
    // Force OE=0 so pin goes to pull-down (LOW)
    pio_sm_exec(_pio, _smGate, pio_encode_set(pio_pindirs, 0));
}

/**
 * @brief  IRQ-safe stop — disable both SMs, switch GPIO to SIO, force LOW.
 *         Only register writes — safe from alarm callbacks.
 */
void BuzzerPIO::_stopIrqSafe() {
    pio_sm_set_enabled(_pio, _smGate, false);
    pio_sm_set_enabled(_pio, _smPwm, false);

    // Switch GPIO to SIO and force LOW
    gpio_set_function(_pin, GPIO_FUNC_SIO);
    sio_hw->gpio_oe_set = (1u << _pin);
    sio_hw->gpio_clr    = (1u << _pin);
}

/**
 * @brief  Set SM2 clock divider for the desired tone frequency.
 *         SM2 period = 64 PIO clocks → freq = sys_clk / (clkdiv × 64).
 */
void BuzzerPIO::_setFreq(uint16_t freqHz) {
    if (freqHz == 0) return;

    uint32_t sysClk    = clock_get_hz(clk_sys);
    uint32_t target    = (uint32_t)freqHz * 64;

    uint32_t divInt    = sysClk / target;
    uint32_t remainder = sysClk % target;
    uint32_t divFrac   = (remainder * 256) / target;

    if (divInt == 0)     { divInt = 1; divFrac = 0; }
    if (divInt > 65535)  { divInt = 65535; divFrac = 0; }

    pio_sm_set_clkdiv_int_frac(_pio, _smGate, (uint16_t)divInt, (uint8_t)divFrac);
}

void BuzzerPIO::_cancelAlarm() {
    if (_alarm > 0) {
        cancel_alarm(_alarm);
        _alarm = 0;
    }
}
