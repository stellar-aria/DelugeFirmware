//! SPIKE (SR2b) — THROWAWAY de-risk crate, superseded by SR2d.
//!
//! FFI declarations for the `extern "C"` shims (cpp/shim.cpp) over the app's convert.cpp / stitch.cpp,
//! plus `#[test]`s that (a) round-trip through the cc-built x86-SIMDe objects to prove they behave
//! correctly — mirroring tests/spec_audio_stream/{convert,stitch,convert_cluster}_spec.cpp — and
//! (b) assert the armv7a-NEON compile-check (stamped by build.rs) succeeded.

// RawDataFormat (src/deluge/storage/audio/audio_file_format.h) as u8.
#[allow(dead_code)]
mod format {
    pub const NATIVE: u8 = 0;
    pub const FLOAT: u8 = 1;
    pub const UNSIGNED_8: u8 = 2;
    pub const ENDIANNESS_WRONG_16: u8 = 3;
    pub const ENDIANNESS_WRONG_24: u8 = 4;
    pub const ENDIANNESS_WRONG_32: u8 = 5;
}

// Used only under cfg(test); the non-test lib build sees them as unused.
#[allow(dead_code)]
extern "C" {
    fn spike_convert_word(word: i32, format: u8) -> i32;

    fn spike_convert_cluster_data(
        data: *mut u8,
        data_len: usize,
        cluster_index: i32,
        format: u8,
        audio_data_start_pos_bytes: u32,
        audio_data_length_bytes: u64,
        first_cluster_index_with_no_audio_data: i32,
        cluster_size: usize,
        cluster_size_magnitude: usize,
        unconverted_head_out: *mut u8, // 3 bytes
        yield_counter: *mut i32,       // nullable
    );

    #[allow(clippy::too_many_arguments)]
    fn spike_stitch_boundaries(
        self_data: *mut u8,
        self_len: usize,
        cluster_index: i32,
        format: u8,
        audio_data_start_pos_bytes: u32,
        cluster_size: usize,
        self_start: *mut bool,
        self_end: *mut bool,
        prev_tail: *mut u8, // null => no prev
        prev_tail_len: usize,
        prev_end_converted: *mut bool,
        next_head: *mut u8, // null => no next
        next_head_len: usize,
        next_unconverted_head: *const u8, // 3 bytes
        next_start_converted: *mut bool,
    );
}

#[cfg(test)]
mod tests {
    use super::format::*;
    use super::*;

    fn conv_cluster(
        data: &mut [u8],
        cluster_index: i32,
        format: u8,
        start_pos: u32,
        len_bytes: u64,
        first_no_audio: i32,
        cluster_size: usize,
        magnitude: usize,
        yield_counter: Option<&mut i32>,
    ) -> [u8; 3] {
        let mut head = [0u8; 3];
        let yc = yield_counter.map_or(std::ptr::null_mut(), |c| c as *mut i32);
        let dlen = data.len();
        unsafe {
            spike_convert_cluster_data(
                data.as_mut_ptr(), dlen, cluster_index, format, start_pos, len_bytes,
                first_no_audio, cluster_size, magnitude, head.as_mut_ptr(), yc,
            );
        }
        head
    }

    // ---- convert_word: mirrors convert_spec.cpp ------------------------------------------------
    #[test]
    fn convert_word_matches_spec() {
        unsafe {
            assert_eq!(spike_convert_word(0x11223344u32 as i32, NATIVE), 0x11223344u32 as i32);
            assert_eq!(spike_convert_word(0x11223344u32 as i32, ENDIANNESS_WRONG_24), 0x11223344u32 as i32);
            assert_eq!(spike_convert_word(0x01020304, ENDIANNESS_WRONG_32), 0x04030201);
            assert_eq!(spike_convert_word(0x01020304, ENDIANNESS_WRONG_16), 0x02010403);
            assert_eq!(spike_convert_word(0x00112233, UNSIGNED_8), 0x8091A2B3u32 as i32);
            // FLOAT 0.5f -> Q31 0x40000000
            assert_eq!(spike_convert_word(0.5f32.to_bits() as i32, FLOAT), 0x40000000);
        }
    }

    // ---- convert_cluster_data UNSIGNED_8 across a 16-byte SIMD boundary: mirrors convert_cluster_spec
    #[test]
    fn convert_cluster_unsigned8_simd_region() {
        let cluster_size = 128;
        let mut data = vec![0u8; cluster_size];
        // Region [4, 80): 76 bytes -> every byte XORed with 0x80.
        conv_cluster(&mut data, 0, UNSIGNED_8, 4, 76, 1, cluster_size, 7, None);
        assert_eq!(data[0], 0x00);
        assert_eq!(data[3], 0x00);
        for b in &data[4..80] {
            assert_eq!(*b, 0x80);
        }
        assert_eq!(data[80], 0x00);
        assert_eq!(data[127], 0x00);
    }

