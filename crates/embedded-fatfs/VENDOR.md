# VENDOR.md — embedded-fatfs

## Provenance

- **Source repo:** https://github.com/MabezDev/embedded-fatfs
- **Vendored rev:** `518528cc111fcf65c48abbdeb80735a38eada112` (short: `518528c`), dated 2026-05-01
- **Vendored subtree:** the core crate only — `embedded-fatfs/Cargo.toml`, `embedded-fatfs/src/`,
  `embedded-fatfs/LICENSE`, `embedded-fatfs/README.md` from the upstream workspace. The upstream
  workspace also contains `block-device-adapters`, `block-device-driver`, `embedded-partitions`, and
  `sdspi` — none of those are vendored here; the core crate's only dependencies are the published
  crates.io crates `bitflags = "1.0"` and `embedded-io-async = "0.7.0"`, kept as normal crates.io deps
  (not vendored).
- **License:** MIT (upstream `license = "MIT"` in `Cargo.toml`; `LICENSE` file preserved verbatim,
  copyright held by Rafał Harabień 2017 and Scott Mabin 2023). Compatible with this repo's GPL-3.0.
- **Workspace isolation:** `crates/embedded-fatfs/Cargo.toml` carries an empty `[workspace]` table so
  this directory is its own detached workspace root — it does not attach to (and cannot perturb)
  `crates/Cargo.toml`'s workspace/lockfile, which feeds the firmware build. It is intentionally NOT
  listed in `crates/Cargo.toml`'s `members`.
- **Not vendored:** upstream `tests/`, `examples/`, `resources/`, `scripts/`, `CHANGELOG.md`,
  `rustfmt.toml` — not needed to build the library; the `resources/*` exclude in `Cargo.toml` is
  inherited from upstream and left as-is (harmless, since those files aren't present here either).

## Why own the code instead of a git dependency

`embedded-fatfs` is unpublished (crates.io has no release); the prior task git-pinned it
(`rev = 518528c`) and confirmed it builds `no_std`-clean. This task replaces that git dependency with
a vendored path dependency so we can carry our own bug-fix and hardening commits without depending on
an upstream repo we don't control. Divergences this project's differential-testing work finds become
hardening commits in this owned copy.

## Applied upstream PR fixes (each its own commit, individually revertible)

(none yet — filled in as each fix lands, one commit per PR)

## Deferred (SP1 — block-device-adapters path, not vendored yet)

- **PR #68** — 32-bit `usize` overflow/truncation at ≥4 GiB in the block-device-adapters buffering
  layer. A real *our-target* data-loss bug once we're on the sector-oriented device path, but that
  crate isn't vendored in SP0 (SP0's `MemIo` harness implements `embedded-io-async` traits directly
  over a RAM image, bypassing block-device-adapters entirely).
- **PR #62** — `BufStream`/`StreamSlice` seek sign/overflow bug, also in block-device-adapters.
  Deferred alongside #68 for the same reason.

## Deferred (write-differential loop — Task 6, evidence-driven hand-ports)

- **FAT32 `..`-cluster-zero bug** (upstream `rafalh/rust-fatfs` commit `c4bb769`; lives in our
  `dir.rs:418` write path, root-directory `..` entries should point at cluster 0 but the vendored code
  doesn't special-case it in all write paths). No clean upstream PR against `embedded-fatfs` to apply;
  plan is to let Task 6's write-differential harness demonstrate the bug on our vendored copy first,
  then fix + show the fix green — evidence before the patch, not a blind hand-port.
- **PR #66** — rename `..`-update fix; shares the same root-cluster caveat as the item above. Deferred
  for the same evidence-driven reason.

## Skipped (rejected, not deferred)

- **PR #32** — treats a corrupted LFN entry as a hard error that aborts the whole directory listing.
  Rejected: cuts against this project's degrade-gracefully-on-power-loss posture (we'd rather list what
  we can than fail the whole directory over one bad LFN chain).
- **FSInfo / `format_volume` fix** (upstream commit `2f1bf3e`) — Deluge never formats a card
  (`FF_USE_MKFS = 0` in the existing FatFs config), so `format_volume` is unreachable code for us; not
  worth carrying the fix.
