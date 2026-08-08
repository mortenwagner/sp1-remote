# Storage layout

Design for sharing the single flash page between presets and a
runtime-editable profile. **Not implemented**: it depends on the page being
ours, which `preset_store_is_safe()` and the PLAY dump exist to establish.
Written up now so the work is ready rather than improvised later.

## The constraint

There is exactly one 4 KB page available, at `0xFF000`. Everything below it
belongs to the bootloader (`0x0`) or the application slot (`0x20000`), and
the firmware flasher rewrites `0x20000` to `0xFF000` on every update
(`solderless utility/js/protocol.js:10-11`), so that page is the only
storage that survives a reflash.

Two consequences shape everything below:

- **Zephyr NVS is unusable.** It needs at least two sectors.
- **Erase granularity is the whole page.** You cannot erase presets without
  erasing the profile. Any compaction has to rewrite both.

## Current layout (implemented, presets only)

An append-log of fixed 40-byte records, each `'S' 'P' version ... crc8`.
Write the next erased slot, read the last valid one, erase only when full.
102 records per erase cycle.

## Proposed layout (presets + profile)

Split the page by region, keeping the existing preset format untouched so
the tested code and its 48 host tests stay valid.

```
0xFF000 ┌─────────────────────────────────────┐
        │ preset log                          │  3072 bytes
        │ 76 records of 40 bytes              │  = 76 saves per erase
0xFFC00 ├─────────────────────────────────────┤
        │ profile log                         │  1024 bytes
        │ 7 records of 136 bytes              │  = 7 edits per erase
0x100000└─────────────────────────────────────┘
```

Why regions rather than one interleaved log: the two record types are
different sizes, and a single log would need a length field in the header,
which changes the preset format that is already written and tested. Regions
cost nothing but arithmetic.

Why the profile gets the smaller share: presets are a performance gesture
and get saved often; the mapping is edited rarely, and 7 edits between
erases is generous for something you change a handful of times a year.

### Profile record, 136 bytes

```
[0]      magic 'S'
[1]      magic 'C'          (config, distinct from 'SP' presets)
[2]      version = 1
[3]      reserved (0)
[4..11]  4 faders x (cc, channel)
[12..123] 4 buttons x 28 bytes:
           mode, channel, cc, on_value, off_value,
           steps[4], n_steps, init_step, preset_slot,
           list[5] x (channel, cc, value), n_list
[124..129] preset_capture[5]
[130]    preset_capture_len
[131..134] reserved (0)
[135]    CRC-8 over bytes 0..134
```

Same CRC-8 (polynomial 0x07) as preset records, so `crc8()` is shared.

### Compaction

When either region fills, the whole page must be rewritten:

1. Read the newest valid preset record and the newest valid profile record.
2. Erase the page.
3. Write both back at offset 0 of their regions.

A reset between steps 2 and 3 loses stored presets and the stored profile.
That is acceptable and non-fatal by construction: **the compiled-in
`profile_popgoblin_default` is always the fallback**, so a lost profile
degrades to the shipped mapping rather than to an unusable puck. Say so in
the code, because it is the reason this design is allowed to be simple.

## Editor protocol

Line-based ASCII on the existing CDC console, so it is debuggable by hand in
`screen` before any browser is involved. The console is also printing
diagnostics continuously, so the first thing the editor sends is a command
to stop that.

| Command | Effect |
|---|---|
| `!quiet` | stop the diagnostic stream (the editor sends this on connect) |
| `!noisy` | resume it |
| `?prof` | print the current profile, one control per line |
| `!f<i> cc <n> ch <m>` | set fader `i` |
| `!b<i> mode <toggle\|cycle\|preset\|list> cc <n> ch <m>` | set button `i` |
| `!b<i> steps <a,b,c,d> init <k>` | set a cycle button's values |
| `!save` | persist the profile to flash |
| `!default` | restore the compiled-in profile (RAM only until `!save`) |
| `!dump` | hex-dump the storage page |

Every command replies with a single line beginning `ok` or `err`, so the
browser never has to guess whether something applied.

Note the CDC console can only be held by one program at a time: the browser
page cannot connect while `screen` has the port.

## Order of work

1. Confirm the page is unowned (PLAY dump). **Blocks everything below.**
2. Extend `presets.c` with profile record encode/decode plus region-aware
   scanning, all pure, with host tests alongside the existing ones.
3. Add the region logic and compaction to `presets_flash.c`.
4. Load the stored profile at boot, falling back to the compiled-in default.
5. Add the command protocol to the console.
6. Add the editor as a tab in `tools/monitor.html`, which already has the
   WebSerial plumbing.

Steps 2 to 4 are worth doing even without the editor: they make the mapping
survive a reflash, which is most of the value.
