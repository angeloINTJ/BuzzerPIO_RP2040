/**
 * @file    MelodyPlayer.ino
 * @brief   BuzzerPIO_RP2040 — Non-blocking melody playback example.
 *
 * Demonstrates:
 *   - Defining melodies as BuzzerNote arrays
 *   - One-shot melody playback with playMelody()
 *   - Using freq=0 for silent pauses within a melody
 *   - Checking playback status with isPlaying()
 *   - Playing multiple melodies in sequence
 *   - v2.5: Melody completion callback (setMelodyDoneCallback)
 *   - v2.5: Pause/resume melody playback
 *
 * Wiring:
 *   - Passive buzzer between GP22 and GND
 *
 * @note Melodies play entirely in hardware alarm chain. The CPU is
 *       free to do other work while the melody plays. No update()
 *       or polling loop is needed.
 */

#include <BuzzerPIO_RP2040.h>

BuzzerPIO buzzer(22, pio0);

// ── Melody completion flag (set from IRQ, read from loop) ────────────
volatile bool melodyFinished = false;

/**
 * @brief  Callback fired when a one-shot melody finishes.
 *
 * @warning Runs in IRQ context — only set a flag here.
 *          Do NOT call Serial, delay(), or any blocking function.
 */
void onMelodyDone(void* /* userData */) {
    melodyFinished = true;
}

// ── Melody definitions ───────────────────────────────────────────────
// Each note: { frequency_Hz, duration_ms }
// Use { 0, duration } for silent pauses.

// Success chime (ascending C-E-G)
const BuzzerNote melodySuccess[] = {
    { 523, 100 },   // C5
    { 659, 100 },   // E5
    { 784, 150 }    // G5
};

// Error sound (descending)
const BuzzerNote melodyError[] = {
    { 400, 150 },
    { 250, 220 }
};

// Notification (ding-dong)
const BuzzerNote melodyNotify[] = {
    { 1200,  80 },   // Ding
    {    0,  60 },   // Pause
    { 1500, 120 }    // Dong
};

// Mario-style coin
const BuzzerNote melodyCoin[] = {
    { 988,  80 },   // B5
    { 1319, 300 }   // E6
};

// Scale (C major, one octave)
const BuzzerNote melodyScale[] = {
    { 523, 150 },   // C5
    { 587, 150 },   // D5
    { 659, 150 },   // E5
    { 698, 150 },   // F5
    { 784, 150 },   // G5
    { 880, 150 },   // A5
    { 988, 150 },   // B5
    { 1047, 300 }   // C6
};

// Fanfare (triumphant)
const BuzzerNote melodyFanfare[] = {
    { 523,  80 },   // C5
    { 523,  80 },   // C5 (repeat)
    { 523,  80 },   // C5 (repeat)
    {   0,  40 },   // Short pause
    { 659, 120 },   // E5
    {   0,  40 },
    { 784, 100 },   // G5
    {   0,  40 },
    { 1047, 250 }   // C6 (resolution)
};


// ── Melody table for easy iteration ──────────────────────────────────
struct MelodyEntry {
    const char*       name;
    const BuzzerNote* notes;
    uint16_t          len;
};

const MelodyEntry allMelodies[] = {
    { "Success",      melodySuccess,  sizeof(melodySuccess)  / sizeof(BuzzerNote) },
    { "Error",        melodyError,    sizeof(melodyError)    / sizeof(BuzzerNote) },
    { "Notification", melodyNotify,   sizeof(melodyNotify)   / sizeof(BuzzerNote) },
    { "Coin",         melodyCoin,     sizeof(melodyCoin)     / sizeof(BuzzerNote) },
    { "C Major",      melodyScale,    sizeof(melodyScale)    / sizeof(BuzzerNote) },
    { "Fanfare",      melodyFanfare,  sizeof(melodyFanfare)  / sizeof(BuzzerNote) }
};

const int MELODY_COUNT = sizeof(allMelodies) / sizeof(MelodyEntry);

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);

    Serial.println("BuzzerPIO_RP2040 — MelodyPlayer Example");
    Serial.println("========================================\n");

    if (!buzzer.begin()) {
        Serial.println("ERROR: PIO init failed! Need 4 instr + 2 SMs.");
        while (true) delay(1000);
    }

    buzzer.setVolume(70);

    // ── Register melody completion callback ──────────────────────────
    // The callback sets melodyFinished=true (from IRQ), so we can
    // react in loop() without polling isPlaying().
    buzzer.setMelodyDoneCallback(onMelodyDone);

    // ── Play each melody using the callback to detect completion ─────
    for (int i = 0; i < MELODY_COUNT; i++) {
        Serial.print("Playing: ");
        Serial.print(allMelodies[i].name);
        Serial.print(" (");
        Serial.print(allMelodies[i].len);
        Serial.println(" notes)...");

        melodyFinished = false;
        buzzer.playMelody(allMelodies[i].notes, allMelodies[i].len);

        // Wait for completion callback (CPU is free between checks!)
        while (!melodyFinished) {
            // Could do sensor reads, display updates, etc. here
            delay(10);
        }

        Serial.println("  Done (callback received).");
        delay(800);  // Pause between melodies
    }

    // ── Demonstrate pause/resume with the scale melody ───────────────
    Serial.println("\n--- Pause/Resume Demo ---");
    Serial.println("Playing C Major scale, will pause mid-way...\n");

    melodyFinished = false;
    buzzer.playMelody(melodyScale, sizeof(melodyScale) / sizeof(BuzzerNote));

    // Let it play a few notes, then pause
    delay(500);
    Serial.println("  PAUSED (isPaused = true)");
    buzzer.pauseMelody();
    delay(1500);  // Silence for 1.5 seconds

    Serial.println("  RESUMED");
    buzzer.resumeMelody();

    // Wait for it to finish
    while (!melodyFinished) delay(10);
    Serial.println("  Scale finished after resume.\n");

    Serial.println("All demos complete!");
    Serial.println("Send 1-6 via Serial to replay a melody.");
}

void loop() {
    // Interactive replay via Serial
    if (Serial.available()) {
        char c = Serial.read();
        int idx = c - '1';

        if (idx >= 0 && idx < MELODY_COUNT) {
            Serial.print("Replaying: ");
            Serial.println(allMelodies[idx].name);
            buzzer.playMelody(allMelodies[idx].notes, allMelodies[idx].len);
        }
    }
}
