//! Rust/Embassy firmware image for the Synthstrom Deluge (RZ/A1L).
//!
//! The third BSP for the Deluge, alongside `src/bsp/rza1` (on-device C) and
//! `src/bsp/host` (simulator). Unlike those, this one OWNS reset, `main`/`_start`
//! and the Embassy executor: the HAL startup (`rza1l_hal` `_start` →
//! `_reset_handler` → `bl main`) lands in [`main`] here, which brings up the
//! platform on the deluge-sdk HAL+BSP and runs the portable C++ application's
//! `deluge_main()` superloop inside a single Embassy task (Stage 1).
//!
//! The C++ application is compiled by CMake into `libdeluge_app.a` and linked in
//! by `build.rs`; this crate implements the `<libdeluge/...>` C-ABI services it
//! calls. See docs/dev/libdeluge_bsp_design.md.
#![cfg_attr(target_os = "none", no_std)]
#![cfg_attr(target_os = "none", no_main)]
#![feature(impl_trait_in_assoc_type)]

#[cfg(target_os = "none")]
use core::mem::MaybeUninit;
#[cfg(target_os = "none")]
use core::panic::PanicInfo;

#[cfg(target_os = "none")]
use embassy_executor::{Executor, InterruptExecutor, Spawner};
// General-purpose Rust heaps (for BSP/Embassy/our boot). The C++ app keeps its
// own GeneralMemoryAllocator over the region we hand it; the Rust app allocator
// (TLSF/slab, docs/dev/allocator_redesign.md) is a later, separately-gated step.
// Device-only: `deluge-alloc` is a `target_os = "none"`-only dependency (see
// Cargo.toml) — no Rust-side heap on host in M1 (Task 4 grows a Vec-backed one).
#[cfg(target_os = "none")]
use deluge_alloc as allocator;

// Link-only: the C++ app (archived into this image by build.rs) calls the
// deluge_resource_* residency C ABI and the deluge_{alloc,slab_*} allocator C ABI.
// `extern crate` forces both rlibs onto the link line so those #[no_mangle]
// symbols resolve; nothing here references them from Rust. deluge_alloc arrives
// transitively via deluge_resource (a distinct crate from the `deluge_alloc`
// aliased above — that one is the sibling deluge-sdk allocator).
#[cfg(target_os = "none")]
extern crate deluge_resource;

/// libdeluge POD types generated from include/libdeluge/*.h (types only; the
/// service functions are defined in [`ffi`]). No C++ app is linked on host in
/// M1 (see build.rs), so there is no bindgen output to include.
#[cfg(target_os = "none")]
#[allow(non_camel_case_types, non_upper_case_globals, dead_code)]
mod sys {
    include!(concat!(env!("OUT_DIR"), "/libdeluge_sys.rs"));
}
/// Host stand-in for the above (no C++ app / bindgen output off-target). Used
/// unless `host_app` is enabled, in which case the real bindgen output below
/// takes over.
#[cfg(all(not(target_os = "none"), not(feature = "host_app")))]
#[path = "sys_host.rs"]
mod sys;
/// `host_app` feature: the real host-ABI bindgen output (see build.rs), for
/// when the host-built C++ `deluge_app` object closure is linked in too.
/// Replaces the `sys_host.rs` stand-ins above with the genuine generated
/// types (M4b).
#[cfg(all(not(target_os = "none"), feature = "host_app"))]
mod sys {
    #![allow(
        non_upper_case_globals,
        non_camel_case_types,
        non_snake_case,
        dead_code
    )]
    include!(concat!(env!("OUT_DIR"), "/libdeluge_sys.rs"));
}

