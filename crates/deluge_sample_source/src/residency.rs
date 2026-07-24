//! The seam between the cursor's residency POLICY (promotion / LOADING-retain /
//! READY-fuse / prefetch — reimplemented in `cursor.rs`) and HOW a chunk becomes
//! resident. The production provider is `ManagerResidency` (facade-backed); the
//! SR2c differential supplies a Scenario-driven provider as the gate's second
//! backend. The cursor is generic over this trait (monomorphized — no vtable on
//! the acquire hot path).

/// The tri-state result of asking a provider to make a region resident, mirroring
/// `DelugeRegionState`. `Ready`/`Loading` carry the pin the cursor takes ownership
/// of; `Unavailable` pins nothing.
pub enum Get<P> {
    Ready(P),
    Loading(P),
    Unavailable,
}

/// A held, pinned residency of exactly ONE region. `Drop` releases the pin — the
/// cursor stores these in `Cell<Option<Pin>>` and drops the outgoing one on every
/// transition, so lease balance is by ownership, never by hand.
pub trait RegionPin {
    /// The cluster index this pin tracks (residency identity is by index — the
    /// cursor's three slots never track the same index at once).
    fn index(&self) -> u32;
    /// LIVE readiness — a pin taken as `Loading` may have since landed. The indexed
    /// `state` query and the promotion path both re-read this.
    fn is_ready(&self) -> bool;
    /// The pinned payload base. Meaningful only while `is_ready()`.
    fn payload(&self) -> *const u8;
    /// The opaque generation-checked independent-pin token for this region (fed to
    /// the C-ABI `retain`/`release`). `0` if the pin can't mint one.
    fn token(&self) -> u64;
}

pub trait Residency {
    type Pin: RegionPin;
    fn acquire(&self, index: u32, priority: u32) -> Get<Self::Pin>;
    fn num_clusters(&self) -> u32;
    fn retain_token(&self, token: u64);
    fn release_token(&self, token: u64);
}
