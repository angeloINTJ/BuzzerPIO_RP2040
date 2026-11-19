/**
 * @file    BuzzerPIO_RP2040.h
 * @brief   Ultrasonic PWM tone generation using dual PIO state machines.
 * @version 2.5.2
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
 *   Between tones the pin is off the PIO, on SIO and driven LOW; both
 *   state machines keep running (v2.5.2).
 *
 *   **Result: output = SM1_pwm AND SM2_gate** — amplitude-modulated tone.
 *
 *   - 4 PIO instructions (fits in 4 free slots of either block)
 *   - 2 PIO state machines (auto-probed, same block)
 *   - Zero DMA, zero IRQ handlers, zero FIFO
 *   - Volume = duty cycle patching (same proven mechanism as v1.0)
 *   - Frequency = clock divider (same proven mechanism as v1.0)
 *   - Melody = hardware alarm chain (same proven mechanism as v1.0)
 *
 * @par Resources
 *   - 2 PIO state machines + 4 instruction slots (auto-probed)
 *   - 1 hardware alarm (melody sequencing)
 *   - 1 spinlock (via critical_section_t, for multi-core safety)
 *   - ~56 bytes RAM per instance
 *   - 0 DMA channels
 *
 * @par Thread safety
 *   All public methods are safe to call from either core or from
 *   IRQ context (alarm callbacks). Shared state is protected by
 *   a pico-sdk `critical_section_t`, which uses a hardware spinlock
 *   and disables interrupts on the calling core — correct for both
 *   single-core and dual-core RP2040 usage.
 *
 * @par Changelog (v2.5.2)
 *   - A note is no longer switched off by another driver of the same
 *     PIO block. The four SM_ENABLE bits of a block share its CTRL
 *     register, and pio_sm_set_enabled() reads CTRL, changes one bit and
 *     writes it back. Up to v2.5.1 this library switched SM_ENABLE at
 *     note boundaries: SM2 was enabled at every note's start, and both
 *     state machines were disabled in a melody's rests and at its end.
 *     A driver interrupted by a note's alarm between its read and its
 *     write then wrote the old value back, with this library's bits
 *     cleared: the note that had just started stopped after one burst
 *     or none, its pin frozen HIGH or LOW until the next note. Both
 *     state machines are now enabled in begin(), through CTRL's atomic
 *     set alias, and run until end(). After begin() returns, nothing
 *     here reads CTRL or clears SM_ENABLE, so an old value written back
 *     is the value already there; each note's start writes the two bits
 *     again through the same alias, which changes nothing unless they
 *     had been cleared. A tone is switched by the pin's mux instead: on
 *     the PIO while it sounds, on SIO and driven LOW when it is silent.
 *     Measured on a Pico W (2026-11-18) whose pio1 also runs the CYW43
 *     radio's SPI and a DHT22, with a looping beep of 2200 Hz, 80 ms on
 *     and 80 ms off, heard by a second board on the buzzer pin. v2.5.1
 *     lost 6 beeps of 3,420: 5 of 2,854 with the radio receiving 500
 *     UDP datagrams a second and 1 of 566 with the network quiet. A
 *     counter in the note's alarm found both SM_ENABLE bits cleared at
 *     the end of each of the six, and of no other note.
 *     v2.5.2 lost none in the same test: the counter stayed at 0 over
 *     9,149 notes (8,393 under the same traffic, 756 with the network
 *     quiet), and none was missing among the 9,010 of them that fell
 *     inside the second board's listening windows. None of the
 *     irregular ones is the library's (read again on 2026-11-19). Five
 *     note boundaries ran 13 to 48 ms late, each in a window where the
 *     firmware erased flash, which keeps the interrupts off for up to
 *     about 50 ms: that note ran long, the next one short by as much,
 *     and the beat kept its time. Two beeps read as 159 ms, with no
 *     pause between them and the beat 80 ms off after each: every note
 *     is scheduled from the previous note's due time, so the board
 *     cannot move the beat, and the second board lost those 80 ms.
 *   - SM2 is re-phased without its enable bit: its clock divider is
 *     parked at 65,535 and restarted, its delay counter cleared and its
 *     program counter forced to the ON instruction while it stands
 *     still, and the tone's divider restarted from phase 0. A tone
 *     opens on its ON half within one gate tick (1/64 of its period) of
 *     that last restart, which ends the call.
 *     Measured on the same board, the pin captured pulse by pulse: 34
 *     note changes of two melodies (400 -> 250 Hz and 880 -> 1100 ->
 *     1320 Hz) opened -0.002 to +0.059 ms from their written time, as
 *     in v2.5.1, and 21 of 21 beeps that follow a pause opened on a
 *     whole ON half (219 to 226 us; the half period is 227 us).
 *   - Whenever no tone sounds, from begin() on, the pin is an SIO
 *     output driven LOW: after begin(), noTone(), stopMelody(),
 *     pauseMelody(), a timed tone's end, a melody's rests and end, and
 *     at volume 0. Up to v2.5.1 that was so only in a melody's rests
 *     and at its end and a timed tone's; elsewhere the pin stayed on
 *     the PIO with its output disabled, held LOW by the pull-down.
 *   - Both state machines run from begin() to end(), in silence too.
 *     Up to v2.5.1 both were stopped in a melody's rests, at its end
 *     and at a timed tone's end. Their supply current was not measured.
 *   - setVolume() patches SM1's two instructions while SM1 runs; SM1
 *     is no longer stopped around the patch.
 *   - Measured on an RP2040 only. Not measured: an RP2350, and a
 *     driver that shares the block from the other core.
 *
 * @par Changelog (v2.5.1)
 *   - Every tone opens on the ON half of its gate cycle. A note change
 *     restarted SM2 with pio_sm_restart() alone, which keeps the program
 *     counter, so the new note resumed the old note's cycle wherever it
 *     stood: it could open on its OFF half, up to half a period late,
 *     and a change just after an ON edge left a click of a few carrier
 *     pulses before a whole OFF half. _toneStart() now stops SM2,
 *     restarts its clock divider and jumps it to the ON instruction, so
 *     a tone opens one gate tick (1/64 of its period) after the gate is
 *     enabled, which comes the caller's latency after the call or the
 *     alarm. Measured on a Pico W, its buzzer pin captured pulse by pulse
 *     (2026-11-18): a 400 -> 250 Hz change after exactly 60 cycles
 *     opened off-phase in 10 of 12 plays, and an 880 -> 1100 -> 1320 Hz
 *     melody started half a period late in 12 of 12. After the fix, each
 *     of the 36 note changes in those 24 plays opened within 0.06 ms of
 *     its written time, counted from the melody's first ON edge.
 *   - tone() called again restarts the tone on its ON half. Calls closer
 *     than half a period (tone() on every loop() pass) now keep the gate
 *     ON, which is silent; v2.5.0 advanced half a cycle per call instead,
 *     which sounds at the wrong pitch. Call tone() when the pitch changes.
 *
 * @par Changelog (v2.5.0)
 *   - begin() and end() now fully protected by critical section,
 *     eliminating race conditions when called from different cores.
 *   - end() atomically sets _ready=false BEFORE releasing PIO resources,
 *     closing the window where another core could call tone() on
 *     deallocated state machines.
 *   - add_alarm_in_ms() return value now checked — alarm pool exhaustion
 *     is handled gracefully (melody aborts cleanly, timed tone stops).
 *   - Volume mapping uses quadratic curve for perceptually linear
 *     loudness control (human hearing is logarithmic).
 *   - SM2 placeholder clkdiv uses int_frac variant instead of float,
 *     avoiding soft-float code emission on Cortex-M0+ (no FPU).
 *   - Melody completion callback: optional user callback fires when
 *     a one-shot melody finishes (setMelodyDoneCallback).
 *   - pauseMelody() / resumeMelody() added for non-destructive
 *     melody suspension (preserves note index).
 *   - getVolume() documented with snapshot semantics for consistency.
 *   - BuzzerNote gains constexpr constructor and durationMs max
 *     documented explicitly (65535 ms ≈ 65.5 seconds).
 *   - keywords.txt version comment updated to v2.5.
 *
 * @par Changelog (v2.4.0)
 *   - Locking harmonized: _timedToneCallback now acquires _lock before
 *     writing _alarm and calling _stopIrqSafe (fixes race where tone()
 *     from another core could interleave with the callback).
 *   - tone(freq) without duration: _toneStart/_toneStop now execute
 *     inside the critical section, consistent with the duration overload.
 *   - setVolume() protected by critical section — _volume write and
 *     _setDuty() are now atomic w.r.t. IRQ callbacks reading _volume.
 *   - _toneStart() adds _ready guard (prevents access to deallocated
 *     SMs if called after end()).
 *   - _setFreq() uses 64-bit arithmetic internally to prevent uint32_t
 *     overflow on freqHz * 64, and clamps frequency defensively.
 *   - Volatile cast in _melodyCallback simplified from double const_cast
 *     to a single removal of volatile (same semantics, cleaner code).
 *   - playMelody()/playMelodyLoop() document lifetime requirement for
 *     the note array pointer in public API (was only in internal docs).
 *
 * @par Changelog (v2.3.0)
 *   - Multi-core safety: critical_section_t (spinlock) replaces raw
 *     save_and_disable_interrupts (which only protects one core).
 *   - tone() frequency parameter widened to uint32_t — now covers
 *     the full [BUZZER_PIO_MIN_FREQ, BUZZER_PIO_MAX_FREQ] range.
 *     BuzzerNote::freqHz remains uint16_t (sufficient for audible tones).
 *   - Move semantics: alarm is cancelled under lock before transferring
 *     ownership (fixes use-after-move race with pending callbacks).
 *   - tone(freq, duration) merged into a single critical section
 *     (eliminates window between alarm cancel and re-schedule).
 *   - Explicit volatile cast in _melodyCallback with justification.
 *   - isPlaying()/isLooping() documented as snapshot-consistent reads.
 *
 * @par Changelog (v2.2.0)
 *   - Critical sections around shared IRQ/thread state (race condition fix)
 *   - _alarm marked volatile (written in IRQ context)
 *   - GPIO pin validation in constructor (0–29)
 *   - Frequency clamped to [BUZZER_PIO_MIN_FREQ, BUZZER_PIO_MAX_FREQ]
 *   - Melody length widened to uint16_t (supports up to 65535 notes)
 *   - Deduplicated playMelody/playMelodyLoop via private _startMelody()
 *   - Move semantics (transfer of hardware ownership)
 */

