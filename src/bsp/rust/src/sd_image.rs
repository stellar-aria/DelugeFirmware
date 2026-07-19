//! Streaming-underrun harness: assembles a REAL FAT-formatted SD card image containing a
//! song + its samples for the [`crate::scenario`] driver to load, reusing the GOLDEN
//! HARNESS's own corpus/packing tooling instead of hand-rolling a FAT image (per the Task
//! 5 brief). Two steps, both delegated to already-proven tooling rather than reimplemented
//! here:
//!   1. the project TREE (`SONGS/…`, `SAMPLES/…`) — `scripts/golden_mixdown.sh
//!      reconstruct`, the exact script the golden-master harness uses to rebuild its
//!      fixture from the developer's local `~/Deluge Backup` corpus (see this repo's
//!      song-corpus-location note).
//!   2. the FAT IMAGE itself — mtools (`mformat`+`mcopy`), the SAME approach
//!      `src/bsp/host/host_render_main.cpp`'s `pack_image()` uses for `deluge_render`/
//!      `deluge_host` (32 KB clusters, >= 2.5 GB floor — FAT32 needs >= 65525 clusters at
//!      that cluster size, matching real Deluge SD card geometry).
//!
//! Host-only (`host_app`); never compiled for device.
#![cfg(all(not(target_os = "none"), feature = "host_app"))]

use std::path::{Path, PathBuf};
use std::process::Command;

/// Ensures fixture `name`'s project tree exists locally (reconstructing it from
/// `DELUGE_BACKUP`/`DELUGE_GOLDEN_DIR` via `scripts/golden_mixdown.sh reconstruct` if not
/// already cached — same env-var conventions as the golden harness), then packs it into a
/// fresh temporary FAT32 image. Returns the image path (a process-unique temp file; the
/// caller is responsible for pointing `DELUGE_SD_IMAGE` at it before any SD access).
///
/// Panics with a clear message on failure (missing local backup corpus, missing `mtools`,
/// etc.) — this is one-time harness setup, not something to silently degrade under.
pub fn pack_golden_fixture(repo_root: &Path, fixture: &str) -> PathBuf {
    let golden_dir = std::env::var("DELUGE_GOLDEN_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            PathBuf::from(std::env::var("HOME").expect("HOME must be set"))
                .join(".cache/deluge-golden")
        });
    let proj = golden_dir.join(format!("{fixture}_proj"));

    if !proj.is_dir() {
        log::info!(
            "streaming-scenario: reconstructing '{fixture}' project tree at {} (scripts/golden_mixdown.sh reconstruct)",
            proj.display()
        );
        let status = Command::new("bash")
            .arg(repo_root.join("scripts/golden_mixdown.sh"))
            .arg("reconstruct")
            .env("FIXTURE", fixture)
            .current_dir(repo_root)
            .status()
            .expect("run scripts/golden_mixdown.sh reconstruct");
        assert!(
            status.success(),
            "scripts/golden_mixdown.sh reconstruct failed for fixture '{fixture}' — is DELUGE_BACKUP \
             (default '~/Deluge Backup') present?"
        );
        assert!(
            proj.is_dir(),
            "reconstruct did not produce {} — see the golden_mixdown.sh output above",
            proj.display()
        );
    }

    pack_image(&proj)
}

/// Packs `project_dir` into a fresh sparse FAT32 image via mtools. Mirrors
/// `host_render_main.cpp`'s `pack_image()` exactly: `truncate` a sparse file (only written
/// samples occupy real disk blocks), `mformat -c 64` (32 KB clusters — the geometry a real
/// Deluge SD card uses; mtools' size-based default would pick tiny 2 KB clusters, which
/// makes the firmware stream in far smaller Clusters than on-device), then `mcopy -s` the
/// whole project tree onto the image root.
fn pack_image(project_dir: &Path) -> PathBuf {
    let img = std::env::temp_dir().join(format!(
        "deluge-streaming-scenario-{}.img",
        std::process::id()
    ));

    let dir_bytes = dir_size_bytes(project_dir);
    let mut bytes = dir_bytes.saturating_mul(2) + (64u64 << 20);
    let min_bytes = 2560u64 << 20; // 2.5 GB -> comfortably >= 65525 32 KB clusters (valid FAT32)
    if bytes < min_bytes {
        bytes = min_bytes;
    }
    bytes = (bytes + 511) & !511u64;

    // Shelled out (not separate `Command`s with Rust-side globbing) so `mcopy`'s `*` gets
    // the SAME shell glob expansion `host_render_main.cpp`'s `system()` calls rely on.
    let script = format!(
        "set -e; truncate -s {bytes} '{img}'; mformat -i '{img}' -F -c 64 ::; \
         mcopy -s -Q -i '{img}' '{proj}'/* ::/",
        bytes = bytes,
        img = img.display(),
        proj = project_dir.display(),
    );
    let status = Command::new("sh")
        .arg("-c")
        .arg(&script)
        .status()
        .expect("run the mtools pack script (sh)");
    assert!(
        status.success(),
        "packing '{}' into a FAT image failed — is mtools (mformat/mcopy) installed? script: {script}",
        project_dir.display()
    );

    log::info!(
        "streaming-scenario: packed '{}' -> {} ({bytes} bytes)",
        project_dir.display(),
        img.display()
    );
    img
}

fn dir_size_bytes(dir: &Path) -> u64 {
    let out = Command::new("du")
        .arg("-sb")
        .arg(dir)
        .output()
        .expect("run du -sb");
    String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .next()
        .and_then(|s| s.parse::<u64>().ok())
        .unwrap_or(0)
}
