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
#define PAGE_LEN     4096

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

/* Is every slot either erased or a record we wrote? If not, something else
 * owns this page and we must not touch it. */
bool preset_store_is_safe(void)
{
	if (page_read() != 0) {
		return false;
	}
	for (uint32_t off = 0; off + PRESET_REC_SIZE <= PAGE_LEN;
	     off += PRESET_REC_SIZE) {
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
			return false;      /* foreign content: hands off */
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
		printk("preset: REFUSING to write, page 0xFF000 holds foreign "
		       "data. Dump it with PLAY before trusting this.\n");
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
	printk("preset: page 0xFF000, %u of %u bytes are not 0xFF, safe=%d\n",
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