/// audio_io.h — duplex block audio over the SSI0 DMA rings.
#[cfg(target_os = "none")]
mod audio;
/// board.h — capability descriptor + GPIO/audio/CV bring-up.
#[cfg(target_os = "none")]
mod board;
/// C++ memory-model bring-up (SDRAM bss/data, global ctors).
#[cfg(target_os = "none")]
mod boot_mem;
/// control_surface.h — pads/buttons/encoders + LEDs (M2b WIP).
mod control;
/// cv_gate.h — CV/gate outputs + external trigger clock.
#[cfg(target_os = "none")]
mod cv_gate;
/// display.h — main OLED output over deluge_bsp::oled.
mod display;
/// The libdeluge C-ABI service implementations the C++ app calls (stubs).
#[cfg(target_os = "none")]
mod ffi;
/// Non-header app/BSP symbols (USB-host globals, FatFS glue, NE10, runtime shims).
#[cfg(target_os = "none")]
mod ffi_extra;
/// The worker fiber: a stackful coroutine for the long synchronous C++ operations
/// that pause via `yield()`. This module is the context-switch primitive.
mod fiber;
/// flash.h — persistent settings flash over deluge_bsp::flash / spibsc.
mod flash;
/// midi_io.h — DIN MIDI over deluge_bsp::uart (+ USB-MIDI peripheral, see usb).
#[cfg(target_os = "none")]
mod midi;
/// scheduler.h / OSLikeStuff scheduler_api.h — the cooperative task scheduler,
/// implemented on the Embassy executor (one task per registered Deluge task).
mod scheduler;
/// block_device.h + FatFS diskio — SD card over deluge_bsp::sd.
mod sd;
/// Real impls of the simplest services (system.h, clock.h, memory.h).
mod services;
/// signals.h — board GPIO signals, battery, MIDI/gate timer.
#[cfg(target_os = "none")]
mod signals;
/// USB device bring-up — USB-MIDI 1.0 peripheral (Deluge → computer).
#[cfg(target_os = "none")]
mod usb;

#[cfg(target_os = "none")]
unsafe extern "C" {
    /// One-time C++ application bring-up (deluge.cpp / app.h). On this BSP it also
    /// runs `registerTasks()`, which spawns one Embassy task per registered Deluge
    /// task through the `scheduler_api.h` C ABI in [`scheduler`]. After this returns
    /// the spawned runners drive the app — there is no `deluge_app_tick` loop.
    fn deluge_app_init(board: *const sys::DelugeBoard);
}

/// Rust-side SRAM heap pool. The C++ app's GeneralMemoryAllocator owns the rest
/// of SRAM (`[__heap_start, program_stack_start)`), so the Rust heap is a small,
/// dedicated static pool that can't overlap it. Nothing on the Rust side
/// allocates from SRAM yet; sized small.
#[cfg(target_os = "none")]
static mut RUST_SRAM_POOL: [u8; 64 * 1024] = [0; 64 * 1024];

#[cfg(target_os = "none")]
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // `extern "C"` boundary: never unwind (panic = "abort" is also set).
    #[cfg(all(feature = "rtt", target_os = "none"))]
    log::error!("PANIC: {}", _info);
    loop {
        core::hint::spin_loop();
    }
}

#[cfg(target_os = "none")]
static mut EXECUTOR: MaybeUninit<Executor> = MaybeUninit::uninit();

/// Preemptive Embassy executor for audio. Runs the audio
/// render task in a GIC SGI handler at a priority above thread mode but below the
/// µs hard-RT ISRs, so it preempts the cooperative thread executor (a storage
/// `yield()` spin can no longer starve audio) yet is itself preempted by
/// OSTM/MTU2-gate/MIDI. The scheduler routes the priority-0 task here.
#[cfg(target_os = "none")]
static AUDIO_EXEC: InterruptExecutor = InterruptExecutor::new();

/// GIC Software-Generated Interrupt id driving [`AUDIO_EXEC`] (0..=15; SMP-free
/// board, so SGIs are otherwise unused).
#[cfg(target_os = "none")]
const AUDIO_SGI: u8 = 8;
/// Audio SGI GIC priority. Numerically ABOVE the hard-RT IRQs (OSTM=14, UART/MIDI
/// =10, DMAC=13) so they preempt the render, and below PMR (31) so it is
/// forwarded. (Lower number = more urgent.)
#[cfg(target_os = "none")]
const AUDIO_SGI_PRIORITY: u8 = 20;

