//! Recursive read/enumerate differential: walks the whole tree through both
//! FatFS backends (via `ops::FsOps`) and asserts they agree byte-for-byte on
//! file contents and entry-for-entry on directory listings.
//!
//! `replay_and_compare` (Task 6) extends this to the write path.
use crate::efatfs::EFatFs;
use crate::fatfs_c::CFatFs;
use crate::ops::{Entry, FsOps, FsOpsMut, Op};
use crate::ram_disk::RamDisk;

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

/// A backend-agnostic snapshot of a filesystem tree, captured (via `FsOps`)
/// while a backend is live over the shared `DISK` image. Both C FatFS and
/// embedded-fatfs address that same global (`ram_disk.rs`), so they can
/// never be mounted against two *different* images at once -- which is
/// exactly what `replay_and_compare` needs, since each backend replays the
/// op sequence on its own copy of the starting fixture. Capturing each
/// backend's whole tree into this owned, disk-independent form first lets
/// `compare_read` diff the two results afterward with neither backend still
/// mounted.
enum Node {
    File(Vec<u8>),
    Dir(Vec<(Entry, Node)>),
}

struct Captured(Node);

impl Captured {
    fn capture(fs: &dyn FsOps) -> Self {
        Captured(Self::capture_dir(fs, "/"))
    }

    fn capture_dir(fs: &dyn FsOps, path: &str) -> Node {
        let entries = fs.read_dir(path);
        let mut kids = Vec::with_capacity(entries.len());
        for e in entries {
            let child = child_path(path, &e);
            let node = if e.is_dir {
                Self::capture_dir(fs, &child)
            } else {
                Node::File(fs.read_file(&child))
            };
            kids.push((e, node));
        }
        Node::Dir(kids)
    }

    fn lookup(&self, path: &str) -> &Node {
        let mut cur = &self.0;
        for part in path.split('/').filter(|s| !s.is_empty()) {
            match cur {
                Node::Dir(kids) => {
                    cur = &kids
                        .iter()
                        .find(|(e, _)| e.name == part)
                        .unwrap_or_else(|| panic!("captured tree: no entry {part:?} on path {path}"))
                        .1;
                }
                Node::File(_) => panic!("captured tree: {path} descends through a file"),
            }
        }
        cur
    }
}

impl FsOps for Captured {
    fn read_file(&self, path: &str) -> Vec<u8> {
        match self.lookup(path) {
            Node::File(b) => b.clone(),
            Node::Dir(_) => panic!("read_file({path}) on a captured directory"),
        }
    }
    fn read_dir(&self, path: &str) -> Vec<Entry> {
        match self.lookup(path) {
            Node::Dir(kids) => kids.iter().map(|(e, _)| e.clone()).collect(),
            Node::File(_) => panic!("read_dir({path}) on a captured file"),
        }
    }
}

/// Runs `ops` against BOTH backends, each on its OWN fresh copy of the image
/// at `img_path` -- write ops mutate the shared `DISK` RAM image, so the two
/// backends must never run concurrently against one mutable copy (see
/// `ram_disk.rs`, `Captured` above). Captures each backend's resulting tree,
/// then diffs those two snapshots with the same `compare_read` the read-path
/// differential uses. Compares the LOGICAL tree only -- never the raw image
/// bytes, since free-space maps / FSInfo / allocation order legitimately
/// differ between the two implementations.
pub fn replay_and_compare(img_path: &str, ops: &[Op]) -> Result<(), String> {
    let orig = std::fs::read(img_path).unwrap_or_else(|e| panic!("read fixture image {img_path}: {e}"));

    let _disk = RamDisk::load_bytes(&orig);
    let mut c = CFatFs::mount();
    for op in ops {
        c.apply(op);
    }
    let c_tree = Captured::capture(&c);
    drop(c);

    let _disk = RamDisk::load_bytes(&orig);
    let mut e = EFatFs::mount();
    for op in ops {
        e.apply(op);
    }
    let e_tree = Captured::capture(&e);
    drop(e);

    compare_read(&c_tree, &e_tree)
}
