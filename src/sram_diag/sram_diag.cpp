// SPDX-License-Identifier: MIT
/*
 * SuperOS-303 SRAM Diagnostic Firmware
 *
 * File:
 *   sram_diag/sram_diag.cpp
 *
 * Purpose:
 *   Standalone diagnostic firmware for testing the TB-303 SRAM section using
 *   the SuperOS-303 / OS-303 Teensy++ 2.0 hardware mapping.
 *
 * Hardware model:
 *   - 3 x 2114-equivalent SRAMs
 *   - All address, data, and WE lines are shadowed across the three SRAMs
 *   - SRAM CE is selected externally by a 4556 decoder
 *   - Two upper memory-address-style lines select the active SRAM via 4556
 *
 * Important diagnostic hardware requirement:
 *   - Original TB-303 PI0 / CPU pin 30 is Memory WE
 *   - PI0 is not connected to the Teensy in this hardware
 *   - For SRAM diagnostic mode, fit a temporary jumper:
 *
 *       Teensy-driven PI1 signal
 *           ->
 *       TB-303 CPU pin 30 / Memory WE
 *
 *   This firmware therefore uses PI1_PIN as the SRAM write-enable signal.
 *
 * Display model:
 *   Status mode:
 *     C LED = SRAM 1
 *     D LED = SRAM 2
 *     E LED = SRAM 3
 *
 *     Off        = not yet tested
 *     Flashing   = currently testing or failed
 *     Solid on   = passed
 *
 *   WE self-test failure:
 *     DOWN, UP, ACCENT, SLIDE flash together
 *     Full RAM test is not run
 *
 *   Diagnostic page 1:
 *     Bits 0-9   = first failing local address
 *     Bits 10-11 = failed chip index, zero-based
 *
 *   Diagnostic page 2:
 *     Bits 0-3   = expected nibble
 *     Bits 4-7   = actual nibble
 *     Bits 8-11  = expected XOR actual
 *
 * 12-bit diagnostic LED order:
 *   bit 0  = C
 *   bit 1  = D
 *   bit 2  = E
 *   bit 3  = F
 *   bit 4  = G
 *   bit 5  = A
 *   bit 6  = B
 *   bit 7  = C2
 *   bit 8  = DOWN
 *   bit 9  = UP
 *   bit 10 = ACCENT
 *   bit 11 = SLIDE
 *
 * Button:
 *   FUNCTION cycles display mode:
 *     status -> address/chip diagnostic -> data diagnostic -> status
 */

#include <Arduino.h>
#include "../src/pins.h"

// -----------------------------------------------------------------------------
// SRAM configuration
// -----------------------------------------------------------------------------

constexpr uint8_t SRAM_CHIP_COUNT = 3;
constexpr uint16_t SRAM_ADDRESS_COUNT = 1024;

// Assumed TB-303 memory mapping:
//
//   PC0-PC3 = 4-bit memory data I/O
//   PD0-PD3 = local address A0-A3
//   PF0-PF3 = local address A4-A7
//   PE0-PE1 = local address A8-A9
//   PE2-PE3 = 4556 select inputs for SRAM CE
//
// If your 4556 inputs are wired differently, change only
// SRAM_CHIP_SELECT_PINS.

const uint8_t SRAM_DATA_PINS[4] = {
  PC0_PIN,   // SRAM D0
  PC1_PIN,   // SRAM D1
  PC2_PIN,   // SRAM D2
  PC3_PIN    // SRAM D3
};

const uint8_t SRAM_ADDRESS_PINS[10] = {
  PD0_PIN,   // SRAM A0
  PD1_PIN,   // SRAM A1
  PD2_PIN,   // SRAM A2
  PD3_PIN,   // SRAM A3
  PF0_PIN,   // SRAM A4
  PF1_PIN,   // SRAM A5
  PF2_PIN,   // SRAM A6
  PF3_PIN,   // SRAM A7
  PE0_PIN,   // SRAM A8
  PE1_PIN    // SRAM A9
};

const uint8_t SRAM_CHIP_SELECT_PINS[2] = {
  PE2_PIN,   // 4556 select bit 0
  PE3_PIN    // 4556 select bit 1
};

