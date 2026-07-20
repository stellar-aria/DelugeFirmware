//! SP0 differential harness: C FatFS vs embedded-fatfs over one RAM image.
pub mod ops;
pub mod ram_disk;
pub mod fatfs_c;
pub mod block_dev;
pub mod efatfs;
pub mod diff;
