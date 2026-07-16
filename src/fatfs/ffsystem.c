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

/* NO-OP grants. Correct ONLY because the current block_on execution model has no
 * concurrent entry into FatFS on any BSP (Embassy parks the executor + SD_GATE;
 * legacy is cooperative single-thread + the app checkers; host is single-thread).
 * The async-SD follow-on MUST replace ff_req_grant/ff_rel_grant with a real
 * per-BSP grant (an interrupt-friendly Embassy mutex) BEFORE it makes FatFS calls
 * yield mid-transfer -- see the cooperative-glue-retirement design spec §Phase 2b.
 * The fatfs_stress harness (tests/fatfs_stress/) validates the grant discipline
 * itself. */

int ff_cre_syncobj (BYTE vol, FF_SYNC_t* sobj) { (void)vol; *sobj = 1; return 1; }
int ff_del_syncobj (FF_SYNC_t sobj)            { (void)sobj; return 1; }
int ff_req_grant   (FF_SYNC_t sobj)            { (void)sobj; return 1; }
void ff_rel_grant  (FF_SYNC_t sobj)            { (void)sobj; }

#endif