/// GIC handler for [`AUDIO_SGI`]: drive the audio interrupt executor. Registered
/// in the HAL dispatch (`gic::register`), which already acks (GICC_IAR) before and
/// EOIs (GICC_EOIR) after, with IRQs re-enabled for nesting.
#[cfg(target_os = "none")]
fn audio_sgi_handler() {
    // SAFETY: only called from the SGI handler, after AUDIO_EXEC.start().
    unsafe { AUDIO_EXEC.on_interrupt() };
}

/// Firmware entry. The HAL reset handler (`_reset_handler` in rza1l-hal) ends in
/// `bl main`; control lands here with caches/MMU off and stacks set up.
#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn main() -> ! {
    // RTT logger first, so every boot step is visible over the probe. The ring
    // buffer + control block live in uncached SRAM (.rtt_buffer / rza1l_rtt.x).
    #[cfg(all(feature = "rtt", target_os = "none"))]
    {
        let channels = rtt_target::rtt_init! {
            up: { 0: { size: 16384, name: "Terminal", section: ".rtt_buffer" } }
            section_cb: ".rtt_buffer"
        };
        rtt_target::set_print_channel(channels.up.0);
        rtt_target::init_logger_with_level(log::LevelFilter::Debug);
    }
    log::info!("deluge-rust: boot — Rust/Embassy BSP for RZ/A1L");

    // Initialise the Rust-side SRAM heap over its dedicated static pool.
    unsafe {
        let p = core::ptr::addr_of_mut!(RUST_SRAM_POOL);
        allocator::SRAM.init(p as *mut u8, core::mem::size_of_val(&*p));
    }

    // CPG/PLL, MMU, L1+L2 caches, SDRAM controller, GIC, and the embassy-time
    // (OSTM) driver — the whole platform bring-up, owned by the BSP.
    unsafe { deluge_bsp::system::init_clocks() };
    log::info!("deluge-rust: clocks/MMU/cache/SDRAM/GIC up");

    // SDRAM is up: zero the app's SDRAM .bss and copy .sdram_init from its SRAM
    // load address (the debugger can't write SDRAM at download).
    unsafe { boot_mem::init_sdram_memory() };

    // Rust SDRAM allocator gets the reserved top slice; the app owns the rest
    // (its heap runs __heap_start .. deluge_memory_external_end()).
    unsafe {
        allocator::SDRAM.init(
            boot_mem::RUST_SDRAM_BASE as *mut u8,
            boot_mem::RUST_SDRAM_RESERVE,
        )
    };
    log::info!("deluge-rust: SDRAM memory image ready");

    // Run C++ global constructors before any application code touches globals.
    unsafe { boot_mem::run_init_array() };
    log::info!("deluge-rust: C++ global constructors done");

    // Setup window — GIC source registration, with IRQs still masked:
    //  - PIC co-processor UART (SCIF1) with DMA RX/TX, started at 31250 bps; the
    //    pump task (control::pic_pump) runs the handshake up to 200000 bps.
    //  - The six front-panel quadrature encoders (GPIO + GIC edge IRQs).
    // Both must be configured before the global interrupt enable below.
    unsafe { deluge_bsp::uart::init_pic(31_250) };
    unsafe { deluge_bsp::uart::init_midi(31_250) };
    unsafe { deluge_bsp::encoder::irq_init() };
    // Sample-accurate MIDI/gate one-shot timer (MTU2 ch2). Unlock MTU2 register
    // writes once, then register the compare-match ISR; armed at render time by
    // the app via deluge_midi_gate_timer_arm(). MTU2's module clock is already
    // ungated by init_clocks (STBCR3).
    unsafe { rza1l_hal::mtu2::enable_write() };
    unsafe { signals::gate_timer_setup() };
    // Trigger-clock (analog clock-in): route P1_14 → IRQ6 (falling edge — an
    // on-board transistor inverts the external clock, so an external rising edge
    // is a falling edge here). The app's handler runs from the cooperative
    // trigger_clock_task between app ticks; with no preemption it needs none of
    // the critical-section protection the original ISR path used. See cv_gate.rs.
    unsafe { deluge_bsp::trigger_clock::irq_init() };
    // USB-MIDI 1.0 peripheral (Deluge → computer): bring up USB0 in device mode
    // and build the device now (registers the USB0 GIC handler); its tasks are
    // spawned on the executor below. The GIC line is enabled by the driver when
    // device.run() starts.
    let usb_midi = unsafe { usb::build() };

    // Audio interrupt-executor: register the SGI handler and set its priority
    // (below the hard-RT IRQs, above thread mode). The GIC line is *enabled* only
    // after AUDIO_EXEC.start() below, so audio_sgi_handler can never run before the
    // executor it drives is initialised.
    unsafe {
        rza1l_hal::gic::register(AUDIO_SGI as u16, audio_sgi_handler);
        rza1l_hal::gic::set_priority(AUDIO_SGI as u16, AUDIO_SGI_PRIORITY);
    }

    // Verify the worker-fiber context switch in isolation before it drives real
    // operations. Pure + synchronous; logs PASS/FAIL over RTT.
    fiber::selftest();

    // Unmask IRQs so the time driver and peripheral ISRs fire.
    unsafe { cortex_ar::interrupt::enable() };

    // Tell the interrupt-executor SGI pender where the GIC Distributor actually
    // is. The pender otherwise derives it from CBAR/PERIPHBASE, which is only
    // valid on Cortex-A MPCore parts; on this single-core RZ/A1L CBAR reads
    // 0xF000_0000 while the Renesas-integrated GIC Distributor is fixed at
    // 0xE820_1000. Without this the pender writes GICD_SGIR to a bogus address
    // and the audio SGI never fires (silent audio). Must precede AUDIO_EXEC use.
    embassy_executor::set_gicd_base(0xE820_1000);

    // Start the audio interrupt-executor and stash its SendSpawner so the scheduler
    // can spawn the priority-0 (audio) task onto it. Must precede deluge_app_init
    // (in app_task), which calls registerTasks() → addRepeatingTask(priority 0).
    // Enable the SGI in the GIC only now that the executor is initialised.
    scheduler::set_audio_spawner(AUDIO_EXEC.start(AUDIO_SGI));
    unsafe { rza1l_hal::gic::enable(AUDIO_SGI as u16) };

    let executor: &'static mut Executor = unsafe {
        let p = core::ptr::addr_of_mut!(EXECUTOR);
        (*p).write(Executor::new());
        (*p).assume_init_mut()
    };
    executor.run(move |spawner: Spawner| {
        // Stash the spawner so the scheduler's add*Task C-ABI entry points (called
        // synchronously by the C++ app during registerTasks) can spawn task runners.
        scheduler::set_spawner(spawner);
        // The PIC pump decodes pad/button input concurrently with the app's tick
        // loop (which yields each tick, letting these tasks make progress).
        spawner.spawn(control::pic_pump().unwrap());
        // Bridges the encoder edge ISRs to the scheduler: unblocks the app's
        // self-blocking encoder task on movement (else encoders stay dead).
        spawner.spawn(control::encoder_wake_pump().unwrap());
        // The sole PIC transmitter: drains the LED/pad output queue.
        spawner.spawn(control::pad_render().unwrap());
        // The OLED render task: SSD1309 init + frame streaming over RSPI0.
        spawner.spawn(display::oled_render().unwrap());
        // CV DAC writer (shares RSPI0 with the OLED).
        spawner.spawn(cv_gate::cv_writer().unwrap());
        // External trigger-clock: calls the app's clock-in handler once per
        // captured rising edge (cooperatively, between app ticks).
        spawner.spawn(cv_gate::trigger_clock_task().unwrap());
        // USB-MIDI 1.0 peripheral: the device state machine + the two MIDI 1.0
        // endpoint pumps (host↔class byte queues, bridged by midi.rs port 1).
        spawner.spawn(usb::device_task(usb_midi.device).unwrap());
        spawner.spawn(usb::midi_rx_task(usb_midi.ep_out).unwrap());
        spawner.spawn(usb::midi_tx_task(usb_midi.ep_in).unwrap());
        spawner.spawn(app_task().unwrap());
    });
}

