//! The SEQUENTIAL capture-then-compare differential (mirrors
//! `fs_differential::replay_and_compare`).
//!
//! The C++ backing is a process-wide singleton, so two backings can never run
//! live in lockstep. Instead: run the whole op sequence on backend A, capturing
//! each op's backend-agnostic result into a `Vec<OpResult>`; DROP A; run the
//! SAME sequence on a fresh backend B; capture ITS `Vec`; then diff the two
//! `Vec`s element-wise. This is exactly what lets SR2d's Rust backing drop in as
//! backend B unchanged — the driver only ever holds one backing live.
use crate::ops::{Op, Region, RegionPortOps, RegionState};

/// One op's backend-agnostic captured result — the unit the diff compares. The
/// raw lease token is DELIBERATELY absent: it is a backend-specific pointer and
/// diffing it across two backings would be meaningless. `leases` (the total
/// held-lease count after the op) is the observable side-effect that makes
/// `Retain`/`Release`/`close` hygiene diffable.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OpResult {
    pub op_desc: String,
    pub state: RegionState,
    pub region: Option<Region>,
    pub leases: u32,
}

/// The first field-level divergence between two captured sequences.
#[derive(Debug, Clone)]
pub struct Divergence {
    pub op_index: usize,
    pub op_desc: String,
    pub field: &'static str,
    pub a: String,
    pub b: String,
}

impl std::fmt::Display for Divergence {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "divergence at op #{} [{}]: field `{}` differs — A={} vs B={}",
            self.op_index, self.op_desc, self.field, self.a, self.b
        )
    }
}

/// Drive `ops` through ONE backing, capturing each op's backend-agnostic result.
///
/// `Retain`/`Release` act on the lease of the LAST `Ready` acquire (the caller's
/// independent-pin retain/release). Their captured `state` is a placeholder
/// (`Unavailable`) — identical on both backings — with the real, diffable effect
/// carried by the `leases` count.
pub fn capture(backend: &mut dyn RegionPortOps, ops: &[Op]) -> Vec<OpResult> {
    let mut last_lease = 0u64;
    let mut out = Vec::with_capacity(ops.len());
    for op in ops {
        let (state, region) = match op {
            Op::Acquire { index, direction, priority } => {
                let o = backend.acquire(*index, *direction, *priority);
                if o.state == RegionState::Ready {
                    last_lease = o.lease;
                }
                (o.state, o.region)
            }
            Op::State { index } => (backend.state(*index), None),
            Op::Retain => {
                backend.retain(last_lease);
                (RegionState::Unavailable, None)
            }
            Op::Release => {
                backend.release(last_lease);
                (RegionState::Unavailable, None)
            }
        };
        out.push(OpResult {
            op_desc: format!("{op:?}"),
            state,
            region,
            leases: backend.total_leases(),
        });
    }
    out
}

/// Diff two captured sequences element-wise; return the FIRST divergence (op
/// index + which field + the two values), or `Ok(())` if byte-identical.
pub fn compare(a: &[OpResult], b: &[OpResult]) -> Result<(), Divergence> {
    if a.len() != b.len() {
        return Err(Divergence {
            op_index: a.len().min(b.len()),
            op_desc: "<length>".to_string(),
            field: "sequence length",
            a: a.len().to_string(),
            b: b.len().to_string(),
        });
    }
    for (i, (ra, rb)) in a.iter().zip(b.iter()).enumerate() {
        let div = |field, av: String, bv: String| Divergence {
            op_index: i,
            op_desc: ra.op_desc.clone(),
            field,
            a: av,
            b: bv,
        };
        if ra.state != rb.state {
            return Err(div("state", format!("{:?}", ra.state), format!("{:?}", rb.state)));
        }
        match (&ra.region, &rb.region) {
            (Some(rega), Some(regb)) => {
                if let Some(d) = compare_region(i, &ra.op_desc, rega, regb) {
                    return Err(d);
                }
            }
            (None, None) => {}
            (Some(_), None) => return Err(div("region", "Some".into(), "None".into())),
            (None, Some(_)) => return Err(div("region", "None".into(), "Some".into())),
        }
        if ra.leases != rb.leases {
            return Err(div("leases", ra.leases.to_string(), rb.leases.to_string()));
        }
    }
    Ok(())
}

fn compare_region(i: usize, op_desc: &str, a: &Region, b: &Region) -> Option<Divergence> {
    let div = |field, av: String, bv: String| Divergence {
        op_index: i,
        op_desc: op_desc.to_string(),
        field,
        a: av,
        b: bv,
    };
    if a.region_index != b.region_index {
        return Some(div("region_index", a.region_index.to_string(), b.region_index.to_string()));
    }
    if a.resident_bytes != b.resident_bytes {
        return Some(div("resident_bytes", a.resident_bytes.to_string(), b.resident_bytes.to_string()));
    }
    if a.payload != b.payload {
        // Point at the first differing byte for a precise report.
        let at = a.payload.iter().zip(b.payload.iter()).position(|(x, y)| x != y);
        let (av, bv) = match at {
            Some(k) => (format!("payload[{k}]={:#04x}", a.payload[k]), format!("payload[{k}]={:#04x}", b.payload[k])),
            None => (format!("payload len {}", a.payload.len()), format!("payload len {}", b.payload.len())),
        };
        return Some(div("payload", av, bv));
    }
    None
}
