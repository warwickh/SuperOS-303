// combined.h -- combined SuperOS-303 + D650C-emulator build (SUPEROS_COMBINED).
//
// One image, one firmware active per boot, selected by a byte in the internal
// EEPROM and switchable from either firmware's FUNCTION+CLEAR config menu.
// SuperOS keeps its patterns in the flash arena; the d650c side keeps ALL of
// its state (uPD444 pattern store + settings) in the 4 KB internal EEPROM,
// which SuperOS never touches.
//
// Internal EEPROM map (AT90USB1286, 4096 bytes):
//   0x000            firmware select: 0/0xFF = SuperOS, 1 = D650C
//   0x001            d650 area magic (EE_EMU_MAGIC_VAL when initialized)
//   0x002            mask-ROM magic (EE_ROM_MAGIC_VAL when a valid ROM is stored)
//   0x003..0x004     mask-ROM sum16 (LE) over the 2048 ROM bytes
//   0x008..0x017     d650 settings blob (EMU_SETTINGS_LEN = 16)
//   0x020..0x61F     d650 uPD444 packed pattern store (D650_EXT_BYTES = 1536)
//   0x620..0xE1F     user-loaded D650C mask ROM (2048 bytes; the D650_ROM_IN_RAM
//                    build lets the user upload their own dump via SysEx over
//                    USB-C or DIN -- see src/d650/rom_store.h)
//   0xE20..0xFEF     free
//   0xFF0..0xFF3     TEMP wedge diagnostics (combined.cpp): boot crumb, 1 Hz
//                    heartbeat, boot counter, WDT-recovery counter. These were
//                    BARE ADDRESS LITERALS in combined.cpp and appeared in no
//                    map, which is the shape of the fault A808 found twice in
//                    one instrument on 2026-08-26: a diagnostic block silently
//                    overlapping real data, corrupting it slowly, with the
//                    symptom nowhere near the cause. Named and asserted here.
#pragma once
#include <stdint.h>

// THE INTEGERS ARE THE MAP. The pointers below are DERIVED from them, and that
// is the whole point of the ordering: this block used to define the pointers as
// literals and the integers as a parallel copy, with a comment saying "keep
// these equal". A comment is not a check. If the two had ever drifted, the
// static_asserts would have gone on checking the copy while the code used the
// pointers, which is a guard that passes while the thing it guards is wrong.
// (A303, 2026-08-26, on A808's report that both of the 808's remaining EEPROM
// overlaps came from addresses at call sites that no assert mentioned.)
//
// The asserts must sit on the INTEGERS: a cast from a pointer to an integer is
// not a constant expression, so a static_assert on the pointer form is not a
// check. It compiles on avr-gcc as an extension and fails under clang, so an
// AVR-only tree can carry those asserts passing by luck (A101 hit the clang
// half, A808 measured the avr-gcc half).
static constexpr uint16_t EE_SIZE          = 4096;
static constexpr uint16_t EE_OFF_FW_SELECT = 0x000;
static constexpr uint16_t EE_OFF_EMU_MAGIC = 0x001;
static constexpr uint16_t EE_OFF_ROM_MAGIC = 0x002;
static constexpr uint16_t EE_OFF_ROM_SUM   = 0x003;
static constexpr uint16_t EE_OFF_SETTINGS  = 0x008;
static constexpr uint16_t EE_OFF_PATT      = 0x020;
static constexpr uint16_t EE_OFF_ROM       = 0x620;
static constexpr uint16_t EE_OFF_DIAG      = 0xFF0;
static constexpr uint16_t EE_DIAG_LEN      = 4;

// EE_ROM_SUM IS TWO BYTES WIDE and until 2026-08-26 nothing said so: the map
// listed it as one address and rom_store.h writes EE_ROM_SUM and EE_ROM_SUM+1.
// Byte 0x004 was therefore free-looking and reachable, and taking it would have
// clobbered the checksum's high byte with every assert still passing.
static constexpr uint16_t EE_ROM_SUM_LEN   = 2;

// The four diagnostic bytes, by NAME, in the map. They used to be defined only
// in combined.cpp as EE_DIAG_BASE + 0..3, so EE_DIAG_LEN = 4 was a hand-kept
// coincidence in a different file: a fifth crumb at +4 would not have moved it
// and the region assert would have gone on passing. The offsets live here now
// and the assert is against the highest one, not against a remembered count.
static constexpr uint16_t EE_OFF_BOOT_CRUMB = EE_OFF_DIAG + 0;
static constexpr uint16_t EE_OFF_HEARTBEAT  = EE_OFF_DIAG + 1;
static constexpr uint16_t EE_OFF_BOOT_COUNT = EE_OFF_DIAG + 2;
static constexpr uint16_t EE_OFF_WDT_COUNT  = EE_OFF_DIAG + 3;
static constexpr uint16_t EE_DIAG_HIGHEST   = EE_OFF_WDT_COUNT;

