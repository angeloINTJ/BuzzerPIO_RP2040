/**
 * @file    BuzzerPIO_RP2040.cpp
 * @brief   BuzzerPIO v2.5 — Dual-SM AND gate: ultrasonic PWM × tone gate.
 * @version 2.5.0
 * @license MIT
 *
 * SM1 (2 instr): ultrasonic PWM via `set pins` → controls GPIO value.
 * SM2 (2 instr): audio gate via `set pindirs` → controls GPIO output enable.
 * PIO ORs per-SM registers: value = SM1_val, OE = SM2_oe.
 * With GPIO pull-down: output = SM1_pwm AND SM2_gate.
 *
 * Zero DMA. Zero IRQ handlers. Zero FIFO. Pure PIO hardware.
 *
 * v2.5.0 fixes (over v2.4.0):
 *   - begin() protected by critical section (multi-core safety).
 *   - end() atomically sets _ready=false UNDER LOCK before releasing
 *     PIO resources, closing the use-after-free race window.
 *   - add_alarm_in_ms() return value checked — alarm pool exhaustion
 *     handled gracefully (melody aborts, timed tone stops cleanly).
 *   - Volume mapping uses quadratic curve for perceptually linear
 *     loudness (human hearing is logarithmic, not linear).
 *   - SM2 placeholder clkdiv uses int_frac variant (avoids soft-float
 *     code emission on Cortex-M0+).
 *   - Melody completion callback (setMelodyDoneCallback).
 *   - pauseMelody() / resumeMelody() for non-destructive suspension.
 *
 * v2.4.0 fixes (over v2.3.0):
 *   - _timedToneCallback acquires _lock (race condition fix).
 *   - tone(freq) moves _toneStart/_toneStop inside critical section.
 *   - setVolume() acquires _lock for atomic _volume + _setDuty.
 *   - _toneStart() guards on _ready (prevents use-after-end).
 *   - _setFreq() uses uint64_t for overflow-safe divider calculation.
 *   - Volatile cast in _melodyCallback simplified to single const_cast.
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
//
// The critical_section_t is initialized here (not in begin()) because
// move semantics and the destructor need the lock regardless of whether
// begin() was called.
// =========================================================================

BuzzerPIO::BuzzerPIO(uint8_t pin, PIO pio)
    : _pio(pio)
    , _preferredPio(pio)
    , _pin(pin)
    , _pinValid(pin <= BUZZER_PIO_MAX_PIN)
{
    critical_section_init(&_lock);
}

BuzzerPIO::~BuzzerPIO() {
    end();
    critical_section_deinit(&_lock);
}


// =========================================================================
// MOVE SEMANTICS — transfers hardware ownership from another instance
//
// v2.3: The source's pending alarm is cancelled UNDER LOCK before any
// field is copied. This prevents the alarm callback from firing on the
// source after the move has transferred its _melody pointer and other
// state to the destination.
//
// After the move, the source instance is left in a safe "empty" state
// (not ready, no resources held) that can be destroyed without side
// effects.
// =========================================================================

void BuzzerPIO::_moveFrom(BuzzerPIO& other) noexcept {
    // Cancel any pending alarm on the source under ITS lock.
    // This ensures no callback can fire referencing the source's state
    // after we steal it.
    critical_section_enter_blocking(&other._lock);
    if (other._alarm > 0) {
        cancel_alarm(other._alarm);
        other._alarm = 0;
    }
    critical_section_exit(&other._lock);

    // Now safe to copy all fields — no IRQ references the source anymore
    _pio               = other._pio;
    _preferredPio      = other._preferredPio;
    _pin               = other._pin;
    _pinValid          = other._pinValid;
    _smPwm             = other._smPwm;
    _smGate            = other._smGate;
    _offset            = other._offset;
    _ready             = other._ready;
    _volume            = other._volume;
    _alarm             = 0;              // Already cancelled above
    _melody            = other._melody;
    _melodyLen         = other._melodyLen;
    _noteIndex         = other._noteIndex;
    _looping           = other._looping;
    _paused            = other._paused;
    _onMelodyDone      = other._onMelodyDone;
    _melodyDoneUserData = other._melodyDoneUserData;

    // Invalidate the source so its destructor does nothing
    other._ready             = false;
    other._melody            = nullptr;
    other._looping           = false;
    other._paused            = false;
    other._onMelodyDone      = nullptr;
    other._melodyDoneUserData = nullptr;
}

BuzzerPIO::BuzzerPIO(BuzzerPIO&& other) noexcept
    : _pio(pio0)           // Placeholder — _moveFrom overwrites immediately
    , _preferredPio(pio0)
    , _pin(0)
    , _pinValid(false)
{
    // Initialize our own lock before _moveFrom (which uses other._lock)
    critical_section_init(&_lock);
    _moveFrom(other);
}

BuzzerPIO& BuzzerPIO::operator=(BuzzerPIO&& other) noexcept {
    if (this != &other) {
        end();              // Release our current resources first
        _moveFrom(other);
    }
    return *this;
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
    // Check instruction memory availability
    if (!pio_can_add_program(pio, &_program)) return false;
    _offset = pio_add_program(pio, &_program);

    // Claim SM1 (PWM carrier)
    int sm1 = pio_claim_unused_sm(pio, false);
    if (sm1 < 0) {
        pio_remove_program(pio, &_program, _offset);
        return false;
    }

    // Claim SM2 (tone gate) — rollback SM1 on failure
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
//
// v2.5: begin() and end() are now fully protected by the critical section.
//
// begin(): The _ready check-and-set is atomic under lock, preventing two
// cores from simultaneously allocating PIO resources.
//
// end(): Sets _ready=false UNDER LOCK before releasing PIO resources.
// This closes the v2.4 race window where another core could call tone()
// between stopMelody()/noTone() returning and _ready=false being written.
// =========================================================================

bool BuzzerPIO::begin() {
    // v2.5: Acquire lock to make the _ready check-and-set atomic.
    // Prevents double-initialization if two cores call begin() concurrently.
    critical_section_enter_blocking(&_lock);

    if (_ready) {
        critical_section_exit(&_lock);
        return true;
    }

    // Reject invalid GPIO pins early
    if (!_pinValid) {
        critical_section_exit(&_lock);
        return false;
    }

    // ── 1. Auto-probe: try preferred PIO, fallback to the other ──────
    if (!_tryAllocPio(_preferredPio)) {
        PIO fallback = (_preferredPio == pio0) ? pio1 : pio0;
        if (!_tryAllocPio(fallback)) {
            critical_section_exit(&_lock);
            return false;
        }
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

        // v2.5: Use int_frac variant instead of float to avoid soft-float
        // code emission on Cortex-M0+ (no FPU). Placeholder = very slow.
        sm_config_set_clkdiv_int_frac(&cfg, 65535, 0);

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
    critical_section_exit(&_lock);
    return true;
}


/**
 * @brief  Release all PIO resources and silence the buzzer.
 *
 * v2.5: Sets _ready=false atomically UNDER LOCK before releasing any
 * PIO hardware. This eliminates the v2.4 race where another core could
 * call tone() on already-deallocated state machines:
 *
 *   v2.4 (broken):  stopMelody() → noTone() → _freePio() → _ready=false
 *         ↑ another core passes the _ready guard between noTone() and _ready=false
 *
 *   v2.5 (fixed):   lock { cancel alarm, stop SMs, _ready=false } → _freePio()
 *         ↑ alarm callback is cancelled, SMs stopped, then _ready=false blocks new calls
 */