// PI1 is used as the diagnostic WE driver.
// Fit temporary jumper from PI1 signal to TB-303 CPU pin 30 / Memory WE.
const uint8_t SRAM_WE_PIN = PI1_PIN;

// 2114 /WE is active low.
constexpr bool SRAM_WE_ACTIVE_LOW = true;

// Full 4-bit pattern sweep.
// This tests all possible nibbles, 0000 through 1111.
const uint8_t TEST_PATTERNS[] = {
  0x0, 0x1, 0x2, 0x3,
  0x4, 0x5, 0x6, 0x7,
  0x8, 0x9, 0xA, 0xB,
  0xC, 0xD, 0xE, 0xF
};

constexpr uint8_t TEST_PATTERN_COUNT =
  sizeof(TEST_PATTERNS) / sizeof(TEST_PATTERNS[0]);

// -----------------------------------------------------------------------------
// UI and result state
// -----------------------------------------------------------------------------

enum TestState : uint8_t {
  TEST_NOT_RUN = 0,
  TEST_RUNNING = 1,
  TEST_PASS    = 2,
  TEST_FAIL    = 3
};

enum DisplayPage : uint8_t {
  PAGE_STATUS       = 0,
  PAGE_DIAG_ADDRESS = 1,
  PAGE_DIAG_DATA    = 2
};

enum FatalFault : uint8_t {
  FAULT_NONE = 0,
  FAULT_WE_SELF_TEST = 1
};

struct ChipResult {
  TestState state;
  bool failed;
  uint16_t firstFailAddress;
  uint8_t expected;
  uint8_t actual;
};

ChipResult chipResults[SRAM_CHIP_COUNT];

FatalFault fatalFault = FAULT_NONE;
DisplayPage currentPage = PAGE_STATUS;
uint8_t selectedFailChip = 0;

uint16_t ledMask12 = 0;
uint8_t ledScanCursor = 0;

unsigned long lastBlinkMs = 0;
bool blinkPhase = false;

unsigned long lastButtonPollMs = 0;
bool lastFunctionPressed = false;

// First 12 switched LEDs from pins.h.
// These are the exact LEDs used for the 12-bit diagnostic display.
const OutputIndex DIAG_LEDS[12] = {
  C_KEY_LED,
  D_KEY_LED,
  E_KEY_LED,
  F_KEY_LED,
  G_KEY_LED,
  A_KEY_LED,
  B_KEY_LED,
  C_KEY2_LED,
  DOWN_KEY_LED,
  UP_KEY_LED,
  ACCENT_KEY_LED,
  SLIDE_KEY_LED
};

// C, D, E are the three chip status LEDs.
const OutputIndex STATUS_LEDS[SRAM_CHIP_COUNT] = {
  C_KEY_LED,
  D_KEY_LED,
  E_KEY_LED
};

// Diagnostic bit masks.
constexpr uint16_t MASK_ALL_12        = 0x0FFF;
constexpr uint16_t MASK_WE_FAULT_LEDS = (1U << 8) | (1U << 9) | (1U << 10) | (1U << 11);

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

void setupMatrixPins();
void setupButtonPins();
void setupSramPins();

void allMatrixOff();
void serviceLedScan();

void serviceUi();
void pollButtons();
void renderUi();
void renderStatusPage();
void renderDiagAddressPage();
void renderDiagDataPage();
void renderFatalFault();

void clearResults();
bool runWriteEnableSelfTest();
bool testWriteEnableOnChip(uint8_t chip);
void runAllTests();
bool testOneChip(uint8_t chip);
uint8_t findFirstFailedChip();
bool anyChipFailed();

void selectSramChip(uint8_t chip);
void setSramAddress(uint16_t address);
void setDataBusOutput();
void setDataBusInput();
void writeDataBus(uint8_t value);
uint8_t readDataBus();
void enableWrite();
void disableWrite();
void writeSram(uint8_t chip, uint16_t address, uint8_t data);
uint8_t readSram(uint8_t chip, uint16_t address);

void setLedMaskBit(OutputIndex led, bool on);
void startupLampTest();

