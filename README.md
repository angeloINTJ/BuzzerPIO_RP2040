# BuzzerPIO_RP2040

Zero-jitter tone generation for passive buzzers using the RP2040 PIO coprocessor.

## Why this library?

The standard Arduino `tone()` function on the RP2040 has several problems:

| Problem | `tone()` / PWM | **BuzzerPIO** |
|---------|---------------|---------------|
| Timing jitter | 50–200 ms when loop is busy | **< 10 µs** (hardware alarm) |
| Blocking | Some implementations block | **100% non-blocking** |
| PWM conflicts | Uses PWM slices shared with servos/LEDs | **Uses PIO** (independent) |
| Volume control | Not supported | **64 levels** via duty cycle |
| Melody playback | Requires polling in loop() | **Hardware alarm chain** |
| CPU usage during tone | Periodic ISR or busy-wait | **Zero** (PIO runs alone) |

## How it works

```
┌─────────┐    setFreq()     ┌─────────────┐    square wave    ┌─────────┐
│  Your   │ ──────────────→  │  PIO State  │ ──────────────→  │ Passive │
│  Code   │    setVolume()   │  Machine    │    GPIO pin      │ Buzzer  │
│         │                  │ (4 instrs)  │                  │         │
│         │  playMelody()    ├─────────────┤                  └─────────┘
│         │ ──────────────→  │  Hardware   │
│         │                  │  Alarm Chain│
│         │    (returns      │  (IRQ ctx)  │
│         │   immediately)   └─────────────┘
└─────────┘                   Runs autonomously
```

1. **PIO program** (4 instructions in a wrap-loop) generates a continuous square wave. Frequency is controlled by the PIO clock divider; volume by patching the instruction delays to adjust the duty cycle.

2. **Hardware alarm chain** sequences melody notes in IRQ context. Each alarm callback configures the PIO for the next note and re-schedules itself for that note's duration. Timing precision is limited only by the RP2040 hardware timer (< 10 µs).

3. **Your code** calls `tone()` or `playMelody()` and continues immediately. No `update()`, no polling, no `delay()`.

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

BuzzerPIO buzzer(22);  // GPIO 22, uses pio0 by default

void setup() {
    buzzer.begin();
    buzzer.setVolume(80);
    buzzer.tone(1000, 500);  // 1 kHz for 500 ms — non-blocking!
}

void loop() {
    // CPU is free — PIO handles the tone
}
```

## API reference

### Constructor

```cpp
BuzzerPIO(uint8_t pin, PIO pio = pio0);
```

| Parameter | Description |
|-----------|-------------|
| `pin` | GPIO number (0–28) connected to the passive buzzer |
| `pio` | PIO block to use: `pio0` (default) or `pio1` |

**Choosing a PIO block:** Each RP2040 has 2 PIO blocks with 4 state machines each. If `pio0` is occupied by other PIO libraries (NeoPixel, I2S, etc.), use `pio1`.

### Lifecycle

```cpp
bool begin();   // Initialize PIO. Returns false if no SM available.
void end();     // Release all resources. Called automatically by destructor.
bool isReady(); // Check if begin() succeeded.
```

### Tone

```cpp
void tone(uint16_t freqHz);                    // Continuous tone
void tone(uint16_t freqHz, uint16_t durationMs); // Timed tone (auto-stop)
void noTone();                                 // Silence immediately
```

All calls are **non-blocking**. A timed tone uses a hardware alarm for auto-shutoff — no CPU involvement.

**Frequency range:** 15 Hz – 976 kHz (at 125 MHz sys_clk). Audible sweet spot: 200 Hz – 8 kHz.

### Volume

```cpp
void setVolume(uint8_t volume);  // 0 (silent) to 100 (max)
uint8_t getVolume();
```

Volume is implemented by adjusting the duty cycle of the square wave via PIO instruction patching. Can be changed while a tone is playing — takes effect immediately.

| Volume | Duty cycle | Effect |
|--------|-----------|--------|
| 100% | 50% | Maximum amplitude |
| 50% | ~25% | Moderate |
| 10% | ~5% | Quiet |
| 0% | 0% | Silent (SM disabled) |

### Melody playback

```cpp
void playMelody(const BuzzerNote* notes, uint8_t len);      // Play once
void playMelodyLoop(const BuzzerNote* notes, uint8_t len);   // Play forever
void stopMelody();                                            // Stop immediately
bool isPlaying();                                             // Check status
bool isLooping();                                             // Check loop mode
```

**BuzzerNote structure:**

```cpp
struct BuzzerNote {
    uint16_t freqHz;      // Frequency in Hz (0 = silent pause)
    uint16_t durationMs;  // Duration in milliseconds
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

## Wiring

```
  RP2040 GPIO 22 ──── (+) Passive Buzzer (–) ──── GND
                  │
                  └── (Optional) 100Ω resistor for current limiting
```

**Passive buzzer only.** Active buzzers have a built-in oscillator and produce a fixed tone regardless of the input frequency — they won't work with this library.

## Resource usage

| Resource | Usage |
|----------|-------|
| PIO state machines | 1 (auto-claimed) |
| PIO instruction memory | 4 slots (of 32 per block) |
| Hardware alarms | 1 (from Pico SDK alarm pool) |
| RAM | ~40 bytes per instance |
| Flash | ~2 KB (code) |
| CPU | 0% during tone/melody (only on API calls) |

## FAQ

**Q: Can I use this with NeoPixels / WS2812?**
A: Yes. NeoPixel libraries typically use `pio0` SM0. BuzzerPIO auto-claims the next free SM. If `pio0` is full, construct with `pio1`: `BuzzerPIO buzzer(22, pio1);`

**Q: Can I have two buzzers?**
A: Yes, each on its own GPIO and SM: `BuzzerPIO buzz1(22); BuzzerPIO buzz2(18);`. Each uses one SM from the same (or different) PIO block.

**Q: Why not just use PWM?**
A: PWM works for simple tones but has three issues: (1) each PWM slice is shared between two GPIOs — changing one affects the other; (2) there's no built-in melody sequencing; (3) the RP2040 has only 8 PWM slices, often needed for motors/LEDs. PIO state machines are independent and purpose-built for this.

**Q: Does this work on the Pico W?**
A: Yes. The Pico W uses `pio1` SM0 for the CYW43 Wi-Fi driver. BuzzerPIO on `pio0` works without conflict. If you need `pio1`, it will auto-claim SM1 (leaving SM0 for Wi-Fi).

**Q: What about the Pico 2 (RP2350)?**
A: The RP2350 has PIO v2 with the same instruction set. This library should work without changes, but has not been tested yet.

## Examples

| Example | Description |
|---------|-------------|
| [BasicTone](examples/BasicTone/BasicTone.ino) | Continuous/timed tones, volume sweep, frequency sweep |
| [MelodyPlayer](examples/MelodyPlayer/MelodyPlayer.ino) | Predefined melodies, interactive serial replay |
| [AlarmLoop](examples/AlarmLoop/AlarmLoop.ino) | Simulated temperature alarm with looping siren and button dismiss |

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
