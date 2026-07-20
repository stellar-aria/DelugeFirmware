//! Host `block_device_driver::BlockDevice<512>` over the shared `DISK` image
//! (`ram_disk.rs`), for `efatfs::EFatFs` to mount `embedded-fatfs` through --
//! the real `BlockDevice` -> `BufStream` stack (`crates/block-device-driver`,
//! `crates/block-device-adapters`) SP1 will drive on-device, instead of the
//! byte-granular `MemIo` shortcut SP0 used.
//!
//! `RamDisk::read_at`/`write_at` are already byte-granular over `DISK`, so
//! this just walks the requested blocks and delegates each 512-byte chunk to
//! them -- same shared image the C FatFS FFI bridge's `disk_read`/`disk_write`
//! callbacks address sector-granular.

use aligned::{Aligned, A4};
use block_device_driver::BlockDevice;

use crate::ram_disk::RamDisk;

/// Block size this harness's fixtures and the vendored C FatFS `ffconf.h`
/// both fix (`FF_MIN_SS == FF_MAX_SS == 512`, see `ram_disk.rs`).
const BLOCK_SIZE: usize = 512;

/// A `BlockDevice<512>` over the shared `DISK` image.
pub struct FileBlockDevice;

impl BlockDevice<BLOCK_SIZE> for FileBlockDevice {
    type Align = A4;
    type Error = core::convert::Infallible;

    async fn read(
        &mut self,
        block_address: u32,
        data: &mut [Aligned<A4, [u8; BLOCK_SIZE]>],
    ) -> Result<(), Self::Error> {
        for (i, blk) in data.iter_mut().enumerate() {
            let off = (block_address as u64 + i as u64) * BLOCK_SIZE as u64;
            RamDisk::read_at(off, &mut blk[..]);
        }
        Ok(())
    }

    async fn write(
        &mut self,
        block_address: u32,
        data: &[Aligned<A4, [u8; BLOCK_SIZE]>],
    ) -> Result<(), Self::Error> {
        for (i, blk) in data.iter().enumerate() {
            let off = (block_address as u64 + i as u64) * BLOCK_SIZE as u64;
            RamDisk::write_at(off, &blk[..]);
        }
        Ok(())
    }

    async fn size(&mut self) -> Result<u64, Self::Error> {
        Ok(RamDisk::len())
    }
}