#ifndef BUZZER_PIO_RP2040_H
#define BUZZER_PIO_RP2040_H

#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"          // spin locks, memory barriers
#include "pico/time.h"
#include "pico/critical_section.h"  // critical_section_init / enter / exit

// =========================================================================
// Configuration
// =========================================================================

/** SM1 (PWM) clock divider. Controls ultrasonic carrier frequency.
 *  Carrier = sys_clk / (CLKDIV × 33) ≈ 94.7 kHz at CLKDIV=40.
 *  The carrier is CONSTANT regardless of volume (period fixed at 33). */
#ifndef BUZZER_PWM_CLKDIV
#define BUZZER_PWM_CLKDIV 40
#endif

/** Maximum valid GPIO pin number on the RP2040 (GP0–GP29). */
#define BUZZER_PIO_MAX_PIN  29

/** Frequency limits derived from sys_clk=125 MHz and SM2 period=64 clocks.
 *  Max = 125 MHz / (1 × 64) ≈ 1.95 MHz, clamped to practical limit.
 *  Min = 125 MHz / (65535 × 64) ≈ 29.8 Hz, rounded down conservatively.
 *
 *  @note tone() accepts uint32_t to cover the full range.
 *        BuzzerNote::freqHz is uint16_t — sufficient for audible tones
 *        (human hearing ≤ 20 kHz; max uint16_t = 65535 Hz). */
