#ifndef BB_RUNTIME_CHEATS_H
#define BB_RUNTIME_CHEATS_H
#include <stdint.h>

/* probe.c: write cheat bytes into the guest image at `offset` (eboot vaddr 0 = image offset 0,
 * the BBPATCH2 convention). Validates the range against the loaded segments; 1 on success.
 * Page-aware: executable pages are made writable for the copy (instruction cache flushed), and
 * NOACCESS pages are written through the fault so the GPU's VEH path tracks the change. */
int runtime_cheat_write(uint64_t offset, const void *data, uint64_t size);

/* Boot (probe.c, after the GPU is up and before the image is protected and the guest starts):
 * loads the cheat JSONs for this serial+version from the cheats directory and re-applies the
 * mods persisted as enabled. */
void runtime_cheats_boot(const char *serial, const char *version);

/* probe.c registers the guest VA of the cheat scratch page (the image tail) before boot.
 * Community patch families share data through absolute low patch-space addresses (an
 * "attacker" slot at 0x4000 in the CUSA03023 set); scripts/relocate_cheats.py rewrites
 * those references to scratch_base + T and records the base it assumed in each json's
 * bbport_scratch field, which the loader verifies against this value — a mismatch means
 * the boot image changed without rerunning the relocation and the file is skipped with
 * a loud message rather than crashing on a stale absolute address. */
void bbcheats_set_scratch(uintptr_t base);

/* Overlay bridge (the present thread is the only caller after boot). Index bounds are
 * bbcheats_file_count / bbcheats_mod_count; toggles return NULL on success or a static
 * error string (a literal, valid for the process lifetime). */
int bbcheats_file_count(void);
const char *bbcheats_file_name(int file);
const char *bbcheats_file_dir(void);
int bbcheats_mod_count(int file);
const char *bbcheats_mod_name(int file, int mod);
const char *bbcheats_mod_desc(int file, int mod);
int bbcheats_mod_enabled(int file, int mod);
int bbcheats_mod_one_way(int file, int mod);
const char *bbcheats_toggle(int file, int mod, int enable);
int bbcheats_master_available(int file);
const char *bbcheats_master_name(int file);
int bbcheats_master_enabled(int file);
const char *bbcheats_master_toggle(int file, int enable);
#endif