// -----------------------------------------------------------------------------
// Matrix output
// -----------------------------------------------------------------------------

void allMatrixOff() {
  // PH lines are matrix selectors. Keep all inactive here.
  digitalWrite(PH0_PIN, HIGH);
  digitalWrite(PH1_PIN, HIGH);
  digitalWrite(PH2_PIN, HIGH);
  digitalWrite(PH3_PIN, HIGH);

  // PG lines drive switched LEDs. Keep all off here.
  digitalWrite(PG0_PIN, LOW);
  digitalWrite(PG1_PIN, LOW);
  digitalWrite(PG2_PIN, LOW);
  digitalWrite(PG3_PIN, LOW);
}

void setupMatrixPins() {
  pinMode(PH0_PIN, OUTPUT);
  pinMode(PH1_PIN, OUTPUT);
  pinMode(PH2_PIN, OUTPUT);
  pinMode(PH3_PIN, OUTPUT);

  pinMode(PG0_PIN, OUTPUT);
  pinMode(PG1_PIN, OUTPUT);
  pinMode(PG2_PIN, OUTPUT);
  pinMode(PG3_PIN, OUTPUT);

  // PC0-PC3 are intentionally not used as normal direct LEDs in this diagnostic.
  // They are used as the SRAM data bus.
  allMatrixOff();
}

// Drive one switched LED from the SuperOS / OS-303 matrix table.
// This diagnostic only uses switched LEDs 0 to 11.
void driveOneSwitchedLed(OutputIndex ledIndex) {
  if (ledIndex >= 16) {
    return;
  }

  const MatrixPin &m = switched_leds[ledIndex];

  allMatrixOff();

  // The matrix uses a PH select and a PG drive line.
  digitalWrite(m.select, LOW);
  digitalWrite(m.led, HIGH);
}

// Multiplex one active diagnostic LED per call.
// Call this frequently from loops and during blocking test code.
void serviceLedScan() {
  allMatrixOff();

  for (uint8_t tries = 0; tries < 12; tries++) {
    ledScanCursor++;
    if (ledScanCursor >= 12) {
      ledScanCursor = 0;
    }

    if (ledMask12 & (1U << ledScanCursor)) {
      driveOneSwitchedLed(DIAG_LEDS[ledScanCursor]);
      return;
    }
  }
}

// -----------------------------------------------------------------------------
// Button input
// -----------------------------------------------------------------------------

void setupButtonPins() {
  pinMode(PA0_PIN, INPUT_PULLUP);
  pinMode(PA1_PIN, INPUT_PULLUP);
  pinMode(PA2_PIN, INPUT_PULLUP);
  pinMode(PA3_PIN, INPUT_PULLUP);

  pinMode(PB0_PIN, INPUT_PULLUP);
  pinMode(PB1_PIN, INPUT_PULLUP);
  pinMode(PB2_PIN, INPUT_PULLUP);
  pinMode(PB3_PIN, INPUT_PULLUP);
}

// FUNCTION_KEY is PA1 with PH2 selected in the OS-303 style matrix.
// If your fork changed the input mapping, adjust this function only.
bool readFunctionButtonRaw() {
  allMatrixOff();

  digitalWrite(PH2_PIN, LOW);
  delayMicroseconds(20);

  bool pressed = (digitalRead(PA1_PIN) == LOW);

  allMatrixOff();
  return pressed;
}

void pollButtons() {
  unsigned long now = millis();

  if ((now - lastButtonPollMs) < 20) {
    return;
  }

  lastButtonPollMs = now;

  bool functionPressed = readFunctionButtonRaw();

  if (functionPressed && !lastFunctionPressed) {
    if (currentPage == PAGE_STATUS) {
      currentPage = PAGE_DIAG_ADDRESS;
    } else if (currentPage == PAGE_DIAG_ADDRESS) {
      currentPage = PAGE_DIAG_DATA;
    } else {
      currentPage = PAGE_STATUS;
    }
  }

  lastFunctionPressed = functionPressed;
}

// -----------------------------------------------------------------------------
// SRAM bus setup and control
// -----------------------------------------------------------------------------