void BuzzerPIO::end() {
    // Atomically disable everything under lock, THEN release hardware.
    // Any concurrent call to tone()/playMelody()/etc. will see
    // _ready==false and bail out immediately.
    critical_section_enter_blocking(&_lock);

    if (!_ready) {
        critical_section_exit(&_lock);
        return;
    }

    // Cancel alarm BEFORE setting _ready=false (alarm callback checks _ready)
    _cancelAlarm();

    // Inline the SM stop operations here instead of calling _toneStop(),
    // because _toneStop() has an `if (!_ready) return` guard that would
    // skip the stop if we set _ready=false first.
    pio_sm_set_enabled(_pio, _smGate, false);
    pio_sm_exec(_pio, _smGate, pio_encode_set(pio_pindirs, 0));

    // NOW mark as not ready — prevents any new PIO access from other cores
    _ready   = false;
    _looping = false;
    _paused  = false;
    _melody  = nullptr;

    critical_section_exit(&_lock);

    // Release PIO resources OUTSIDE the lock (safe — _ready is false,
    // no concurrent code can touch _smPwm/_smGate anymore)
    _freePio();

    // Return GPIO to SIO and force LOW
    gpio_set_function(_pin, GPIO_FUNC_SIO);
    gpio_set_dir(_pin, GPIO_OUT);
    gpio_put(_pin, 0);
}


