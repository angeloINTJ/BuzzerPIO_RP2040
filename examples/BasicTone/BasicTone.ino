/**
 * @file    BasicTone.ino
 * @brief   BuzzerPIO_RP2040 — Basic tone generation example.
 *
 * Demonstrates:
 *   - Continuous tone (manual stop)
 *   - Timed tone (auto-stop after duration)
 *   - Volume control
 *   - noTone() for silence
 *
 * Wiring:
 *   - Passive buzzer between GP22 and GND
 *   - (Optional) 100Ω series resistor for current limiting
 *
 * @note All tone calls are non-blocking. The loop() runs freely
 *       while the PIO hardware generates the square wave.
 */

#include <BuzzerPIO_RP2040.h>

// Create a buzzer on GPIO 22 using PIO block 0
BuzzerPIO buzzer(22, pio0);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait for USB serial (max 3s)

    Serial.println("BuzzerPIO_RP2040 — BasicTone Example");
    Serial.println("=====================================\n");

    // Initialize PIO resources
    if (!buzzer.begin()) {
        Serial.println("ERROR: Failed to initialize PIO!");
        Serial.println("Check if the PIO block has a free state machine.");
        while (true) delay(1000);
    }

    Serial.println("PIO initialized successfully.\n");

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
    // The PIO generates tones in hardware, no polling needed.
}