#define BUZZER_PIO_MAX_FREQ 976000
#define BUZZER_PIO_MIN_FREQ 15

// =========================================================================
// Types
// =========================================================================

/**
 * @brief  Represents a single note in a melody sequence.
 *
 * @note   Maximum duration per note is 65535 ms (~65.5 seconds).
 *         Use freqHz=0 for silent pauses within a melody.
 */
struct BuzzerNote {
    uint16_t freqHz;      ///< Frequency in Hz (0 = silent pause, max 65535 Hz)
    uint16_t durationMs;  ///< Duration in milliseconds (max 65535 ≈ 65.5 s)

    /// @brief Constexpr constructor for compile-time melody arrays.
    constexpr BuzzerNote(uint16_t freq = 0, uint16_t dur = 0)
        : freqHz(freq), durationMs(dur) {}
};

/**
 * @brief  Callback type for melody completion notification.
 *
 * @warning Fires from hardware alarm IRQ context. Keep the callback
 *          body minimal — no blocking calls, no Serial prints, no
 *          heap allocation. Typically used to set a flag that is
 *          checked in loop().
 *
 * @param  userData  Opaque pointer passed to setMelodyDoneCallback().
 */
using MelodyDoneCallback = void (*)(void* userData);

// =========================================================================
// Class
// =========================================================================