    // ---- convert_cluster_data ENDIANNESS_WRONG_32 across a SIMD boundary + scalar tail --------
    #[test]
    fn convert_cluster_wrong32_simd_region() {
        let cluster_size = 128;
        let mut data: Vec<u8> = (0..cluster_size as u16).map(|i| i as u8).collect();
        // Region [4, 72): every word has all 4 bytes reversed.
        conv_cluster(&mut data, 0, ENDIANNESS_WRONG_32, 4, 68, 1, cluster_size, 7, None);
        assert_eq!(data[0], 0);
        assert_eq!(data[3], 3);
        let mut w = 4usize;
        while w < 72 {
            for k in 0..4 {
                assert_eq!(data[w + k], (w + 3 - k) as u8, "word at {w}, k={k}");
            }
            w += 4;
        }
        assert_eq!(data[72], 72);
        assert_eq!(data[127], 127);
    }

    // ---- convert_cluster_data ENDIANNESS_WRONG_24 backup + swap: mirrors convert_cluster_spec ---
    #[test]
    fn convert_cluster_wrong24_swaps_and_backs_up() {
        let cluster_size = 32;
        let mut data: Vec<u8> = (0..cluster_size as u16).map(|i| i as u8).collect();
        let backup = conv_cluster(&mut data, 0, ENDIANNESS_WRONG_24, 2, 1000, 5, cluster_size, 5, None);
        assert_eq!(backup, [0, 1, 2]);
        assert_eq!(&data[0..2], &[0, 1]); // before audio start untouched
        assert_eq!(&data[2..5], &[4, 3, 2]); // first group [2,3,4] -> [4,3,2]
        assert_eq!(&data[29..32], &[31, 30, 29]); // last group [29,30,31] -> [31,30,29]
    }

    // ---- the no-op-Yield concretization still fires the cadence: mirrors convert_cluster_spec ---
    #[test]
    fn convert_cluster_invokes_yield() {
        let cluster_size = 4096;
        let mut data = vec![0u8; cluster_size];
        let mut yc = 0i32;
        conv_cluster(&mut data, 0, ENDIANNESS_WRONG_24, 3, 100_000, 5, cluster_size, 12, Some(&mut yc));
        assert!(yc >= 1, "expected at least one yield, got {yc}");
    }

    // ---- stitch_boundaries no neighbors: self_data unchanged, flags untouched ------------------
    #[test]
    fn stitch_no_neighbors_unchanged() {
        let cluster_size = 32;
        let mut self_data: Vec<u8> = (0..(cluster_size + 7) as u16).map(|i| i as u8).collect();
        let original = self_data.clone();
        let (mut ss, mut se) = (false, false);
        let slen = self_data.len();
        unsafe {
            spike_stitch_boundaries(
                self_data.as_mut_ptr(), slen, 1, UNSIGNED_8, 1, cluster_size,
                &mut ss, &mut se,
                std::ptr::null_mut(), 0, std::ptr::null_mut(),
                std::ptr::null_mut(), 0, std::ptr::null(), std::ptr::null_mut(),
            );
        }
        assert_eq!(self_data, original);
        assert!(!ss);
        assert!(!se);
    }

    // ---- stitch_boundaries UNSIGNED_8 misaligned, both neighbors: mirrors stitch_spec ----------
    #[test]
    fn stitch_unsigned8_misaligned_both_neighbors() {
        let cluster_size = 32;
        let mut self_data: Vec<u8> = (0..(cluster_size + 7) as u16).map(|i| i as u8).collect();
        let mut prev_tail: Vec<u8> = (0..11u16).map(|i| (100 + i) as u8).collect();
        let mut next_head: Vec<u8> = (0..7u16).map(|i| (200 + i) as u8).collect();
        let next_unconverted_head: [u8; 3] = [50, 51, 52];
        let (mut ss, mut se) = (false, false);
        let mut prev_end = false;
        let mut next_start = false;
        let slen = self_data.len();
        let ptlen = prev_tail.len();
        let nhlen = next_head.len();
        unsafe {
            spike_stitch_boundaries(
                self_data.as_mut_ptr(), slen, 1, UNSIGNED_8, 1, cluster_size,
                &mut ss, &mut se,
                prev_tail.as_mut_ptr(), ptlen, &mut prev_end,
                next_head.as_mut_ptr(), nhlen, next_unconverted_head.as_ptr(), &mut next_start,
            );
        }
        // Prev half.
        assert_eq!(prev_tail[0], 100);
        assert_eq!(&prev_tail[1..5], &[229, 230, 231, 128]);
        assert_eq!(&self_data[0..3], &[128, 1, 2]);
        assert!(prev_end);
        // Next half.
        assert_eq!(&self_data[29..33], &[157, 158, 159, 72]);
        assert_eq!(next_head[0], 72);
        assert_eq!(&next_head[1..7], &[201, 202, 203, 204, 205, 206]);
        assert!(next_start);
        assert!(ss);
        assert!(se);
    }

    // ---- armv7a-NEON compile-check (stamped by build.rs; no QEMU run) ---------------------------
    #[test]
    fn armv7a_neon_compiles() {
        let stamp = include_str!(concat!(env!("OUT_DIR"), "/arm_compile_result.txt"));
        assert!(
            stamp.starts_with("OK"),
            "armv7a-NEON cc compile-check failed:\n{stamp}"
        );
    }
}