void setupSramPins() {
  // Address outputs.
  for (uint8_t i = 0; i < 10; i++) {
    pinMode(SRAM_ADDRESS_PINS[i], OUTPUT);
    digitalWrite(SRAM_ADDRESS_PINS[i], LOW);
  }

  // Data bus starts as input.
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(SRAM_DATA_PINS[i], LOW);
    pinMode(SRAM_DATA_PINS[i], INPUT);
  }

  // 4556 chip select address lines.
  for (uint8_t i = 0; i < 2; i++) {
    pinMode(SRAM_CHIP_SELECT_PINS[i], OUTPUT);
    digitalWrite(SRAM_CHIP_SELECT_PINS[i], LOW);
  }

  // PI1 is used as the diagnostic SRAM WE driver.
  pinMode(SRAM_WE_PIN, OUTPUT);
  disableWrite();
}

void disableWrite() {
  digitalWrite(SRAM_WE_PIN, SRAM_WE_ACTIVE_LOW ? HIGH : LOW);
}

void enableWrite() {
  digitalWrite(SRAM_WE_PIN, SRAM_WE_ACTIVE_LOW ? LOW : HIGH);
}

void selectSramChip(uint8_t chip) {
  // chip 0 -> 00
  // chip 1 -> 01
  // chip 2 -> 10
  // chip 3 -> 11, unused
  digitalWrite(SRAM_CHIP_SELECT_PINS[0], bitRead(chip, 0));
  digitalWrite(SRAM_CHIP_SELECT_PINS[1], bitRead(chip, 1));

  // Small margin for the 4556 decoder and CE settling.
  delayMicroseconds(1);
}

void setSramAddress(uint16_t address) {
  for (uint8_t i = 0; i < 10; i++) {
    digitalWrite(SRAM_ADDRESS_PINS[i], bitRead(address, i));
  }
}

void setDataBusOutput() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(SRAM_DATA_PINS[i], OUTPUT);
  }
}

void setDataBusInput() {
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(SRAM_DATA_PINS[i], LOW);
    pinMode(SRAM_DATA_PINS[i], INPUT);
  }
}

void writeDataBus(uint8_t value) {
  value &= 0x0F;

  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(SRAM_DATA_PINS[i], bitRead(value, i));
  }
}

uint8_t readDataBus() {
  uint8_t value = 0;

  for (uint8_t i = 0; i < 4; i++) {
    bitWrite(value, i, digitalRead(SRAM_DATA_PINS[i]) ? 1 : 0);
  }

  return value & 0x0F;
}

void writeSram(uint8_t chip, uint16_t address, uint8_t data) {
  disableWrite();

  selectSramChip(chip);
  setSramAddress(address);

  setDataBusOutput();
  writeDataBus(data);

  delayMicroseconds(1);

  enableWrite();
  delayMicroseconds(1);
  disableWrite();

  delayMicroseconds(1);
}

uint8_t readSram(uint8_t chip, uint16_t address) {
  disableWrite();

  selectSramChip(chip);
  setSramAddress(address);

  setDataBusInput();

  delayMicroseconds(1);

  return readDataBus();
}

// -----------------------------------------------------------------------------
// Write-enable self-test
// -----------------------------------------------------------------------------

bool testWriteEnableOnChip(uint8_t chip) {
  // Test a few addresses so a single bad memory cell does not falsely look like
  // a missing WE jumper.
  const uint16_t testAddresses[] = {
    0x000,
    0x001,
    0x155,
    0x2AA,
    0x3FF
  };

  const uint8_t testValues[] = {
    0x0,
    0xF,
    0x5,
    0xA
  };

  const uint8_t addressCount =
    sizeof(testAddresses) / sizeof(testAddresses[0]);

  const uint8_t valueCount =
    sizeof(testValues) / sizeof(testValues[0]);

  for (uint8_t a = 0; a < addressCount; a++) {
    uint16_t addr = testAddresses[a];

    for (uint8_t v = 0; v < valueCount; v++) {
      uint8_t expected = testValues[v] & 0x0F;

      writeSram(chip, addr, expected);
      serviceUi();

      uint8_t actual = readSram(chip, addr) & 0x0F;
      serviceUi();

      if (actual != expected) {
        return false;
      }
    }
  }

  return true;
}

