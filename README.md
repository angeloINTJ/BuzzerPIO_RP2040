# BuzzerPIO_RP2040

Ultrasonic PWM tone generation for passive buzzers using the RP2040 PIO coprocessor.

## Why this library?

The standard Arduino `tone()` function on the RP2040 has several problems:

| Problem | `tone()` / PWM | **BuzzerPIO v2.5** |
|---|---|---|
| Timing jitter | 50–200 ms when loop is busy | **< 10 µs** (hardware alarm) |
| Blocking | Some implementations block | **100% non-blocking** |
| PWM conflicts | Uses PWM slices shared with servos/LEDs | **Uses PIO** (independent) |
| Volume control | Not supported | **32 levels** via ultrasonic PWM duty cycle |
| Volume perception | N/A | **Perceptual curve** (quadratic mapping) |
| Melody playback | Requires polling in loop() | **Hardware alarm chain** |
| Melody events | N/A | **Completion callback** (IRQ-driven) |
| Melody control | N/A | **Pause/resume** support |
| CPU usage during tone | Periodic ISR or busy-wait | **Zero** (PIO runs alone) |
| Volume distortion | N/A | **None** (ultrasonic carrier, not audible-freq duty) |
| Multi-core safety | Not specified | **Spinlock-protected** (safe from both cores) |

## How it works — Dual-SM AND gate

```
          SM1 (ultrasonic PWM)              SM2 (tone gate)
          ┌─────────────────┐              ┌─────────────────┐
          │ set pins,1 [dH] │              │ set pindirs,1[31]│
clkdiv=40 │ set pins,0 [dL] │  clkdiv=var  │ set pindirs,0[31]│
  ~95 kHz │ (wraps [0..1])  │  =tone freq  │ (wraps [2..3])  │
          └────────┬────────┘              └────────┬────────┘
                   │ VALUE                          │ OE
                   │                                │
          PIO combines: value = SM1_val, OE = SM2_oe
                   │                                │
                   └──────── GPIO pin ──────────────┘
                             │
                        pull-down R
                             │
                            GND

OE=1 → pin outputs SM1 PWM (buzzer hears ultrasonic carrier)
OE=0 → pin hi-Z → pulled LOW (silence)

Result: output = SM1_pwm AND SM2_gate
```

1. **SM1 (PWM carrier)** — 2 instructions wrapping `[0..1]`. Generates a ~95 kHz ultrasonic PWM square wave via `set pins`. The duty cycle controls the equivalent amplitude (volume). The buzzer's mechanical inertia low-pass filters the carrier, so only the duty-cycle envelope is perceived as loudness.

2. **SM2 (tone gate)** — 2 instructions wrapping `[2..3]`. Square wave at the audible frequency via `set pindirs` (output enable). When the gate is open (OE=1), the buzzer hears SM1's PWM. When closed (OE=0), the GPIO pull-down holds the pin LOW.

3. **AND gate via hardware** — SM1 controls the pin VALUE but never touches OE. SM2 controls OE but never touches VALUE. The RP2040 PIO ORs per-SM outputs within a block: `final_value = SM1_val`, `final_OE = SM2_oe`. With the GPIO pull-down, the result is `output = SM1_pwm AND SM2_gate`.

4. **Volume** — Patching SM1's instruction delays changes the PWM duty cycle from 3% (barely audible) to 97% (maximum). v2.5 uses a quadratic mapping curve so that perceived loudness increases evenly across the 0–100 range (matching human hearing's logarithmic response). The carrier frequency stays constant at ~95 kHz regardless of volume — no harmonic distortion, no frequency shift.

5. **Melody sequencing** — Hardware alarm chain (same proven mechanism as v1.0). Each note transition sets SM2's clock divider for the new frequency. All note transitions happen with < 10 µs jitter, completely independent of the main loop.

## Installation

### Arduino IDE