/// The application bring-up task. Runs the C++ app's one-time init, which on this
/// BSP also calls `registerTasks()` → spawns one Embassy task per registered
/// Deluge task via the [`scheduler`] C ABI. Those runners (plus the BSP's async
/// I/O tasks) then drive everything cooperatively, so this task has nothing left
/// to do and parks forever. The decomposed scheduler replaces the old yielding
/// `deluge_app_tick` superloop.
#[cfg(target_os = "none")]
#[embassy_executor::task]
async fn app_task() {
    use embassy_time::Timer;
    use rza1l_hal::gpio;

    // M1a sign-of-life: blink the SYNC LED (P6.7). Visible on the panel and
    // proves the executor, time driver and GPIO all work end-to-end.
    const SYNC_LED_PORT: u8 = 6;
    const SYNC_LED_BIT: u8 = 7;
    // SAFETY: we own this pin; GPIO clocks are up after init_clocks.
    unsafe { gpio::set_as_output(SYNC_LED_PORT, SYNC_LED_BIT) };
    for i in 0..6 {
        unsafe { gpio::write(SYNC_LED_PORT, SYNC_LED_BIT, true) };
        Timer::after_millis(120).await;
        unsafe { gpio::write(SYNC_LED_PORT, SYNC_LED_BIT, false) };
        Timer::after_millis(120).await;
        log::info!("deluge-rust: alive ({})", i);
    }

    // Bring SD up BEFORE deluge_app_init so the app's boot settings read finds the
    // card ready (no "SD CARD ERROR" popup). But first wait for the PIC's baud-rate
    // handshake (31250→200000, run by pic_pump) to finish: sd::init() racing that
    // handshake corrupts it → garbled pads. With the PIC already ready, sd::init has
    // nothing to race. Must be async — sd::init's embassy-time Timers can't run under
    // block_on (integrated timer queue).
    deluge_bsp::pic::wait_ready().await;
    crate::sd::boot_init().await;

    log::info!("deluge-rust: deluge_app_init() (registers + spawns task runners)");
    // deluge_app_init → registerTasks() spawns the per-task runners onto this
    // executor via scheduler::set_spawner's stashed spawner. They begin running as
    // soon as we yield below.
    unsafe { deluge_app_init(board::deluge_board()) };
    log::info!("deluge-rust: scheduler running; pumping async worker");

    // The scheduler's task runners own the periodic app work. This task now pumps
    // the worker fiber ([`fiber`]): it drives the long user-initiated operations
    // (song load, stem export, grid clip create) that `yield()` instead of
    // busy-waiting. Each pass starts/resumes a ready operation; between passes the
    // executor runs every other task + I/O, so the awaited work makes progress
    // (this is what lets song-load-while-playing complete instead of hanging on a
    // frozen executor).
    //
    // Wake-driven: sleep on WORKER_WAKE rather than polling on a fixed tick. While
    // an op is suspended a coarse fallback timer also re-checks (covers predicate
    // flips that aren't signalled — most are, via deluge_worker_run, the task
    // runners, and the SD waker); when idle we sleep until an op is submitted.
    use embassy_futures::select::select;
    loop {
        let busy = fiber::worker_poll();
        if busy {
            let _ = select(
                fiber::WORKER_WAKE.wait(),
                embassy_time::Timer::after_millis(8),
            )
            .await;
        } else {
            fiber::WORKER_WAKE.wait().await;
        }
    }
}

