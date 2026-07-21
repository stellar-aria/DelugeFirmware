/* Board memory layout for the Synthstrom Deluge (RZ/A1L) — RTT enabled.
 * INCLUDE'd by rza1l_rtt.x (from rza1l-hal) when built with --features rtt
 * (the default). Mirrors deluge-sdk's controller-firmware/memory_rtt.x. */

MEMORY {
    /* 2.875 MB on-chip SRAM, minus the RTT region carved out below. */
    RAM (rwx) : ORIGIN = 0x20020000, LENGTH = 0x002E0000

    /* 64 MB external SDRAM (CS3) */
    SDRAM (rwx) : ORIGIN = 0x0C000000, LENGTH = 0x04000000

    /* 64 KB reserved at the top of SRAM for the RTT ring buffer, immediately
       below the exception/program stacks (which start at INTERNAL_RAM_END -
       0x10000 = 0x202F0000). Placing it here — rather than mid-SRAM — lets the
       image + `__sram_heap` span grow all the way up to the RTT region instead
       of being capped ~192 KB lower, which is what a large live feature (e.g.
       the `efatfs_streaming` embedded-fatfs mount) needs to fit at debug
       opt-levels. RTT addresses are auto-discovered from the ELF's `_SEGGER_RTT`
       symbol (deluge_run.py / cortex-debug `address: auto`), so no tooling
       address is hard-coded to the value below.
       Cached alias: 0x202E0000–0x202EFFFF; uncached mirror is +0x40000000. */
    RTT_RAM   (rw) : ORIGIN = 0x202E0000, LENGTH = 0x00010000  /* 64 KB cached   */
    NCACHE_RTT_RAM (rw): ORIGIN = 0x602E0000, LENGTH = 0x00010000  /* 64 KB uncached */
}

/* Stack sizes */
PROGRAM_STACK_SIZE = 0x8000;   /* 32 KB - application / SYS mode */
IRQ_STACK_SIZE     = 0x2000;   /*  8 KB */
FIQ_STACK_SIZE     = 0x2000;   /*  8 KB */
SVC_STACK_SIZE     = 0x2000;   /*  8 KB */
ABT_STACK_SIZE     = 0x2000;   /*  8 KB */

/* Top byte address of on-chip SRAM (anchors stack sections) */
INTERNAL_RAM_END = 0x20300000;
