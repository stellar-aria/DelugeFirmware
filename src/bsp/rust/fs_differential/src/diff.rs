//! Recursive read/enumerate differential: walks the whole tree through both
//! FatFS backends (via `ops::FsOps`) and asserts they agree byte-for-byte on
//! file contents and entry-for-entry on directory listings.
use crate::ops::{Entry, FsOps};

/// Walk the whole tree rooted at `/` through both `a` and `b`, comparing
/// directory listings and file contents at every level. Returns `Err` with a
/// precise description of the FIRST divergence found (depth-first, in
/// sorted-entry order), or `Ok(())` if the two stacks agree everywhere.
pub fn compare_read(a: &dyn FsOps, b: &dyn FsOps) -> Result<(), String> {
    walk(a, b, "/")
}

fn walk(a: &dyn FsOps, b: &dyn FsOps, dir: &str) -> Result<(), String> {
    let (ea, eb) = (a.read_dir(dir), b.read_dir(dir));
    if ea != eb {
        return Err(format!("dir {dir}: C={ea:?} vs E={eb:?}"));
    }
    for e in &ea {
        let child = child_path(dir, e);
        if e.is_dir {
            walk(a, b, &child)?;
        } else {
            let (fa, fb) = (a.read_file(&child), b.read_file(&child));
            if fa != fb {
                return Err(format!(
                    "file {child}: {} vs {} bytes / content differs",
                    fa.len(),
                    fb.len()
                ));
            }
        }
    }
    Ok(())
}

fn child_path(dir: &str, e: &Entry) -> String {
    if dir == "/" {
        format!("/{}", e.name)
    } else {
        format!("{dir}/{}", e.name)
    }
}