// =========================================================================
// SINGLE TONE
//
// v2.5: add_alarm_in_ms() return value is now checked. If the alarm pool
// is exhausted (returns 0 or negative), the timed tone is stopped
// immediately to prevent an indefinitely stuck tone.
//
// v2.4: BOTH overloads (with and without duration) now execute
// _toneStart/_toneStop inside the critical section. This eliminates
// a race where two cores calling tone() simultaneously could
// interleave PIO register writes (SM restart, clkdiv set).
//
// v2.3: tone(freq, duration) uses a SINGLE critical section that covers
// both the alarm cancellation and the new alarm scheduling, eliminating
// the window where a stale callback could race with the new alarm.
// =========================================================================

void BuzzerPIO::tone(uint32_t freqHz) {
    if (!_ready) return;

    // Clamp frequency to valid range (0 is a special "silence" value)
    if (freqHz != 0) {
        if (freqHz < BUZZER_PIO_MIN_FREQ) freqHz = BUZZER_PIO_MIN_FREQ;
        if (freqHz > BUZZER_PIO_MAX_FREQ) freqHz = BUZZER_PIO_MAX_FREQ;
    }

    // v2.4: Single critical section covers alarm cancellation, melody
    // state clear, AND the _toneStart/_toneStop call. This prevents
    // two concurrent tone() calls from interleaving PIO register writes.
    critical_section_enter_blocking(&_lock);
    _cancelAlarm();
    _melody  = nullptr;
    _looping = false;
    _paused  = false;

    if (freqHz == 0 || _volume == 0) {
        _toneStop();
    } else {
        _toneStart(freqHz);
    }
    critical_section_exit(&_lock);
}

void BuzzerPIO::tone(uint32_t freqHz, uint16_t durationMs) {
    if (!_ready || durationMs == 0) return;

    // Clamp frequency (same logic as tone(freq))
    if (freqHz != 0) {
        if (freqHz < BUZZER_PIO_MIN_FREQ) freqHz = BUZZER_PIO_MIN_FREQ;
        if (freqHz > BUZZER_PIO_MAX_FREQ) freqHz = BUZZER_PIO_MAX_FREQ;
    }

    // Single critical section: cancel old alarm + clear melody + start
    // tone + schedule new alarm. No window for stale callbacks.
    critical_section_enter_blocking(&_lock);
    _cancelAlarm();
    _melody  = nullptr;
    _looping = false;
    _paused  = false;

    if (freqHz == 0 || _volume == 0) {
        _toneStop();
        critical_section_exit(&_lock);
        return;
    }

    _toneStart(freqHz);

    // v2.5: Check alarm scheduling result. If the alarm pool is
    // exhausted, stop the tone immediately to prevent it from
    // playing indefinitely without a stop callback.
    alarm_id_t id = add_alarm_in_ms(durationMs, _timedToneCallback, this, false);
    if (id > 0) {
        _alarm = id;
    } else {
        // Alarm pool exhausted — stop tone to avoid stuck buzzer
        _stopIrqSafe();
        _alarm = 0;
    }

    critical_section_exit(&_lock);
}

void BuzzerPIO::noTone() {
    if (!_ready) return;

    critical_section_enter_blocking(&_lock);
    _cancelAlarm();
    _melody  = nullptr;
    _looping = false;
    _paused  = false;
    _toneStop();
    critical_section_exit(&_lock);
}

/**
 * @brief  Alarm callback for timed tone — stops tone after duration expires.
 *
 * v2.4: Now acquires _lock before writing _alarm and calling _stopIrqSafe.
 * This fixes a race where tone() from another core could call _toneStart()
 * between _stopIrqSafe() and the _alarm = 0 write, causing the new alarm
 * to be silently overwritten.
 */