/// Host harness entry (`cargo build`/`cargo test` off-target, no `target_os =
/// "none"`). Exercises the M1-core modules (`fiber`, `scheduler`, `sd`,
/// `services`) on std without any device BSP/HAL — no MMU/GIC/SDRAM bring-up,
/// no C++ app linked (see build.rs). Proves the host binary actually links and
/// runs: the fiber context switch works on a std thread stack (Task 3), and the
/// `sd.rs` file-backed block-device shim round-trips real bytes through the same
/// `deluge_block_read`/`deluge_block_write` C ABI the app would call (Task 4).
/// Later M1 tasks grow this into a real host scheduler run.
#[cfg(not(target_os = "none"))]
fn main() {
    env_logger::init();
    log::info!("deluge-bsp-rust: HOST harness");
    // Run the fiber selftest (Task 3).
    assert!(fiber::selftest(), "fiber selftest failed on host");

    // sd.rs host shim round-trip (Task 4): write a known sector via the C-ABI
    // deluge_block_write, read it back via deluge_block_read, and verify the
    // bytes survive — proves the file-backed shim actually persists data, not
    // just that it links. Sector 1 (not 0): leaves a notional boot sector alone.
    const TEST_SECTOR: u32 = 1;
    let mut pattern = [0u8; 512];
    for (i, b) in pattern.iter_mut().enumerate() {
        *b = (i as u8).wrapping_mul(31).wrapping_add(7);
    }
    let status = sd::deluge_block_write(0, pattern.as_ptr(), TEST_SECTOR, 1);
    assert_eq!(
        status, 0,
        "sd round-trip: deluge_block_write failed (status={status})"
    );
    let mut readback = [0u8; 512];
    let status = sd::deluge_block_read(0, readback.as_mut_ptr(), TEST_SECTOR, 1);
    assert_eq!(
        status, 0,
        "sd round-trip: deluge_block_read failed (status={status})"
    );
    assert_eq!(
        pattern, readback,
        "sd round-trip: readback did not match what was written"
    );
    log::info!("deluge-bsp-rust: sd round-trip OK (sector {TEST_SECTOR}, 512 bytes)");

    // --- M3: whole-BSP host boot smoke -------------------------------------
    // Bring up a host Embassy executor (platform-std) and spawn the four
    // control/display tasks — the same ones main.rs spawns on device (minus the
    // app/audio/cv/usb tasks, which need the C++ app or real peripherals). With
    // no app and no input, each runs its init sequence then parks:
    //   pic_pump       → pic::init(); loops on pic::read_byte() (host: parks)
    //   pad_render     → pic::wait_ready(); OUT.receive() (parks, nothing queued)
    //   encoder_wake_pump → parks on ENCODER_WAKER (host: never fires)
    //   oled_render    → pic::wait_ready(); oled::init(); sends ONE blank frame
    //                    (captured by the oled host sim), then parks on wait_redraw
    // Boot is proven by the captured blank frame; the watchdog bounds it so a
    // regression (init that blocks/crashes) fails instead of hanging forever.
    use embassy_executor::{Executor, Spawner};
    use std::time::{Duration, Instant};

    std::thread::Builder::new()
        .name("deluge-bsp-boot".into())
        .spawn(|| {
            let executor: &'static mut Executor = Box::leak(Box::new(Executor::new()));
            executor.run(|spawner: Spawner| {
                crate::scheduler::set_spawner(spawner);
                spawner.spawn(control::pic_pump().unwrap());
                spawner.spawn(control::pad_render().unwrap());
                spawner.spawn(control::encoder_wake_pump().unwrap());
                spawner.spawn(display::oled_render().unwrap());
            });
        })
        .expect("spawning the host BSP executor thread");

    // Wait (bounded) for oled_render to capture its first (blank) frame.
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        // A blank frame is all-zero; captured_frame() returns the last frame the
        // oled sim received. Before the first send it is the sim's initial state;
        // we detect "boot reached first send" via a distinct signal below.
        if deluge_bsp::oled::boot_frame_captured() {
            break;
        }
        if Instant::now() >= deadline {
            panic!("host BSP boot: oled_render did not capture its blank frame in time");
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    log::info!("deluge-bsp-rust: whole-BSP host boot OK (control/display tasks quiescent)");

    log::info!("deluge-bsp-rust: HOST harness OK");
}