bool runWriteEnableSelfTest() {
  // Try the WE self-test on all three chips.
  //
  // If at least one chip can be written and read back reliably, then the WE
  // jumper and write path are probably working. Individual chip faults will be
  // caught by the full per-chip RAM test later.
  //
  // If no chip can be written, the most likely causes are:
  //   - PI1 to Memory WE jumper missing
  //   - jumper connected to the wrong point
  //   - WE line held inactive
  //   - shared data/address bus fault
  //   - failed decoder or no active SRAM selected
  //
  // This is intentionally a broad preflight check, not a full fault classifier.

  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    chipResults[chip].state = TEST_RUNNING;

    if (testWriteEnableOnChip(chip)) {
      clearResults();
      return true;
    }

    chipResults[chip].state = TEST_FAIL;
    chipResults[chip].failed = true;
    chipResults[chip].firstFailAddress = 0;
    chipResults[chip].expected = 0xF;
    chipResults[chip].actual = readSram(chip, 0) & 0x0F;

    serviceUi();
  }

  fatalFault = FAULT_WE_SELF_TEST;
  currentPage = PAGE_STATUS;
  return false;
}

// -----------------------------------------------------------------------------
// Test logic
// -----------------------------------------------------------------------------

void clearResults() {
  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    chipResults[chip].state = TEST_NOT_RUN;
    chipResults[chip].failed = false;
    chipResults[chip].firstFailAddress = 0;
    chipResults[chip].expected = 0;
    chipResults[chip].actual = 0;
  }

  selectedFailChip = 0;
}

bool testOneChip(uint8_t chip) {
  chipResults[chip].state = TEST_RUNNING;
  currentPage = PAGE_STATUS;

  // For each pattern:
  //   1. Write the whole SRAM chip.
  //   2. Read the whole SRAM chip back.
  //
  // This is better than immediate write/read at the same address because it is
  // more likely to catch address aliasing and line-select faults.
  for (uint8_t p = 0; p < TEST_PATTERN_COUNT; p++) {
    uint8_t expected = TEST_PATTERNS[p] & 0x0F;

    for (uint16_t addr = 0; addr < SRAM_ADDRESS_COUNT; addr++) {
      writeSram(chip, addr, expected);

      if ((addr & 0x0007) == 0) {
        serviceUi();
      }
    }

    for (uint16_t addr = 0; addr < SRAM_ADDRESS_COUNT; addr++) {
      uint8_t actual = readSram(chip, addr) & 0x0F;

      if (actual != expected) {
        chipResults[chip].state = TEST_FAIL;
        chipResults[chip].failed = true;
        chipResults[chip].firstFailAddress = addr;
        chipResults[chip].expected = expected;
        chipResults[chip].actual = actual;

        selectedFailChip = chip;
        return false;
      }

      if ((addr & 0x0007) == 0) {
        serviceUi();
      }
    }
  }

  chipResults[chip].state = TEST_PASS;
  return true;
}

void runAllTests() {
  clearResults();

  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    testOneChip(chip);
    serviceUi();
  }

  selectedFailChip = findFirstFailedChip();
  currentPage = PAGE_STATUS;
}

uint8_t findFirstFailedChip() {
  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    if (chipResults[chip].failed) {
      return chip;
    }
  }

  return 0;
}

