/* Flash storage for presets: the Zephyr-only half of the preset module.
 *
 * The page at 0xFF000 is the ONLY 4 KB of storage available, and the
 * firmware flasher stops below it (solderless utility/js/protocol.js:10-11),
 * so preset data survives a reflash. One page is not enough for Zephyr NVS,
 * which needs two sectors, hence the append-log in presets.c.
 *
 * SAFETY: the community board files LABEL this page "storage", but a label
 * is not evidence that the TE bootloader ignores it. If the bootloader keeps
 * settings or recovery metadata there, erasing it is a plausible soft-brick
 * on scarce hardware. So this module refuses to write until it can show the
 * page is ours: every slot must be either fully erased or a valid preset
 * record. Anything else and we leave it alone and say so.
 */

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include "presets.h"

#define STORAGE_ID   FIXED_PARTITION_ID(storage_partition)

/* The storage partition is 32 KB at 0xF7000. Presets use its FIRST page;
 * the remaining 28 KB is deliberately left for the runtime-editable
 * profile (see docs/storage-layout.md), which no longer has to share a
 * page with anything.
 *
 * This is NOT the page at 0xFF000. That one belongs to the bootloader:
 * it already held 0x5A82B619 on pop before we ever wrote flash, and
 * feldd's firmware documents it as the bootloader-settings/MBR page,
 * warning that writing it could brick the bootloader, which is the only
 * recovery path on hardware with no reset pin. The board devicetree
 * declares it read-only so nothing can claim it by accident. */
#define PAGE_LEN     4096

/* The 32 KB region is divided by page so the two stores never share an
 * erase unit: presets in page 0, the editable profile in page 1. The
 * remaining 24 KB is spare. */
#define PROFILE_OFF  4096

static uint8_t page_buf[PAGE_LEN];

static int page_read(void)
{
	const struct flash_area *fa;
	int rc;

	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return -1;
	}
	rc = flash_area_read(fa, 0, page_buf, PAGE_LEN);
	flash_area_close(fa);
	return rc;
}

/* Is this page ours to write?
 *
 * Every byte of the page must be accounted for: either part of a record we
 * can decode, or erased. Note the tail: 102 records of 40 bytes cover 4080
 * bytes, and the remaining 16 are checked separately. An earlier version
 * scanned only the records while the erase covered all 4096, so foreign
 * data living solely in that tail would have been declared safe and then
 * destroyed.
 *
 * HONEST LIMIT, worth stating because the name overpromises: this shows the
 * page holds nothing foreign RIGHT NOW. It cannot show the bootloader will
 * not use the page later, or that it expects the page to stay erased. No
 * software check can. What makes the risk acceptable is that the flasher
 * deliberately preserves this page, that a human inspects the dump before
 * the first save, and that the only thing at stake if we are wrong is
 * bootloader state we can reflash around. */
bool preset_store_is_safe(void)
{
	if (page_read() != 0) {
		return false;
	}

	uint32_t off = 0;

	for (; off + PRESET_REC_SIZE <= PAGE_LEN; off += PRESET_REC_SIZE) {
		bool erased = true;

		for (uint32_t i = 0; i < PRESET_REC_SIZE; i++) {
			if (page_buf[off + i] != 0xFF) {
				erased = false;
				break;
			}
		}
		if (erased) {
			continue;
		}

		preset_bank_t tmp;

		if (!preset_record_decode(page_buf + off, &tmp)) {
			return false;      /* foreign or torn: hands off */
		}
	}

	/* The tail the record loop cannot reach. */
	for (; off < PAGE_LEN; off++) {
		if (page_buf[off] != 0xFF) {
			return false;
		}
	}
	return true;
}

bool preset_store_load(preset_bank_t *out)
{
	if (page_read() != 0) {
		return false;
	}
	return preset_page_find_latest(page_buf, PAGE_LEN, out) >= 0;
}

bool preset_store_save(const preset_bank_t *bank)
{
	const struct flash_area *fa;
	bool ok = false;

	if (!preset_store_is_safe()) {
		printk("preset: REFUSING to write. The storage page holds data this "
		       "firmware did not write, or a record torn by a reset "
		       "mid-write. Dump it with PLAY and inspect before "
		       "trusting it. If it is our own torn record, "
		       "preset_store_repair() erases the page.\n");
		return false;
	}
	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return false;
	}

	/* page_buf holds the current contents from preset_store_is_safe. */
	int off = preset_page_next_offset(page_buf, PAGE_LEN);

	if (off < 0) {
		/* Full: erase once and start again. At 102 records per cycle
		 * this is rare. The erase is safe because the check above just
		 * proved every record in the page is one of ours. */
		if (flash_area_erase(fa, 0, PAGE_LEN) == 0) {
			off = 0;
		}
	}
	if (off >= 0) {
		uint8_t rec[PRESET_REC_SIZE];

		preset_record_encode(bank, rec);
		ok = (flash_area_write(fa, (off_t)off, rec, sizeof(rec)) == 0);
	}

	flash_area_close(fa);
	return ok;
}

