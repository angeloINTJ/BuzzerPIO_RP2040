# Contributing to BuzzerPIO_RP2040

Thank you for your interest in improving this library! Here's how to get started.

## How to Contribute

### Reporting Bugs

* Use the [Bug Report](https://github.com/angeloINTJ/BuzzerPIO_RP2040/issues/new?template=bug_report.md) issue template
* Include your board model, Arduino core version, and a minimal sketch to reproduce

### Suggesting Features

* Use the [Feature Request](https://github.com/angeloINTJ/BuzzerPIO_RP2040/issues/new?template=feature_request.md) issue template
* Explain the use case and expected behavior

### Submitting Code

1. **Fork** the repository
2. Create a **feature branch** from `main`:

   ```
   git checkout -b feature/your-feature-name
   ```
3. Make your changes following the code style below
4. **Test** on real hardware (Raspberry Pi Pico or Pico W)
5. **Commit** with a clear message:

   ```
   git commit -m "Add: brief description of what was added"
   ```
6. **Push** and open a **Pull Request**

## Code Style

* **Language**: C++ (Arduino-compatible)
* **Comments**: English, Doxygen-style (`@brief`, `@param`, `@return`) for public API
* **Indentation**: 4 spaces (no tabs)
* **Braces**: K&R style (opening brace on same line)
* **Naming**: `camelCase` for methods, `_camelCase` for private members, `UPPER_CASE` for constants
* **Casts**: Use C++ casts (`static_cast<>`) instead of C-style casts

## Commit Message Convention

```
Add:    new feature or file
Fix:    bug fix
Docs:   documentation only
Refactor: code change that doesn't fix a bug or add a feature
Test:   adding or updating examples
```

## What We're Looking For

* Polyphony support (multiple simultaneous tones via extra SMs)
* Predefined note frequency constants (NOTE_C4, NOTE_D4, etc.)
* RTTTL / MIDI melody format parsers
* Envelope control (attack/decay/sustain/release)
* RP2350 (Pico 2) testing and validation
* Documentation improvements and translations

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
