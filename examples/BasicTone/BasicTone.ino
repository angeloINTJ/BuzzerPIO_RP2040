/**
 * @file    BasicTone.ino
 * @brief   BuzzerPIO_RP2040 — Basic tone generation example.
 *
 * Demonstrates:
 *   - Continuous tone (manual stop)
 *   - Timed tone (auto-stop after duration)
 *   - Volume control (ultrasonic PWM amplitude)
 *   - noTone() for silence
 *
 * Wiring:
 *   - Passive buzzer between GP22 and GND
 *   - (Optional) 100Ω series resistor for current limiting
 *
 * @note All tone calls are non-blocking. The dual PIO state machines
 *       (ultrasonic PWM + tone gate) run autonomously in hardware.
 */

#include <BuzzerPIO_RP2040.h>

// Create a buzzer on GPIO 22 (auto-probes pio0/pio1 for 4 free slots + 2 SMs)
BuzzerPIO buzzer(22, pio0);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait for USB serial (max 3s)

    Serial.println("BuzzerPIO_RP2040 — BasicTone Example");
    Serial.println("=====================================\n");

    // Initialize PIO resources (needs 4 instruction slots + 2 SMs)
    if (!buzzer.begin()) {
        Serial.println("ERROR: Failed to initialize PIO!");
        Serial.println("Need 4 free instruction slots + 2 state machines.");
        while (true) delay(1000);
    }

    Serial.print("PIO initialized on pio");
    Serial.println((buzzer.getActivePio() == pio0) ? '0' : '1');
    Serial.println();

    // ── Example 1: Continuous tone ─────────────────────────────────
    Serial.println("1. Continuous 1 kHz tone for 1 second...");
    buzzer.tone(1000);        // Start 1 kHz (runs until noTone)
    delay(1000);              // Wait 1 second
    buzzer.noTone();          // Stop manually
    delay(500);

    // ── Example 2: Timed tone ──────────────────────────────────────
    Serial.println("2. Timed 2 kHz tone for 500 ms (auto-stop)...");
    buzzer.tone(2000, 500);   // 2 kHz for 500 ms — non-blocking!
    delay(1000);              // CPU is free during tone

    // ── Example 3: Volume sweep ────────────────────────────────────
    // v2.5: Quadratic volume curve — perceived loudness now increases
    // evenly. Low volumes (10-30) are more distinguishable than before.
    Serial.println("3. Volume sweep at 880 Hz (A5)...");
    for (int vol = 10; vol <= 100; vol += 10) {
        buzzer.setVolume(vol);
        buzzer.tone(880, 200);
        delay(300);
    }
    delay(500);

    // ── Example 4: Frequency sweep ─────────────────────────────────
    Serial.println("4. Frequency sweep 200 Hz → 4000 Hz...");
    buzzer.setVolume(80);
    for (int freq = 200; freq <= 4000; freq += 200) {
        buzzer.tone(freq, 80);
        delay(120);
    }
    delay(500);

    // ── Example 5: Silent pause (freq = 0) ─────────────────────────
    Serial.println("5. Beep - pause - beep...");
    buzzer.tone(1500, 200);
    delay(500);
    buzzer.tone(1500, 200);
    delay(500);

    Serial.println("\nDone! Buzzer is silent.");
    Serial.println("The loop() below does nothing — CPU is 100% free.");
}

void loop() {
    // Nothing to do here.
    // The dual PIO state machines generate tones in hardware, no polling needed.
}