int64_t BuzzerPIO::_timedToneCallback(alarm_id_t id, void* user) {
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (!self) return 0;

    // Acquire the lock — consistent with _melodyCallback.
    // critical_section_enter_blocking is safe from IRQ context on the
    // pico SDK (disables interrupts + acquires spinlock).
    critical_section_enter_blocking(&self->_lock);
    self->_stopIrqSafe();
    self->_alarm = 0;
    critical_section_exit(&self->_lock);

    return 0;  // Do not reschedule
}


// =========================================================================
// VOLUME — patches SM1 instruction delays (same mechanism as v1.0)
//
// Note on direct instr_mem access:
//   We write to _pio->instr_mem[] directly because this is the only way
//   to patch instruction delays without stopping+reconfiguring the SM.
//   The pico-sdk does not expose a stable API for this, but the PIO
//   hardware register layout is defined by the RP2040 datasheet and is
//   unlikely to change. This is a conscious trade-off for zero-glitch
//   volume transitions.
//
// v2.5: Volume mapping uses a quadratic curve. The original linear
// mapping (v2.4) mapped volume 1–100 linearly to dH 0–31, but human
// loudness perception is approximately logarithmic (Weber-Fechner law).
// A quadratic approximation yields perceptually even volume steps
// without requiring a lookup table or expensive math.
//
// v2.4: setVolume() acquires _lock so that _volume and _setDuty() are
// atomic w.r.t. IRQ callbacks (e.g. _melodyCallback) that read _volume.
// =========================================================================

/**
 * @brief  Set buzzer volume (0–100).
 *
 * v2.4: Protected by critical section. The _volume write and the
 * _setDuty() PIO patch are now atomic — a melody callback cannot
 * observe _volume mid-change or see a partially patched instruction.
 *
 * @param  volume  0 = mute (gate disabled), 100 = max duty cycle.
 *                 Values > 100 are clamped to 100.
 */
void BuzzerPIO::setVolume(uint8_t volume) {
    uint8_t clamped = (volume > 100) ? 100 : volume;

    critical_section_enter_blocking(&_lock);
    _volume = clamped;
    if (_ready) _setDuty();
    critical_section_exit(&_lock);
}

/**
 * @brief  Patch SM1 instruction delays to set PWM duty cycle.
 *
 *   Instruction [0]: set pins, 1 [dH]  → HIGH for (dH+1) clocks.
 *   Instruction [1]: set pins, 0 [dL]  → LOW  for (dL+1) clocks.
 *   Period = dH + dL + 2.  Duty = (dH+1) / (dH+dL+2).
 *
 *   Volume 100 → dH=31, dL=0  → duty 97% (period=33).
 *   Volume  50 → dH=7,  dL=24 → duty 25% (period=33).   [quadratic]
 *   Volume   1 → dH=0,  dL=31 → duty  3% (period=33).
 *   Volume   0 → SM2 disabled (silence via gate).
 *
 *   v2.5: Quadratic curve — dH = ((vol-1)² × 31) / 99².
 *   This maps volume 50 → dH≈7 instead of the linear dH≈15, giving
 *   perceptually even loudness steps (low volumes are more granular,
 *   high volumes compress — matching human hearing).
 *
 *   Period is always 33 clocks → carrier is fixed at
 *   125MHz / (BUZZER_PWM_CLKDIV × 33) regardless of volume.
 *   At CLKDIV=40: carrier ≈ 94.7 kHz (constant, ultrasonic).
 *
 *   Design note on direct instr_mem patching:
 *   SM1 is briefly disabled (~2 PIO cycles) during the patch to prevent
 *   reading a half-written instruction. Since the carrier is ultrasonic
 *   (~95 kHz), the pause is <25 µs — imperceptible to human hearing.
 *   An alternative double-buffer approach (write to spare instruction
 *   slots, then re-wrap) would eliminate even this micro-pause, but was
 *   deemed unnecessary given the sub-microsecond actual gap and the
 *   additional complexity of managing spare instruction memory.
 */