class BuzzerPIO {
public:
    /**
     * @brief  Construct a BuzzerPIO instance.
     * @param  pin  GPIO number (0–29) connected to the passive buzzer.
     * @param  pio  Preferred PIO block: pio0 (default) or pio1.
     * @note   If pin > 29, the instance is marked invalid and begin()
     *         will return false.
     */
    explicit BuzzerPIO(uint8_t pin, PIO pio = pio0);
    ~BuzzerPIO();

    // ── Non-copyable ────────────────────────────────────────────────────
    BuzzerPIO(const BuzzerPIO&)            = delete;
    BuzzerPIO& operator=(const BuzzerPIO&) = delete;

    // ── Movable (transfers hardware ownership) ──────────────────────────
    BuzzerPIO(BuzzerPIO&& other) noexcept;
    BuzzerPIO& operator=(BuzzerPIO&& other) noexcept;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /**
     * @brief  Initialize PIO resources (4 instruction slots + 2 SMs).
     * @return true on success, false if PIO resources unavailable or
     *         pin is invalid.
     * @note   Thread-safe: protected by critical section. Safe to call
     *         from either core, but typically called once from setup().
     */
    bool begin();

    /**
     * @brief  Release all PIO resources and silence the buzzer.
     * @note   Thread-safe: atomically sets _ready=false under lock
     *         BEFORE releasing hardware, preventing use-after-free
     *         from concurrent tone()/melody calls on another core.
     */
    void end();

    bool isReady() const { return _ready; }

    // ── Tone ────────────────────────────────────────────────────────────
    /**
     * @brief  Play a continuous tone; 0 Hz or volume 0 is silence.
     * @note   Each call restarts the tone on the ON half of its cycle.
     *         Call it when the pitch changes, not on every loop() pass:
     *         calls closer than half a period keep the gate ON (silent).
     */
    void tone(uint32_t freqHz);
    void tone(uint32_t freqHz, uint16_t durationMs);
    void noTone();

    // ── Volume ──────────────────────────────────────────────────────────
    void setVolume(uint8_t volume);

    /**
     * @brief  Get current volume setting (0–100).
     * @return Volume level.
     *
     * @note   Returns a point-in-time snapshot. Safe to call from any
     *         core — single-byte read is atomic on Cortex-M0+.
     */
    uint8_t getVolume() const { return _volume; }

    // ── Melody ──────────────────────────────────────────────────────────

    /**
     * @brief  Play a melody once (one-shot).
     * @param  notes  Pointer to note array. **Must remain valid (not go
     *                out of scope) for the entire duration of playback.**
     *                Typically a global/static array or heap-allocated.
     * @param  len    Number of notes (1–65535).
     */
    void playMelody(const BuzzerNote* notes, uint16_t len);

    /**
     * @brief  Play a melody in continuous loop until stopMelody() is called.
     * @param  notes  Pointer to note array. **Must remain valid (not go
     *                out of scope) for the entire duration of playback.**
     *                Typically a global/static array or heap-allocated.
     * @param  len    Number of notes (1–65535).
     */
    void playMelodyLoop(const BuzzerNote* notes, uint16_t len);

    void stopMelody();

    /**
     * @brief  Pause a running melody without losing position.
     *
     * Cancels the current alarm and silences the buzzer, but preserves
     * _noteIndex, _melody, and _looping so that resumeMelody() can
     * continue from the next note.
     *
     * @note   If no melody is playing, this is a no-op.
     *         If the melody is already paused, this is a no-op.
     */
    void pauseMelody();

