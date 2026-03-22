/**
 * @file    AlarmLoop.ino
 * @brief   BuzzerPIO_RP2040 — Continuous alarm with button dismiss example.
 *
 * Demonstrates:
 *   - playMelodyLoop() for continuous alarm siren
 *   - stopMelody() to silence the alarm
 *   - isLooping() to check alarm state
 *   - Multiple alarm patterns (selectable via Serial)
 *   - Simulated temperature-triggered alarm
 *
 * Wiring:
 *   - Passive buzzer on GP22
 *   - Push button on GP15 (pulled up internally, active LOW)
 *   - (Optional) LED on GP25 (onboard LED) for visual feedback
 *
 * @note The alarm siren runs entirely in hardware. The loop() is free
 *       to read sensors, update displays, etc. without any jitter in
 *       the alarm sound.
 */

#include <BuzzerPIO_RP2040.h>

// ── Pin definitions ──────────────────────────────────────────────────
#define BUZZER_PIN   22
#define BUTTON_PIN   15
#define LED_PIN      25    // Onboard LED (Pico / Pico W)

BuzzerPIO buzzer(BUZZER_PIN, pio0);

// ── Alarm patterns ───────────────────────────────────────────────────

// Pattern 1: Classic dual beep
const BuzzerNote alarmDualBeep[] = {
    { 2200, 180 },   // Beep 1
    {    0, 100 },   // Pause
    { 2200, 180 },   // Beep 2
    {    0, 500 }    // Long pause between cycles
};

// Pattern 2: Alternating siren
const BuzzerNote alarmSiren[] = {
    { 1800, 200 },   // Low
    { 2400, 200 },   // High
    { 1800, 200 },   // Low
    {    0, 400 }    // Pause
};

// Pattern 3: Rapid pulse
const BuzzerNote alarmRapid[] = {
    { 3000, 100 },
    {    0,  60 },
    { 3000, 100 },
    {    0,  60 },
    { 3000, 100 },
    {    0, 400 }
};

// Pattern 4: Escalating urgency
const BuzzerNote alarmEscalate[] = {
    { 1600, 150 },
    { 2000, 150 },
    { 2400, 150 },
    {    0, 450 }
};

// Confirmation beep (played when alarm is dismissed)
const BuzzerNote confirmBeep[] = {
    { 880,  80 },
    { 1100, 80 },
    { 1320, 120 }
};

// ── Alarm pattern table ──────────────────────────────────────────────
struct AlarmPattern {
    const char*       name;
    const BuzzerNote* notes;
    uint8_t           len;
};

const AlarmPattern patterns[] = {
    { "Dual Beep", alarmDualBeep, sizeof(alarmDualBeep) / sizeof(BuzzerNote) },
    { "Siren",     alarmSiren,    sizeof(alarmSiren)    / sizeof(BuzzerNote) },
    { "Rapid",     alarmRapid,    sizeof(alarmRapid)    / sizeof(BuzzerNote) },
    { "Escalate",  alarmEscalate, sizeof(alarmEscalate) / sizeof(BuzzerNote) }
};
const int PATTERN_COUNT = sizeof(patterns) / sizeof(AlarmPattern);
int currentPattern = 0;

// ── Simulated temperature sensor ─────────────────────────────────────
float simulatedTemp = 25.0;
const float TEMP_THRESHOLD = 30.0;
bool alarmActive = false;
uint32_t lastTempRead = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    // Configure button with internal pull-up
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.println("BuzzerPIO_RP2040 — AlarmLoop Example");
    Serial.println("=====================================");
    Serial.println("Simulated temperature alarm system.\n");

    if (!buzzer.begin()) {
        Serial.println("ERROR: PIO init failed!");
        while (true) delay(1000);
    }

    buzzer.setVolume(80);

    Serial.println("Commands via Serial:");
    Serial.println("  + / -   : Adjust simulated temperature");
    Serial.println("  1-4     : Change alarm pattern");
    Serial.println("  d       : Dismiss alarm");
    Serial.print("\nAlarm threshold: ");
    Serial.print(TEMP_THRESHOLD, 1);
    Serial.println(" C\n");

    printStatus();
}

void loop() {
    // ── Read simulated temperature every 500ms ──────────────────────
    if (millis() - lastTempRead > 500) {
        lastTempRead = millis();

        // Check alarm condition
        if (simulatedTemp > TEMP_THRESHOLD && !alarmActive) {
            // Temperature exceeded threshold — start alarm!
            alarmActive = true;
            Serial.println("*** ALARM: Temperature exceeded threshold! ***");
            buzzer.playMelodyLoop(
                patterns[currentPattern].notes,
                patterns[currentPattern].len
            );
            digitalWrite(LED_PIN, HIGH);
        }
        else if (simulatedTemp <= TEMP_THRESHOLD && alarmActive) {
            // Temperature back to normal — auto-dismiss
            dismissAlarm();
        }
    }

    // ── Check dismiss button (active LOW, debounced) ────────────────
    if (alarmActive && digitalRead(BUTTON_PIN) == LOW) {
        delay(50);  // Simple debounce
        if (digitalRead(BUTTON_PIN) == LOW) {
            dismissAlarm();
            // Wait for button release
            while (digitalRead(BUTTON_PIN) == LOW) delay(10);
        }
    }

    // ── Serial commands ─────────────────────────────────────────────
    if (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case '+':
                simulatedTemp += 1.0;
                printStatus();
                break;
            case '-':
                simulatedTemp -= 1.0;
                printStatus();
                break;
            case 'd':
            case 'D':
                if (alarmActive) dismissAlarm();
                break;
            case '1': case '2': case '3': case '4': {
                int idx = c - '1';
                if (idx < PATTERN_COUNT) {
                    currentPattern = idx;
                    Serial.print("Pattern: ");
                    Serial.println(patterns[idx].name);
                    // If alarm is active, switch pattern live
                    if (alarmActive) {
                        buzzer.playMelodyLoop(
                            patterns[currentPattern].notes,
                            patterns[currentPattern].len
                        );
                    }
                }
                break;
            }
        }
    }

    // ── LED blink during alarm ──────────────────────────────────────
    if (alarmActive) {
        digitalWrite(LED_PIN, (millis() / 300) % 2);
    }
}

void dismissAlarm() {
    alarmActive = false;
    buzzer.stopMelody();
    digitalWrite(LED_PIN, LOW);

    // Play confirmation beep
    buzzer.playMelody(confirmBeep, sizeof(confirmBeep) / sizeof(BuzzerNote));

    Serial.println("Alarm dismissed.");
    printStatus();
}

void printStatus() {
    Serial.print("Temp: ");
    Serial.print(simulatedTemp, 1);
    Serial.print(" C | Threshold: ");
    Serial.print(TEMP_THRESHOLD, 1);
    Serial.print(" C | Alarm: ");
    Serial.print(alarmActive ? "ACTIVE" : "off");
    Serial.print(" | Pattern: ");
    Serial.println(patterns[currentPattern].name);
}
