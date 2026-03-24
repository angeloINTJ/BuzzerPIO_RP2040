/**
 * @file    DualCore.ino
 * @brief   BuzzerPIO_RP2040 — Multi-core safety demonstration.
 *
 * Demonstrates:
 *   - BuzzerPIO used safely from both RP2040 cores simultaneously
 *   - Core 0: reads simulated sensor, triggers alarm melody
 *   - Core 1: adjusts volume and can override with manual tone
 *   - critical_section_t (hardware spinlock) prevents data races
 *   - Melody completion callback from IRQ context
 *
 * Wiring:
 *   - Passive buzzer between GP22 and GND
 *
 * Architecture:
 *   Core 0 (setup/loop):
 *     - Initializes buzzer (begin)
 *     - Simulates sensor readings every 500 ms
 *     - Triggers alarm melody when threshold exceeded
 *     - Prints status via Serial
 *
 *   Core 1 (setup1/loop1):
 *     - Periodically adjusts volume (simulating a UI knob)
 *     - Periodically plays a short "heartbeat" tone to confirm
 *       Core 1 can safely call tone() while Core 0 runs melodies
 *
 * @note This example validates that tone(), setVolume(), playMelody(),
 *       and stopMelody() can be called concurrently from different
 *       cores without crashes, glitches, or race conditions.
 */

#include <BuzzerPIO_RP2040.h>

// ── Shared buzzer instance ───────────────────────────────────────────
// BuzzerPIO is thread-safe via critical_section_t (hardware spinlock).
// Both cores may call any public method at any time.
BuzzerPIO buzzer(22, pio0);

// ── Shared flag: melody finished (set from IRQ, read from Core 0) ───
volatile bool melodyDone = false;

void onMelodyDone(void* /* userData */) {
    melodyDone = true;
}

// ── Alarm melody ─────────────────────────────────────────────────────
const BuzzerNote alarmMelody[] = {
    { 2000, 150 },
    {    0,  80 },
    { 2500, 150 },
    {    0,  80 },
    { 2000, 150 },
    {    0, 300 }
};
const uint16_t ALARM_LEN = sizeof(alarmMelody) / sizeof(BuzzerNote);

// ── Simulated sensor ─────────────────────────────────────────────────
volatile float sensorValue   = 25.0;
const float    SENSOR_THRESH = 30.0;

// =========================================================================
// CORE 0 — Sensor monitoring + alarm logic
// =========================================================================

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    Serial.println("BuzzerPIO_RP2040 — DualCore Example");
    Serial.println("====================================");
    Serial.println("Core 0: sensor monitor + alarm");
    Serial.println("Core 1: volume knob + heartbeat tone\n");

    if (!buzzer.begin()) {
        Serial.println("ERROR: PIO init failed!");
        while (true) delay(1000);
    }

    buzzer.setVolume(60);
    buzzer.setMelodyDoneCallback(onMelodyDone);

    Serial.println("Buzzer ready. Simulating sensor drift...\n");
}

void loop() {
    static uint32_t lastRead = 0;
    static bool     alarming = false;

    if (millis() - lastRead >= 500) {
        lastRead = millis();

        // Simulate slow temperature rise then drop (sawtooth)
        static float delta = 0.5;
        sensorValue += delta;
        if (sensorValue > 35.0) delta = -0.5;
        if (sensorValue < 20.0) delta =  0.5;

        // ── Check threshold ──────────────────────────────────────
        if (sensorValue > SENSOR_THRESH && !alarming) {
            alarming   = true;
            melodyDone = false;
            Serial.println("[Core 0] ALARM triggered — playing melody");
            buzzer.playMelody(alarmMelody, ALARM_LEN);
        }

        if (alarming && melodyDone) {
            alarming = false;
            Serial.println("[Core 0] Alarm melody finished (callback)");
        }

        if (sensorValue <= SENSOR_THRESH && alarming) {
            buzzer.stopMelody();
            alarming = false;
            Serial.println("[Core 0] Sensor normal — alarm stopped");
        }

        // Print status
        Serial.print("[Core 0] Sensor: ");
        Serial.print(sensorValue, 1);
        Serial.print(" C | Vol: ");
        Serial.print(buzzer.getVolume());
        Serial.print(" | Playing: ");
        Serial.println(buzzer.isPlaying() ? "yes" : "no");
    }
}

// =========================================================================
// CORE 1 — Volume adjustment + heartbeat tone
//
// Calls setVolume() and tone() from a different core than the one
// running the melody. The critical_section_t inside BuzzerPIO ensures
// these calls are serialized safely with the alarm callbacks.
// =========================================================================

void setup1() {
    // Core 1 setup — nothing special needed. The buzzer was already
    // initialized by Core 0's begin(). All public methods are safe
    // to call from either core immediately after begin() returns.
}

void loop1() {
    static uint32_t lastVol   = 0;
    static uint32_t lastBeep  = 0;
    static uint8_t  volume    = 60;
    static int8_t   volDelta  = 5;

    // ── Adjust volume every 2 seconds (simulates a UI knob) ─────────
    if (millis() - lastVol >= 2000) {
        lastVol = millis();

        volume += volDelta;
        if (volume >= 100) { volume = 100; volDelta = -5; }
        if (volume <= 20)  { volume = 20;  volDelta =  5; }

        // Safe to call from Core 1 — protected by spinlock internally
        buzzer.setVolume(volume);
    }

    // ── Short heartbeat beep every 5 seconds (if no melody playing) ──
    // This demonstrates that Core 1 can call tone() even while Core 0
    // manages melodies. If a melody IS playing, tone() will interrupt
    // it (by design — tone() cancels the current melody).
    if (millis() - lastBeep >= 5000) {
        lastBeep = millis();

        if (!buzzer.isPlaying()) {
            // Quick 50 ms beep — confirms Core 1 has safe access
            buzzer.tone(1000, 50);
        }
    }

    delay(100);  // Yield — Core 1 doesn't need to spin
}
