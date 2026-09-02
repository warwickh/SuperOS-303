# Syncing src/d650/ with the emulator repo

Upstream: `../SuperOS-303D650cEmulator`, commit
`16361360d7b4da1ef580ac81b6a172dcd0f3822c`
("Update binary files and improve emulation performance"). Verified 2026-08-26:
upstream HEAD is still that commit, so every figure below is measured against
exactly this baseline.

`emu/tb303_rom.h` (the reconstructed mask ROM) is intentionally NOT tracked: it
must never be distributed. Copy it from the emulator repo for a local
ROM-embedded `combined` build; the public release build is `combined-norom`,
where users upload their own ROM dump over SysEx.

---

## The old procedure was UNSAFE. Do not follow it.

This file used to say:

> Local changes are guarded by SUPEROS_COMBINED; re-sync by copying
> emu_avr.cpp and emu/\*.{c,h} and re-applying the guarded blocks.

**Both halves of that sentence are false, and the second one is what causes the
damage.** Measured 2026-08-26, `diff -u` against the upstream commit named above:

    emu_avr.cpp        27 hunks:  18 guarded, 9 UNGUARDED
                                  (of the 9, five are CHANGE/DELETE, so a copy
                                   does not merely drop an addition, it
                                   reinstates upstream code)
    emu/d650_host.c     1 hunk    guarded
    emu/ucom4.c         1 hunk    guarded
    emu/ucom4.h         1 hunk    guarded (D650_ROM_IN_RAM)
    emu/emu_settings.h  1 hunk    guarded (SUPEROS_COMBINED)

Re-derive those numbers in one line rather than trusting this table:

    diff -u ../SuperOS-303D650cEmulator/emu_avr.cpp src/d650/emu_avr.cpp | grep -c '^@@'

**"Re-applying the guarded blocks" recovers 18 of 27 and silently loses 9.** An
earlier version of this document said "12 of 16"; that was a different counting
basis and is superseded, not merely stale.

## The actual rule

* **`emu/*.{c,h}` are SHARED.** All four local hunks are inside `#ifdef` guards,
  so an upstream copy conflicts visibly or the guard block is plainly absent.
  Copy these, then re-apply the four guarded blocks and diff to confirm.
* **`emu_avr.cpp` is FORKED.** Do not copy it. Merge upstream changes into it by
  hand, hunk by hand, and run the guard below before committing.

## The guard

    python3 tools/hosttest/sync_guard.py             check the tree
    python3 tools/hosttest/sync_guard.py --selftest  prove each entry can go RED

It is wired into `tools/hosttest/run.sh`. It names each unguarded fix, its
anchors, and why its loss is SILENT: **silence is the criterion for being in the
list.** A reverted fix that visibly breaks something does not need a guard,
because the breakage is the guard.

**The guard covers TWO fixes, not nine.** It is not a substitute for reading the
diff:

1. **claim-091 probe self-cost.** Reverting republishes the `0x40` probe's own
   reply as the `max_pass`/`max_gap` stall figure, and it reads as a healthy
   steady ~1.5 ms number.
2. **LED flicker scan phase-lock.** Upstream resets `TCNT3` and forces a
   counter-advancing `SendISR` in `refresh_inputs`, pinning the LED-refresh
   phase to the panel scan, so LED brightness modulates in time with the tempo.
   It builds, it runs, every SysEx probe answers, and the only instrument that
   can see it is an eye on the panel.

**The other seven unguarded hunks have no guard.** The largest is the deletion
of `enter_bootloader()` from `emu_avr.cpp` (the combined build owns that path);
a copy reinstates a second definition. Add an entry to `sync_guard.py` when you
find another whose loss would be silent.