    /**
     * @brief  Resume a previously paused melody from the next note.
     *
     * @note   If no melody was paused (i.e., _melody is nullptr or
     *         _paused is false), this is a no-op.
     */
    void resumeMelody();

    /**
     * @brief  Register a callback for melody completion (one-shot only).
     *
     * The callback fires from hardware alarm IRQ context when a
     * non-looping melody finishes its last note. It does NOT fire
     * for looping melodies or when stopMelody() is called manually.
     *
     * @param  cb        Function pointer (nullptr to disable).
     * @param  userData  Opaque pointer forwarded to the callback.
     *
     * @warning Keep the callback body minimal — no blocking calls,
     *          no Serial prints, no heap allocation. Set a flag and
     *          handle it in loop().
     */
    void setMelodyDoneCallback(MelodyDoneCallback cb, void* userData = nullptr);

    /**
     * @brief  Check if a melody is currently playing.
     * @return true if a melody is active (one-shot or looping).
     *
     * @note   Returns a point-in-time snapshot. The melody may finish
     *         (via IRQ callback) immediately after this returns true.
     *         Safe to call from any core — reads a single 32-bit pointer
     *         (atomic on Cortex-M0+).
     */
    bool isPlaying() const { return _melody != nullptr; }

    /**
     * @brief  Check if the current melody is set to loop.
     * @return true if melody is looping.
     *
     * @note   Same snapshot semantics as isPlaying(). Safe from any core.
     */
    bool isLooping() const { return _looping; }

    /**
     * @brief  Check if the current melody is paused.
     * @return true if a melody is paused via pauseMelody().
     *
     * @note   Same snapshot semantics as isPlaying(). Safe from any core.
     */
    bool isPaused() const { return _paused; }

    // ── Diagnostics ─────────────────────────────────────────────────────
    PIO getActivePio() const { return _pio; }

private:
    PIO      _pio;
    PIO      _preferredPio;
    uint8_t  _pin;
    bool     _pinValid;                 ///< false if pin > BUZZER_PIO_MAX_PIN
    uint     _smPwm  = 0;              ///< SM1: ultrasonic PWM (value)
    uint     _smGate = 0;              ///< SM2: audio frequency gate (OE)
    uint     _offset = 0;
    bool     _ready  = false;
    uint8_t  _volume = 100;

    // ── Multi-core lock ─────────────────────────────────────────────────
    //   Protects all shared state between thread context and alarm IRQ
    //   callbacks. Uses a hardware spinlock internally — safe across both
    //   RP2040 cores. Initialized in the constructor, not in begin(),
    //   because move semantics need the lock even before begin().
    critical_section_t _lock;

    // ── PIO allocation ──────────────────────────────────────────────────
    bool _tryAllocPio(PIO pio);
    void _freePio();

    // ── Tone internals ──────────────────────────────────────────────────
    void _toneStart(uint32_t freqHz);
    void _toneStop();
    void _stopIrqSafe();
    void _setDuty();
    void _setFreq(uint32_t freqHz);

    // ── Alarm ───────────────────────────────────────────────────────────
    volatile alarm_id_t _alarm = 0;     ///< volatile: written from IRQ context
    void _cancelAlarm();
    static int64_t _timedToneCallback(alarm_id_t id, void* user);

    // ── Melody state (all volatile — shared between thread and IRQ) ─────
    volatile const BuzzerNote* _melody    = nullptr;
    volatile uint16_t          _melodyLen = 0;
    volatile uint16_t          _noteIndex = 0;
    volatile bool              _looping   = false;
    volatile bool              _paused    = false;

    // ── Melody completion callback ──────────────────────────────────────
    MelodyDoneCallback _onMelodyDone     = nullptr;
    void*              _melodyDoneUserData = nullptr;

    void _startMelody(const BuzzerNote* notes, uint16_t len, bool loop);
    static int64_t _melodyCallback(alarm_id_t id, void* user);

    // ── Move helper ─────────────────────────────────────────────────────
    void _moveFrom(BuzzerPIO& other) noexcept;

    // ── PIO program (shared across all instances) ───────────────────────
    static const uint16_t           _programInstructions[4];
    static const struct pio_program _program;
};

#endif // BUZZER_PIO_RP2040_H
