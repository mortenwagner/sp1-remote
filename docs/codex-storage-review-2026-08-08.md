Do not escalate that caution into eMMC work, though. For v1, use an app-owned, reflash-ephemeral page; if that cannot receive one bounded hardware test, ship RAM-only. Leave `0xFF000` completely read-only.

No files were changed.
[2mtokens used[0m
153.048
The last page should now be treated as bootloader-owned. Do not ship B, and do not erase `0xFF000`.

### What is `0x5A82B619`?

Most likely: a bootloader settings/validation CRC or token—not necessarily the application CRC itself.

The strongest evidence is architectural: Nordic’s standard nRF52840 bootloader layout reserves exactly `0xFF000–0x100000` as the 4 KB bootloader settings page. Its first field is conventionally a CRC over bootloader settings. Nordic explicitly describes that address and warns that the settings page contains sensitive validation information. [Nordic memory layout](https://docs.nordicsemi.com/r/bundle/nrf5_sdk_v13.0.0/page/lib_bootloader.html), [settings CRC implementation](https://devzone.nordicsemi.com/f/nordic-q-a/65790/bootloader-and-softdevice-update-using-thread-secure-dfu/282817), [secure-boot guidance](https://docs.nordicsemi.com/r/bundle/sdk_nrf5_v16.0.0/page/lib_secure_boot.html).

TE clearly has a custom layout and protocol, so this does not prove it uses Nordic’s structure verbatim. But the address, page size, first-word placement and bootloader-controlled finalization together are too exact to dismiss.

My likelihood ranking:

1. Bootloader settings/validation CRC or token — high.
2. Application-valid marker or proprietary image checksum — plausible subtype of the above.
3. Fixed bootloader magic/default-state fingerprint — possible.
4. Boot counter — unlikely; repeatedly updating an arbitrary integer in one flash word would require page erases or a bit-clearing scheme.
5. Stock firmware setting — possible, but the address strongly favours bootloader metadata.
6. SHA-1 constant — coincidence. SHA-1 specifies `0x5A827999`; your value differs by `0x3C80`, and I found no authoritative occurrence of `0x5A82B619` as a known algorithmic constant. [RFC 3174](https://datatracker.ietf.org/doc/html/rfc3174)

CRC results for the supplied build:

- Size: 58,708 bytes; loaded at `0x20000`, ending at `0x2E554`.
- Standard CRC-32 of exact `.bin`: `0xD672D284`.
- Padded with `0xFF` to its final 4 KB page: `0xB63396A3`.
- Padded with `0xFF` across the complete app slot: `0xEEAF1811`.
- Earlier looper binary, exact CRC-32: `0x264CCE4E`.
- No match among CRC-32/ISO-HDLC, JAMCRC, CRC-32C, Koopman, MPEG-2 or BZIP2 across those scopes. No prefix of either binary produced the target standard CRC either.

So it is not a straightforward CRC32 of either known application image. A proprietary scope, polynomial or bootloader-settings CRC remains possible.

The updater provides another clue: it sends the last application page to an opaque two-step `H` finalization and passes a returned 32-bit value back to the bootloader ([firmware.js](</Users/morten/Documents/Other Creations/dev/solderless/solderless-2026-05-18/utility/js/firmware.js:158>)). The JavaScript calls that value a “counter,” but that name is reverse-engineering inference, not a protocol guarantee. It could be a validation token.

Also, `F` is opaque: the browser proves only that host writes stop before `0xFF000` ([protocol.js](</Users/morten/Documents/Other Creations/dev/solderless/solderless-2026-05-18/utility/js/protocol.js:8>)). It cannot prove whether the bootloader leaves, erases or rewrites its last page during format/finalization.

I independently confirmed that `refs/sp1-tape-looper` contains zero `flash_area` calls and no other recognizable nRF internal-flash write/erase API calls across its firmware and board sources.

### Ranked recommendation

1. **D — reserve application-owned flash inside the reflashed app slot.**

   For v1, reserve one page around `0xF0000–0xF0FFF`, comfortably away from `0xFF000`, and cap the linker’s application region at `0xF0000`. That still leaves 832 KiB for code versus the current 57 KiB image. Reuse the existing append log.

   Presets persist across power cycles but are explicitly allowed to disappear on firmware update. That is a much better failure mode than risking bootloader metadata. A later browser export/import can preserve them across updates.

   Hardware gate before shipping: save a record, cold-boot repeatedly, enter and exit the two-button bootloader, then reflash and confirm normal recovery. The cost if this assumption is wrong is likely “bootloader rejects the modified app until reflash,” not destruction of bootloader settings.

2. **A — RAM only.**

   Ship this if there is no time for the app-slot relocation and hardware gate. Honest limitation, essentially no new flash risk.

3. **C — eMMC.**

   Correct long-term option if presets must survive firmware updates, but disproportionate for v1. It imports initialization, power sequencing, media-layout and corruption risks merely to store tens of bytes.

4. **B — do not ship.**

   Starting at offset 64 does not make the page safe. If word zero is a CRC over subsequent settings, the first preset write invalidates it without touching the first 64 bytes. “Preserve 0–63 across erase” also creates a power-loss window and may restore a checksum that no longer matches the page.

### Judgement

You were not being too cautious. The refusal gate did exactly its job and uncovered evidence that the inherited `storage` label is wrong or dangerously misleading.

Do not escalate that caution into eMMC work, though. For v1, use an app-owned, reflash-ephemeral page; if that cannot receive one bounded hardware test, ship RAM-only. Leave `0xFF000` completely read-only.

No files were changed.