#define EE_PTR(off)    ((uint8_t *)(uintptr_t)(off))
#define EE_FW_SELECT   EE_PTR(EE_OFF_FW_SELECT)
#define EE_EMU_MAGIC   EE_PTR(EE_OFF_EMU_MAGIC)
#define EE_ROM_MAGIC   EE_PTR(EE_OFF_ROM_MAGIC)
#define EE_ROM_SUM     EE_PTR(EE_OFF_ROM_SUM)
#define EE_EMU_SETTINGS EE_PTR(EE_OFF_SETTINGS)
#define EE_EMU_PATT    EE_PTR(EE_OFF_PATT)
#define EE_ROM_DATA    EE_PTR(EE_OFF_ROM)
#define EE_DIAG_BASE   EE_PTR(EE_OFF_DIAG)

// The head region (fw select, the two magics, the ROM checksum) had no integer
// twins and no assert at all: four bare address literals appearing in no map,
// which is the same thing claim-036 found in the diagnostic bytes. Asserted now.
static_assert(EE_OFF_FW_SELECT == 0,           "fw select must be EEPROM byte 0");
static_assert(EE_OFF_EMU_MAGIC == EE_OFF_FW_SELECT + 1, "emu magic follows fw select");
static_assert(EE_OFF_ROM_MAGIC == EE_OFF_EMU_MAGIC + 1, "rom magic follows emu magic");
static_assert(EE_OFF_ROM_SUM   == EE_OFF_ROM_MAGIC + 1, "rom sum follows rom magic");
static_assert(EE_OFF_ROM_SUM + EE_ROM_SUM_LEN <= EE_OFF_SETTINGS,
              "the head (incl. the 2-byte ROM checksum) runs into settings");
static_assert(EE_OFF_SETTINGS  <  EE_OFF_PATT, "settings run into the pattern store");
static_assert(EE_OFF_PATT      <  EE_OFF_ROM,  "pattern store runs into the mask ROM");
static_assert(EE_OFF_ROM       <  EE_OFF_DIAG, "mask ROM runs into the diagnostics");
static_assert(EE_OFF_DIAG + EE_DIAG_LEN <= EE_SIZE, "diagnostics run off the end");
static_assert(EE_DIAG_HIGHEST < EE_OFF_DIAG + EE_DIAG_LEN,
              "a diagnostic byte sits past EE_DIAG_LEN");

#define EE_BOOT_CRUMB  EE_PTR(EE_OFF_BOOT_CRUMB)
#define EE_HEARTBEAT   EE_PTR(EE_OFF_HEARTBEAT)
#define EE_BOOT_COUNT  EE_PTR(EE_OFF_BOOT_COUNT)
#define EE_WDT_COUNT   EE_PTR(EE_OFF_WDT_COUNT)

static constexpr uint8_t FW_SUPEROS = 0;   // also 0xFF (virgin EEPROM)
static constexpr uint8_t FW_D650    = 1;
// Bump when d650 settings defaults/semantics change (0x67: pitch_base 25,
// factory pitch standard). Mismatch re-defaults settings AND re-seeds the store.
static constexpr uint8_t EE_EMU_MAGIC_VAL = 0x67;
// Written LAST by rom_save (invalidated first) so a torn ROM upload never
// validates at boot.
static constexpr uint8_t EE_ROM_MAGIC_VAL = 0x6D;

// Select the other firmware and reboot through the bootloader's app entry.
// Defined in combined.cpp; callable from either firmware's config menu.
void combined_switch_firmware(uint8_t fw);

// RAM overlay: only one firmware runs per boot, so the two biggest per-side
// objects (SuperOS Engine, d650 machine incl. the 1536-byte uPD444 store)
// share this arena. Sized in combined.cpp to max(sizeof(Engine),
// sizeof(d650_host)); zeroed BSS at every boot, exactly like the statics it
// replaces (Engine is placement-new'ed in superos_setup, d650_init memsets).
extern uint8_t g_fw_arena[];

// SuperOS-only scratch carved out of the arena TAIL, at offset sizeof(Engine).
// The d650 side sizes the arena larger than Engine (it needs d650_host plus the
// 2 KB SRAM mask ROM under D650_ROM_IN_RAM), so those tail bytes are dead space
// whenever SuperOS is the running firmware. Putting a SuperOS-only buffer there
// costs no RAM and gives back what D650_ROM_IN_RAM took from the stack -- which
// the SysEx senders need (the poly reply alone frames 492 bytes of locals).
// Only valid while SuperOS runs: in D650C mode these bytes hold the mask ROM.
// combined.cpp static_asserts that the arena really has this much slack.
static constexpr unsigned FW_ARENA_SUPEROS_TAIL = 512;   // midi.cpp SysEx TX ring

// Inbound USB SysEx reassembly scratch, shared between the two firmwares: the
// AVR usb_midi core's own complete-message buffer is only 60 bytes, so each
// side reassembles oversized messages itself. Only one firmware runs per boot,
// so they share the bytes instead of carrying ~500 B of BSS between them.
// Sized for the larger user, SuperOS's 0x26 poly-blob set:
// F0 + 5 header + 260 packed + F7 = 267. Both sides static_assert their need.
static constexpr unsigned FW_USB_SYSEX_SCRATCH = 267;
extern uint8_t g_usb_sysex_scratch[FW_USB_SYSEX_SCRATCH];