1. Download the [latest release](https://github.com/angeloINTJ/BuzzerPIO_RP2040/releases) as a `.zip` file
2. In Arduino IDE: **Sketch → Include Library → Add .ZIP Library...**
3. Select the downloaded `.zip` file

### PlatformIO

Add to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/angeloINTJ/BuzzerPIO_RP2040.git
```

### Arduino Library Manager

Search for **BuzzerPIO_RP2040** in the Library Manager (Tools → Manage Libraries...).

## Quick start

```cpp
#include <BuzzerPIO_RP2040.h>

BuzzerPIO buzzer(22);  // GPIO 22, auto-probes pio0 then pio1

void setup() {
    buzzer.begin();
    buzzer.setVolume(80);
    buzzer.tone(1000, 500);  // 1 kHz for 500 ms — non-blocking!
}

void loop() {
    // CPU is free — dual PIO state machines handle everything
}
```

## API reference

### Constructor

```cpp
BuzzerPIO(uint8_t pin, PIO pio = pio0);
```

| Parameter | Description |
|---|---|
| `pin` | GPIO number (0–29) connected to the passive buzzer |
| `pio` | Preferred PIO block: `pio0` (default) or `pio1`. If unavailable, `begin()` automatically tries the other block. |

**PIO auto-probe:** The library needs 4 instruction slots + 2 state machines in the **same** PIO block (the AND gate requires per-block OR of SM outputs). If the preferred block doesn't have enough resources, `begin()` transparently falls back to the other.

### Lifecycle

```cpp
bool begin();        // Initialize dual-SM PIO. Returns false if resources unavailable.
void end();          // Release all resources. Called automatically by destructor.
bool isReady();      // Check if begin() succeeded.
PIO getActivePio();  // Returns which PIO block was actually allocated.
```

### Tone

```cpp
void tone(uint32_t freqHz);                    // Continuous tone
void tone(uint32_t freqHz, uint16_t durationMs); // Timed tone (auto-stop)
void noTone();                                 // Silence immediately
```

All calls are **non-blocking**. A timed tone uses a hardware alarm for auto-shutoff — no CPU involvement.

**Frequency range:** 15 Hz – 976 kHz (`uint32_t`). Audible sweet spot: 200 Hz – 8 kHz.

> **Note:** `BuzzerNote::freqHz` is `uint16_t` (max 65535 Hz), which is more than sufficient for audible melodies. The `tone()` method accepts `uint32_t` to cover the full hardware-supported range for ultrasonic or special-purpose applications.

### Volume

```cpp
void setVolume(uint8_t volume);  // 0 (silent) to 100 (max)
uint8_t getVolume();
```

Volume controls the ultrasonic PWM duty cycle. Can be changed while a tone is playing — takes effect immediately via PIO instruction patching.

v2.5 uses a **quadratic mapping curve** for perceptually linear volume steps. This means the difference between volume 10→20 sounds roughly the same as 50→60, matching how human hearing works.

| Volume | Duty (v2.5 quadratic) | Carrier frequency | Perceived effect |
|---|---|---|---|
| 100% | 97% | ~95 kHz | Maximum amplitude |
| 50% | ~25% | ~95 kHz | Perceptually "half" |
| 10% | ~3% | ~95 kHz | Very quiet |
| 0% | — | — | Silent (gate disabled) |

The carrier frequency is **constant** regardless of volume — no harmonic distortion.

### Melody playback

```cpp
void playMelody(const BuzzerNote* notes, uint16_t len);      // Play once
void playMelodyLoop(const BuzzerNote* notes, uint16_t len);   // Play forever
void stopMelody();                                            // Stop immediately
void pauseMelody();                                           // Pause (v2.5)
void resumeMelody();                                          // Resume (v2.5)
bool isPlaying();                                             // Check status
bool isLooping();                                             // Check loop mode
bool isPaused();                                              // Check pause state (v2.5)
```

**BuzzerNote structure:**

```cpp
struct BuzzerNote {
    uint16_t freqHz;      // Frequency in Hz (0 = silent pause, max 65535)
    uint16_t durationMs;  // Duration in milliseconds (max 65535 ≈ 65.5 s)
};
```

**Example — defining a melody:**

```cpp
const BuzzerNote myMelody[] = {
    { 523, 200 },   // C5 for 200 ms
    { 659, 200 },   // E5 for 200 ms
    {   0, 100 },   // 100 ms silence
    { 784, 400 }    // G5 for 400 ms
};

buzzer.playMelody(myMelody, 4);
```

> **Important:** The `notes` array must remain valid (in memory) for the entire duration of playback. Use `const` arrays at global/file scope or `static` arrays inside functions. Do **not** pass a local array that goes out of scope before the melody finishes.

### Melody completion callback (v2.5)

```cpp
void setMelodyDoneCallback(MelodyDoneCallback cb, void* userData = nullptr);
```

Register a callback that fires when a **one-shot** melody finishes its last note. The callback does **not** fire for looping melodies or when `stopMelody()` is called manually.

```cpp
volatile bool melodyDone = false;

void onDone(void* /* userData */) {
    melodyDone = true;  // Set flag — don't do heavy work here!
}

buzzer.setMelodyDoneCallback(onDone);
buzzer.playMelody(notes, len);

// In loop():
if (melodyDone) {
    melodyDone = false;
    // React to melody completion
}
```

> **Warning:** The callback fires from hardware alarm IRQ context. Keep it minimal — set a flag, nothing more. No Serial, no delay(), no heap allocation.

### Pause / Resume (v2.5)

```cpp
buzzer.pauseMelody();   // Silences buzzer, preserves position
buzzer.resumeMelody();  // Continues from the next note
```

Useful for temporary mute during alarm scenarios. The current note is considered consumed when paused — resume starts from the **next** note.

### Thread safety (v2.3+)

All public methods are **multi-core safe**. Shared state between the application thread and hardware alarm callbacks is protected by a pico-sdk `critical_section_t` (hardware spinlock + local interrupt disable). You can safely call `tone()`, `stopMelody()`, etc. from either RP2040 core.

`isPlaying()`, `isLooping()`, `isPaused()`, and `getVolume()` return **point-in-time snapshots** — the melody may finish immediately after the call returns `true`. This is the expected behavior for lock-free status queries.

v2.5 additionally protects `begin()` and `end()` with the critical section, closing race windows that existed in v2.4 when these methods were called from different cores.

## Wiring

```
  RP2040 GPIO 22 ──── (+) Passive Buzzer (–) ──── GND
                  │
                  └── (Optional) 100Ω resistor for current limiting
```

**Passive buzzer only.** Active buzzers have a built-in oscillator and produce a fixed tone regardless of the input frequency — they won't work with this library.

> The library enables the internal GPIO pull-down automatically. No external pull-down resistor is needed.

## Advanced configuration

Override this define **before** including the library header, or via build flags:

```cpp
// Higher carrier frequency (~190 kHz) — less audible on some buzzers
#define BUZZER_PWM_CLKDIV 20

#include <BuzzerPIO_RP2040.h>
```

| Define | Default | Description |
|---|---|---|
| `BUZZER_PWM_CLKDIV` | 40 | SM1 clock divider. Carrier ≈ 125 MHz / (CLKDIV × 33) |

## Resource usage

| Resource | Usage |
|---|---|
| PIO state machines | 2 (auto-claimed, same block) |
| PIO instruction memory | 4 slots (of 32 per block) |
| DMA channels | 0 |
| IRQ handlers | 0 |
| Hardware alarms | 1 (from Pico SDK alarm pool) |
| Hardware spinlocks | 1 (via `critical_section_t`) |
| RAM | ~56 bytes per instance |
| Flash | ~2.8 KB (code) |
| CPU | 0% during tone/melody (only on API calls) |

## Coexistence with other PIO libraries

The dual-SM architecture was specifically designed to fit alongside other PIO-intensive libraries on the Pico W:

| PIO block | Library | Instructions | SMs |
|---|---|---|---|
| pio0 | OneWirePIO_RP2040 (DS18B20) | 27/32 | 1/4 |
| pio0 | **BuzzerPIO** (if auto-probed here) | 4/32 | 2/4 |
| pio1 | CYW43 WiFi SPI (Pico W) | ~10/32 | 1/4 |
| pio1 | DHT22PIO_RP2040 | 17/32 | 1/4 |
| pio1 | **BuzzerPIO** (if auto-probed here) | 4/32 | 2/4 |

Both blocks have room for BuzzerPIO (4 instructions + 2 SMs). The auto-probe tries the preferred block first, then falls back. If neither block has enough resources, `begin()` returns `false`.

## Migration from v1.x / v2.x

### From v1.x

**No code changes required.** The public API is backward compatible. Just update the library and recompile.

Behavioral differences:
- **Volume quality**: Volume changes are now distortion-free. In v1.0, low volume settings distorted the waveform by making it asymmetric. In v2.x, volume controls the ultrasonic carrier duty cycle — the audible waveform stays symmetric at all levels.
- **Volume curve** (v2.5): Volume mapping is now quadratic (perceptually linear). The same `setVolume(50)` call will sound quieter than in v2.4 because it now represents ~25% duty instead of ~50%. If you relied on specific volume→duty mappings, adjust your volume values.
- **Resource usage**: Uses 1 additional state machine (2 total vs 1 in v1.0) + 1 spinlock. No DMA channels needed.
- **Auto-probe**: If the preferred PIO block is full, `begin()` transparently tries the other. In v1.0, it would just fail.

### From v2.2–v2.4

- `tone()` parameter changed from `uint16_t` to `uint32_t` (v2.3). Existing code compiles without changes (implicit widening).
- Internal locking upgraded from interrupt-disable to `critical_section_t` (v2.3). No API changes required.
- **Volume curve changed** (v2.5): See note above under v1.x migration.
- **New methods** (v2.5): `pauseMelody()`, `resumeMelody()`, `isPaused()`, `setMelodyDoneCallback()`. All additive — existing code is unaffected.

## FAQ

**Q: Can I use this with NeoPixels / WS2812?**
A: Yes. NeoPixel libraries typically use `pio0` SM0. BuzzerPIO auto-claims the next free SMs. If `pio0` is full, pass `pio1`: `BuzzerPIO buzzer(22, pio1);`

**Q: Can I have two buzzers?**
A: Only one per PIO block (each instance needs 2 SMs + 4 instruction slots). With both PIO blocks, you could have two buzzers on different blocks.

**Q: Does this work on the Pico W?**
A: Yes. The Pico W uses `pio1` SM0 for the CYW43 Wi-Fi driver. BuzzerPIO on `pio1` auto-claims SM1+SM2 (leaving SM0 for Wi-Fi). On `pio0`, there's no conflict at all.

**Q: What about the Pico 2 (RP2350)?**
A: The RP2350 has PIO v2 with the same instruction set and 3 PIO blocks. This library should work without changes, but has not been tested yet.

**Q: Why ultrasonic PWM instead of direct frequency like v1.0?**
A: In v1.0, volume was controlled by making the square wave asymmetric (e.g., 10% HIGH / 90% LOW). This changes the harmonic content — the tone sounds different at different volumes. In v2.x, the audible waveform is always a clean 50% duty square wave. Volume is controlled by the amplitude of an ultrasonic carrier that the buzzer's mechanical inertia filters out. The result: volume changes only change loudness, not timbre.

**Q: Why two state machines instead of one?**
A: The AND gate trick requires two independent signals on the same GPIO — one for value and one for output enable. A single SM can't toggle both independently at different frequencies. The dual-SM approach achieves this with zero DMA and zero IRQ handlers, using only PIO-internal hardware.

**Q: Is it safe to call tone() from Core 1 while a melody plays on Core 0?**
A: Yes (v2.3+). All shared state is protected by a hardware spinlock. Calling `tone()`, `stopMelody()`, `setVolume()`, etc. from either core is safe. See the [DualCore](examples/DualCore/DualCore.ino) example.

## Examples

| Example | Description |
|---|---|
| [BasicTone](examples/BasicTone/BasicTone.ino) | Continuous/timed tones, volume sweep, frequency sweep |
| [MelodyPlayer](examples/MelodyPlayer/MelodyPlayer.ino) | Melodies, completion callback, pause/resume demo |
| [AlarmLoop](examples/AlarmLoop/AlarmLoop.ino) | Temperature alarm with looping siren, button dismiss, pause/mute |
| [DualCore](examples/DualCore/DualCore.ino) | Multi-core safety: Core 0 runs melodies, Core 1 adjusts volume |

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a pull request.

### Quick Guide

1. Fork this repository
2. Create a feature branch: `git checkout -b feature/my-improvement`
3. Commit your changes: `git commit -m "Add: description of change"`
4. Push to the branch: `git push origin feature/my-improvement`
5. Open a Pull Request

## License

MIT License — see [LICENSE](LICENSE).

## Acknowledgments

* Raspberry Pi Foundation for the RP2040 PIO architecture
* The Arduino-Pico community for the RP2040 Arduino core (Earle Philhower)
* The embedded community for feedback on PIO-based audio generation techniques