void BuzzerPIO::_setDuty() {
    if (_volume == 0) {
        // Mute: disable gate SM → OE=0 → pin pulled LOW via pull-down
        pio_sm_set_enabled(_pio, _smGate, false);
        pio_sm_exec(_pio, _smGate, pio_encode_set(pio_pindirs, 0));
        return;
    }

    // v2.5: Quadratic mapping for perceptually linear volume.
    // vol=1 → v=0 → dH=0.  vol=50 → v=49 → dH≈7.  vol=100 → v=99 → dH=31.
    uint32_t v = (uint32_t)(_volume - 1);
    uint8_t dH = (uint8_t)((v * v * 31) / (99u * 99u));

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
// MELODY PLAYBACK — Hardware alarm chain
//
// v2.5: add_alarm_in_ms() failures are now handled. Melody completion
// callback fires for one-shot melodies. pauseMelody()/resumeMelody()
// allow non-destructive suspension.
//
// v2.4: Locking now fully harmonized — every callback and public method
// that touches shared state acquires _lock first. See also v2.3 notes.
//
// v2.3: Uses critical_section_t for multi-core safety. The _melodyCallback
// also acquires the lock, ensuring consistent state even when tone() or
// stopMelody() is called from a different core.
// =========================================================================

/**
 * @brief  Internal: start a melody (one-shot or looping).
 *
 * @param  notes  Pointer to note array (must remain valid during playback).
 * @param  len    Number of notes (1–65535).
 * @param  loop   If true, melody restarts from the beginning after the
 *                last note.
 */
void BuzzerPIO::_startMelody(const BuzzerNote* notes, uint16_t len, bool loop) {
    if (!_ready || !notes || len == 0) return;

    // Atomically cancel any running alarm and set new melody state.
    // The IRQ callback reads _melody, _melodyLen, _noteIndex, _looping —
    // all must be consistent when the first alarm fires.
    critical_section_enter_blocking(&_lock);
    _cancelAlarm();
    _toneStop();

    _melody    = notes;
    _melodyLen = len;
    _noteIndex = 0;
    _looping   = loop;
    _paused    = false;

    if (notes[0].freqHz > 0 && _volume > 0) {
        _toneStart(notes[0].freqHz);
    }

    // v2.5: Check alarm scheduling result. If the alarm pool is
    // exhausted, abort the melody cleanly instead of hanging on the
    // first note forever.
    alarm_id_t id = add_alarm_in_ms(notes[0].durationMs, _melodyCallback, this, false);
    if (id > 0) {
        _alarm = id;
    } else {
        // Alarm pool exhausted — abort melody
        _toneStop();
        _melody = nullptr;
        _alarm  = 0;
    }

    critical_section_exit(&_lock);
}

void BuzzerPIO::playMelody(const BuzzerNote* notes, uint16_t len) {
    _startMelody(notes, len, false);
}

void BuzzerPIO::playMelodyLoop(const BuzzerNote* notes, uint16_t len) {
    _startMelody(notes, len, true);
}

/**
 * @brief  Stop any running melody and silence the buzzer.
 *
 * v2.4: _toneStop() is now called INSIDE the critical section for
 * consistency with the harmonized locking policy. This simplifies
 * the invariant — all PIO state changes happen under lock.
 */
void BuzzerPIO::stopMelody() {
    critical_section_enter_blocking(&_lock);
    _looping = false;
    _paused  = false;
    _cancelAlarm();
    _melody = nullptr;
    _toneStop();
    critical_section_exit(&_lock);
}

/**
 * @brief  Pause a running melody without losing position.
 *
 * v2.5: Cancels the current alarm and silences the buzzer. The note
 * index, melody pointer, and looping flag are preserved so that
 * resumeMelody() can pick up from the next note.
 *
 * @note   If no melody is active or already paused, this is a no-op.
 *         The remaining time of the current note is lost — resume
 *         will start from the NEXT note, not mid-note.
 */
void BuzzerPIO::pauseMelody() {
    critical_section_enter_blocking(&_lock);

    if (!_melody || _paused) {
        critical_section_exit(&_lock);
        return;
    }

    _paused = true;
    _cancelAlarm();
    _toneStop();

    critical_section_exit(&_lock);
}

/**
 * @brief  Resume a previously paused melody from the next note.
 *
 * v2.5: Advances _noteIndex by one (the note that was playing when
 * pause was called is considered consumed) and schedules the alarm
 * chain from the new position. If that advances past the end:
 *   - Looping melody: wraps to index 0.
 *   - One-shot melody: finishes (fires completion callback if set).
 *
 * @note   If no melody was paused, this is a no-op.
 */
void BuzzerPIO::resumeMelody() {
    critical_section_enter_blocking(&_lock);

    if (!_melody || !_paused) {
        critical_section_exit(&_lock);
        return;
    }

    _paused = false;

    // Advance to the next note (the interrupted note is consumed)
    uint16_t nextIdx = _noteIndex + 1;

    if (nextIdx >= _melodyLen) {
        if (_looping) {
            nextIdx = 0;
        } else {
            // Melody was on the last note — it's finished
            _melody = nullptr;
            _alarm  = 0;

            // Fire completion callback if registered
            MelodyDoneCallback cb   = _onMelodyDone;
            void*              data = _melodyDoneUserData;
            critical_section_exit(&_lock);

            if (cb) cb(data);
            return;
        }
    }

    _noteIndex = nextIdx;

    // Cast away volatile for indexed access (safe — we hold the lock)
    const BuzzerNote* melody =
        const_cast<const BuzzerNote*>(_melody);
    const BuzzerNote& note = melody[nextIdx];

    if (note.freqHz > 0 && _volume > 0) {
        _toneStart(note.freqHz);
    }

    // Schedule the alarm for this note's duration
    alarm_id_t id = add_alarm_in_ms(note.durationMs, _melodyCallback, this, false);
    if (id > 0) {
        _alarm = id;
    } else {
        // Alarm pool exhausted — abort
        _toneStop();
        _melody = nullptr;
        _alarm  = 0;
    }

    critical_section_exit(&_lock);
}

/**
 * @brief  Register a callback for melody completion.
 *
 * v2.5: The callback fires from _melodyCallback (IRQ context) when
 * a non-looping melody finishes. It is NOT fired by stopMelody().
 *
 * @param  cb        Function pointer (nullptr to disable).
 * @param  userData  Opaque pointer forwarded to the callback.
 */
void BuzzerPIO::setMelodyDoneCallback(MelodyDoneCallback cb, void* userData) {
    critical_section_enter_blocking(&_lock);
    _onMelodyDone       = cb;
    _melodyDoneUserData = userData;
    critical_section_exit(&_lock);
}

/**
 * @brief  Melody alarm callback — advances to next note or finishes.
 *
 * Runs in IRQ context. Acquires _lock for consistent access to melody
 * state (safe from IRQ — pico SDK critical_section disables interrupts
 * and acquires spinlock).
 *
 * v2.5: Fires MelodyDoneCallback when a one-shot melody finishes.
 *       The callback is invoked AFTER releasing the lock to minimize
 *       time spent with interrupts disabled.
 *
 * v2.4: volatile cast simplified to single const_cast (removing volatile).
 * The double intermediate cast from v2.3 was semantically redundant.
 */
int64_t BuzzerPIO::_melodyCallback(alarm_id_t id, void* user) {
    // Runs in IRQ context — keep it short, register-level ops only.
    BuzzerPIO* self = static_cast<BuzzerPIO*>(user);
    if (!self) return 0;

    // Acquire the lock to ensure consistent reads of melody state.
    // critical_section_enter_blocking is safe from IRQ context on
    // the pico SDK — it disables interrupts and acquires the spinlock.
    critical_section_enter_blocking(&self->_lock);

    if (!self->_melody || self->_paused) {
        critical_section_exit(&self->_lock);
        return 0;
    }

    uint16_t nextIdx = self->_noteIndex + 1;

    if (nextIdx >= self->_melodyLen) {
        if (self->_looping) {
            nextIdx = 0;
        } else {
            // Melody finished — stop and clean up
            self->_stopIrqSafe();
            self->_melody = nullptr;
            self->_alarm  = 0;

            // Capture callback before releasing lock
            MelodyDoneCallback cb   = self->_onMelodyDone;
            void*              data = self->_melodyDoneUserData;

            critical_section_exit(&self->_lock);

            // Fire callback OUTSIDE the lock (minimizes IRQ-off time)
            if (cb) cb(data);
            return 0;
        }
    }

    self->_noteIndex = nextIdx;

    // v2.4: Simplified cast — single const_cast removes volatile.
    // Safe because we hold the lock and Cortex-M0+ has strongly ordered
    // memory. The volatile qualifier prevents compiler caching across
    // function calls; inside this locked region the pointer is stable.
    const BuzzerNote* melody =
        const_cast<const BuzzerNote*>(self->_melody);
    const BuzzerNote& note = melody[nextIdx];

    if (note.freqHz > 0 && self->_volume > 0) {
        // _toneStart re-enables SM1 (PWM) if it was disabled by a
        // preceding silent note's _stopIrqSafe(), and re-muxes GPIO
        // back to PIO function. Critical for melody loop continuity.
        self->_toneStart(note.freqHz);
    } else {
        self->_stopIrqSafe();
    }

    int64_t rescheduleUs = -(int64_t)note.durationMs * 1000;
    critical_section_exit(&self->_lock);

    // Negative return = relative reschedule (µs). Avoids cumulative drift.
    return rescheduleUs;
}


// =========================================================================
// INTERNAL — TONE CONTROL
// =========================================================================

/**
 * @brief  Start a tone: set SM2 clock divider and enable gate.
 *         SM1 (PWM) runs continuously — only SM2 is started/stopped.
 *
 * v2.4: Added _ready guard to prevent access to deallocated PIO
 *        resources if called after end(). Symmetric with _toneStop().
 *
 * @param  freqHz  Frequency in Hz (must be > 0, caller is responsible
 *                 for clamping to [MIN_FREQ, MAX_FREQ]).
 */
void BuzzerPIO::_toneStart(uint32_t freqHz) {
    if (!_ready) return;

    // Ensure GPIO is muxed to PIO (may have been switched to SIO by
    // _stopIrqSafe during a silent note in a melody)
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

    // Switch GPIO to SIO and force LOW to avoid floating pin
    gpio_set_function(_pin, GPIO_FUNC_SIO);
    sio_hw->gpio_oe_set = (1u << _pin);
    sio_hw->gpio_clr    = (1u << _pin);
}

/**
 * @brief  Set SM2 clock divider for the desired tone frequency.
 *         SM2 period = 64 PIO clocks → freq = sys_clk / (clkdiv × 64).
 *
 * v2.4: Uses uint64_t for the intermediate product (freqHz * 64) to
 *        prevent overflow when freqHz > 67,108,863. Also clamps freqHz
 *        to BUZZER_PIO_MAX_FREQ defensively, independent of caller.
 *
 * @param  freqHz  Frequency in Hz (must be > 0).
 */
void BuzzerPIO::_setFreq(uint32_t freqHz) {
    if (freqHz == 0) return;

    // Defensive clamp — ensures overflow-safe arithmetic below even if
    // the caller forgot to clamp. MAX_FREQ * 64 fits in uint64_t.
    if (freqHz > BUZZER_PIO_MAX_FREQ) freqHz = BUZZER_PIO_MAX_FREQ;

    uint32_t sysClk    = clock_get_hz(clk_sys);

    // v2.4: Use uint64_t to prevent overflow on freqHz * 64.
    // At BUZZER_PIO_MAX_FREQ (976000): 976000 * 64 = 62,464,000 — fits
    // in uint32_t, but using uint64_t makes this future-proof for any
    // sys_clk or max_freq combination without silent truncation.
    uint64_t target64  = (uint64_t)freqHz * 64;

    uint32_t divInt    = (uint32_t)(sysClk / target64);
    uint32_t remainder = (uint32_t)(sysClk % target64);
    uint32_t divFrac   = (uint32_t)((uint64_t)remainder * 256 / target64);

    // Clamp divider to hardware limits (should not trigger if freq was
    // clamped, but serves as a safety net)
    if (divInt == 0)     { divInt = 1;     divFrac = 0; }
    if (divInt > 65535)  { divInt = 65535; divFrac = 0; }

    pio_sm_set_clkdiv_int_frac(_pio, _smGate, (uint16_t)divInt, (uint8_t)divFrac);
}

void BuzzerPIO::_cancelAlarm() {
    if (_alarm > 0) {
        cancel_alarm(_alarm);
        _alarm = 0;
    }
}