/* Task 7.0: dump the page for inspection. Read only, and deliberately NOT
 * at boot: a CDC console discards anything printed before the host opens
 * the port, so a boot-time dump is usually invisible. Triggered by a button
 * instead. Prints only non-erased lines, since a virgin page is 4 KB of
 * 0xFF and that is the answer we hope for. */
void preset_store_dump(void)
{
	if (page_read() != 0) {
		printk("preset: page read FAILED\n");
		return;
	}

	uint32_t nonff = 0;

	for (uint32_t i = 0; i < PAGE_LEN; i++) {
		if (page_buf[i] != 0xFF) {
			nonff++;
		}
	}
	printk("preset: storage page (0xF7000), %u of %u bytes not 0xFF, safe=%d\n",
	       nonff, PAGE_LEN, preset_store_is_safe() ? 1 : 0);

	if (nonff == 0) {
		printk("preset: page is fully erased, nothing owns it\n");
		return;
	}
	for (uint32_t i = 0; i < PAGE_LEN; i += 16) {
		bool all_ff = true;

		for (uint32_t j = 0; j < 16; j++) {
			if (page_buf[i + j] != 0xFF) {
				all_ff = false;
				break;
			}
		}
		if (all_ff) {
			continue;
		}
		printk("%04x:", i);
		for (uint32_t j = 0; j < 16; j++) {
			printk(" %02x", page_buf[i + j]);
		}
		printk("\n");
	}
}

/* Repair path for the one self-inflicted way saving can lock itself out: a
 * reset during a 40-byte write leaves a non-erased, undecodable record, and
 * from then on preset_store_is_safe() refuses everything. Without this the
 * only fix would be a tool that erases a page the flasher deliberately
 * preserves.
 *
 * Deliberately NOT automatic, and deliberately not reachable by accident:
 * it erases the page, which is exactly what we refuse to do when ownership
 * is in doubt. A human should read the dump first and decide. */
bool preset_store_repair(void)
{
	const struct flash_area *fa;
	bool ok = false;

	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return false;
	}
	ok = (flash_area_erase(fa, 0, PAGE_LEN) == 0);
	flash_area_close(fa);
	printk("preset: page erase %s\n", ok ? "OK" : "FAILED");
	return ok;
}

/* ---- profile store, page 1 of the region ---- */

static uint8_t prof_buf[PAGE_LEN];

static int prof_page_read(void)
{
	const struct flash_area *fa;
	int rc;

	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return -1;
	}
	rc = flash_area_read(fa, PROFILE_OFF, prof_buf, PAGE_LEN);
	flash_area_close(fa);
	return rc;
}

/* Seed *out with the compiled default before calling: a stored record only
 * overrides the mapping, never the build's capture list. */
bool profile_store_load(profile_t *out)
{
	if (prof_page_read() != 0) {
		return false;
	}
	return profile_page_find_latest(prof_buf, PAGE_LEN, out) >= 0;
}

bool profile_store_save(const profile_t *prof)
{
	const struct flash_area *fa;
	bool ok = false;

	if (prof_page_read() != 0) {
		return false;
	}

	int off = profile_page_next_offset(prof_buf, PAGE_LEN);

	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return false;
	}
	if (off < 0) {
		/* 28 records per page. Erasing only this page cannot touch the
		 * presets in page 0, and the compiled default is always the
		 * fallback, so losing the stored mapping is never fatal. */
		if (flash_area_erase(fa, PROFILE_OFF, PAGE_LEN) == 0) {
			off = 0;
		}
	}
	if (off >= 0) {
		uint8_t rec[PROFILE_REC_SIZE];

		profile_record_encode(prof, rec);
		ok = (flash_area_write(fa, (off_t)(PROFILE_OFF + off), rec,
				       sizeof(rec)) == 0);
	}
	flash_area_close(fa);
	return ok;
}

bool profile_store_erase(void)
{
	const struct flash_area *fa;
	bool ok;

	if (flash_area_open(STORAGE_ID, &fa) != 0) {
		return false;
	}
	ok = (flash_area_erase(fa, PROFILE_OFF, PAGE_LEN) == 0);
	flash_area_close(fa);
	return ok;
}