bool anyChipFailed() {
  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    if (chipResults[chip].failed) {
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------------------------------
// UI rendering
// -----------------------------------------------------------------------------

void updateBlink() {
  unsigned long now = millis();

  if ((now - lastBlinkMs) >= 250) {
    lastBlinkMs = now;
    blinkPhase = !blinkPhase;
  }
}

void setLedMaskBit(OutputIndex led, bool on) {
  for (uint8_t i = 0; i < 12; i++) {
    if (DIAG_LEDS[i] == led) {
      if (on) {
        ledMask12 |= (1U << i);
      } else {
        ledMask12 &= ~(1U << i);
      }
      return;
    }
  }
}

void renderFatalFault() {
  switch (fatalFault) {
    case FAULT_WE_SELF_TEST:
      // WE fault indication:
      // DOWN, UP, ACCENT, SLIDE flash together.
      ledMask12 = blinkPhase ? MASK_WE_FAULT_LEDS : 0x0000;
      break;

    case FAULT_NONE:
    default:
      break;
  }
}

void renderStatusPage() {
  ledMask12 = 0;

  if (fatalFault != FAULT_NONE) {
    renderFatalFault();
    return;
  }

  for (uint8_t chip = 0; chip < SRAM_CHIP_COUNT; chip++) {
    bool on = false;

    switch (chipResults[chip].state) {
      case TEST_NOT_RUN:
        on = false;
        break;

      case TEST_RUNNING:
        on = blinkPhase;
        break;

      case TEST_PASS:
        on = true;
        break;

      case TEST_FAIL:
        on = blinkPhase;
        break;
    }

    setLedMaskBit(STATUS_LEDS[chip], on);
  }
}

void renderDiagAddressPage() {
  if (fatalFault == FAULT_WE_SELF_TEST) {
    // Diagnostic code for WE self-test failure.
    //
    // Bits 8-11 on indicates this is not a normal chip/address failure.
    // Bits 0-3 set to 0001 identifies WE self-test fault code 1.
    ledMask12 = MASK_WE_FAULT_LEDS | 0x0001;
    return;
  }

  if (!anyChipFailed()) {
    // No failure to show. All 12 LEDs on means "no diagnostic fault record".
    ledMask12 = MASK_ALL_12;
    return;
  }

  const ChipResult &r = chipResults[selectedFailChip];

  uint16_t packed = 0;

  // bits 0-9 = first failing local address.
  packed |= (r.firstFailAddress & 0x03FF);

  // bits 10-11 = chip index, zero-based.
  packed |= ((uint16_t)(selectedFailChip & 0x03) << 10);

  ledMask12 = packed & MASK_ALL_12;
}

void renderDiagDataPage() {
  if (fatalFault == FAULT_WE_SELF_TEST) {
    // Same WE fault indication on data page, but solid instead of flashing.
    ledMask12 = MASK_WE_FAULT_LEDS | 0x0001;
    return;
  }

  if (!anyChipFailed()) {
    // No failure to show. All 12 LEDs on means "no diagnostic fault record".
    ledMask12 = MASK_ALL_12;
    return;
  }

  const ChipResult &r = chipResults[selectedFailChip];

  uint8_t expected = r.expected & 0x0F;
  uint8_t actual = r.actual & 0x0F;
  uint8_t errorMask = (expected ^ actual) & 0x0F;

  uint16_t packed = 0;

  // bits 0-3 = expected.
  packed |= expected;

  // bits 4-7 = actual.
  packed |= ((uint16_t)actual << 4);

  // bits 8-11 = expected XOR actual.
  packed |= ((uint16_t)errorMask << 8);

  ledMask12 = packed & MASK_ALL_12;
}

void renderUi() {
  updateBlink();

  switch (currentPage) {
    case PAGE_STATUS:
      renderStatusPage();
      break;

    case PAGE_DIAG_ADDRESS:
      renderDiagAddressPage();
      break;

    case PAGE_DIAG_DATA:
      renderDiagDataPage();
      break;
  }
}

void serviceUi() {
  pollButtons();
  renderUi();
  serviceLedScan();
}

void startupLampTest() {
  // Blink all 12 LEDs twice.
  for (uint8_t i = 0; i < 2; i++) {
    ledMask12 = MASK_ALL_12;

    unsigned long startOn = millis();
    while ((millis() - startOn) < 250) {
      serviceLedScan();
    }

    ledMask12 = 0x0000;

    unsigned long startOff = millis();
    while ((millis() - startOff) < 250) {
      serviceLedScan();
    }
  }
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------

void setup() {
  setupMatrixPins();
  setupButtonPins();
  setupSramPins();

  clearResults();

  startupLampTest();

  if (runWriteEnableSelfTest()) {
    runAllTests();
  } else {
    // Do not continue into the full destructive write/read test if WE does not
    // appear to work. The loop will continue to display the fault pattern.
    currentPage = PAGE_STATUS;
  }
}

void loop() {
  serviceUi();
}