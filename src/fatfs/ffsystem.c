/*------------------------------------------------------------------------*/
/* Sample Code of OS Dependent Functions for FatFs                        */
/* (C)ChaN, 2018                                                          */
/*------------------------------------------------------------------------*/


#include "ff.h"


#if FF_USE_LFN == 3	/* Dynamic memory allocation */

/*------------------------------------------------------------------------*/
/* Allocate a memory block                                                */
/*------------------------------------------------------------------------*/

void* ff_memalloc (	/* Returns pointer to the allocated memory block (null if not enough core) */
	UINT msize		/* Number of bytes to allocate */
)
{
	return malloc(msize);	/* Allocate a new memory block with POSIX API */
}


/*------------------------------------------------------------------------*/
/* Free a memory block                                                    */
/*------------------------------------------------------------------------*/

void ff_memfree (
	void* mblock	/* Pointer to the memory block to free (nothing to do if null) */
)
{
	free(mblock);	/* Free the memory block with POSIX API */
}

#endif



#if FF_FS_REENTRANT	/* Mutal exclusion */

/* NO-OP grants -- by design, not by an interim shortcut. The async-SD design
 * (docs/superpowers/specs/2026-07-16-async-sd-storage-owner-design.md §4-5)
 * considered upgrading these to a real per-BSP mutex (a fiber-aware grant, or
 * an off-fiber defer paired with one) but Kate chose the single-storage-owner
 * architecture instead: on Embassy, EVERY FatFS-touching operation is routed
 * through `deluge::storage::Owner` (src/deluge/storage/owner.h) onto the one
 * worker fiber (see include/libdeluge/storage_owner.h), which can only ever
 * have one op in flight at a time -- so FatFS is entered from exactly one
 * serialized context by construction, whether that op parks (`block_on`) or
 * yields mid-transfer (`block_on_fiber`, live since the rung-5 flip in
 * src/bsp/rust/src/sd.rs). A real grant would duplicate that guarantee; the
 * `storage-owner-audit` feature's debug_assert (sd.rs's `deluge_block_read`/
 * `deluge_block_write`) is the enforcement point instead, catching a stray
 * off-fiber caller directly rather than relying on a grant it would have to
 * hold correctly. Legacy/host remain single-thread (cooperative / host
 * single-thread respectively), so no grant is needed there either. `SD_GATE`
 * (the scheduler.rs resource mutex this design also made redundant) was
 * retired in the same change.
 * The fatfs_stress harness (tests/fatfs_stress/) validates FatFS's own
 * `lock_fs`/`unlock_fs` macro coverage *given* a correct grant (see
 * tests/fatfs_stress/RESULTS.md); its scope notes are explicit that this does
 * NOT cover the yield-mid-transfer/single-owner case -- that property is
 * proved instead by the host-TSan owner/fiber exercises in
 * src/bsp/rust/tests/support/owner_host_exercise.rs. */

int ff_cre_syncobj (BYTE vol, FF_SYNC_t* sobj) { (void)vol; *sobj = 1; return 1; }
int ff_del_syncobj (FF_SYNC_t sobj)            { (void)sobj; return 1; }
int ff_req_grant   (FF_SYNC_t sobj)            { (void)sobj; return 1; }
void ff_rel_grant  (FF_SYNC_t sobj)            { (void)sobj; }

#endif

