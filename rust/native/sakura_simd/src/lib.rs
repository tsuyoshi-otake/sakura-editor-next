#![no_std]
#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(improper_ctypes_definitions)]
#![deny(clippy::missing_safety_doc)]
#![deny(clippy::undocumented_unsafe_blocks)]

#[cfg(test)]
extern crate std;

use core::mem;
use core::slice;

const U16_BYTES: usize = mem::size_of::<u16>();
const BYTE_KERNEL_INTERNAL_BOUNDARY: usize = 512;

#[inline]
fn find_byte_cr_or_lf_scalar(input: &[u8]) -> usize {
    let mut index = 0;
    while index < input.len() {
        let value = input[index];
        if value == b'\r' || value == b'\n' {
            return index;
        }
        index += 1;
    }
    input.len()
}

#[inline]
fn find_cr_or_lf_scalar(input: &[u16]) -> usize {
    let mut index = 0;
    while index < input.len() {
        let value = input[index];
        if value == b'\r' as u16 || value == b'\n' as u16 {
            return index;
        }
        index += 1;
    }
    input.len()
}

#[inline]
fn is_markdown_special(value: u16) -> bool {
    value == b'\\' as u16
        || value == b'`' as u16
        || value == b'!' as u16
        || value == b'[' as u16
        || value == b'*' as u16
        || value == b'_' as u16
        || value == b'~' as u16
        || value == b'<' as u16
        || value == b'&' as u16
        || value == b'$' as u16
}

#[inline]
fn find_markdown_special_scalar(input: &[u16]) -> usize {
    let mut index = 0;
    while index < input.len() {
        if is_markdown_special(input[index]) {
            return index;
        }
        index += 1;
    }
    input.len()
}

#[inline]
fn find_char_scalar(input: &[u16], target: u16) -> usize {
    let mut index = 0;
    while index < input.len() {
        if input[index] == target {
            return index;
        }
        index += 1;
    }
    input.len()
}

#[cfg(target_arch = "x86_64")]
mod x86_64_kernels {
    use super::{
        find_byte_cr_or_lf_scalar, find_char_scalar, find_cr_or_lf_scalar,
        find_markdown_special_scalar, is_markdown_special, BYTE_KERNEL_INTERNAL_BOUNDARY,
    };
    use core::arch::x86_64::*;

    #[inline]
    #[target_feature(enable = "avx")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized bytes for its entire length. The unaligned loads are
    /// confined to in-bounds portions of that slice.
    pub unsafe fn find_byte_cr_or_lf_avx128(input: &[u8]) -> usize {
        let cr = _mm_set1_epi8(b'\r' as i8);
        let lf = _mm_set1_epi8(b'\n' as i8);
        let mut offset = 0;

        // 512 is an implementation boundary shared with the C++ byte
        // scanner. It is deliberately kept inside the kernel and is not a
        // caller policy minimum.
        while offset < BYTE_KERNEL_INTERNAL_BOUNDARY && input.len() - offset >= 16 {
            // SAFETY: The loop condition proves that 16 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm_loadu_si128(input.as_ptr().add(offset).cast()) };
            let matches = _mm_or_si128(_mm_cmpeq_epi8(values, cr), _mm_cmpeq_epi8(values, lf));
            let mask = _mm_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize);
            }
            offset += 16;
        }

        while input.len() - offset >= 16 {
            // SAFETY: The loop condition proves that 16 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm_loadu_si128(input.as_ptr().add(offset).cast()) };
            let matches = _mm_or_si128(_mm_cmpeq_epi8(values, cr), _mm_cmpeq_epi8(values, lf));
            let mask = _mm_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize);
            }
            offset += 16;
        }

        offset + find_byte_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized bytes for its entire length. The unaligned loads are
    /// confined to in-bounds portions of that slice.
    pub unsafe fn find_byte_cr_or_lf_avx2(input: &[u8]) -> usize {
        let cr = _mm256_set1_epi8(b'\r' as i8);
        let lf = _mm256_set1_epi8(b'\n' as i8);
        let mut offset = 0;

        while offset < BYTE_KERNEL_INTERNAL_BOUNDARY && input.len() - offset >= 32 {
            // SAFETY: The loop condition proves that 32 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast()) };
            let matches =
                _mm256_or_si256(_mm256_cmpeq_epi8(values, cr), _mm256_cmpeq_epi8(values, lf));
            let mask = _mm256_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize);
            }
            offset += 32;
        }

        // Keep the two-vector hot loop explicit. The 512-byte handoff above
        // is a kernel detail, never a dispatch/policy threshold.
        while input.len() - offset >= 64 {
            // SAFETY: The loop condition proves that both 32-byte loads stay
            // within the slice; the loads permit any byte alignment.
            let values0 = unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast()) };
            // SAFETY: The same loop condition proves that the second 32-byte
            // load stays within the slice and it permits any byte alignment.
            let values1 = unsafe { _mm256_loadu_si256(input.as_ptr().add(offset + 32).cast()) };
            let matches0 = _mm256_or_si256(
                _mm256_cmpeq_epi8(values0, cr),
                _mm256_cmpeq_epi8(values0, lf),
            );
            let matches1 = _mm256_or_si256(
                _mm256_cmpeq_epi8(values1, cr),
                _mm256_cmpeq_epi8(values1, lf),
            );
            let aggregate = _mm256_movemask_epi8(_mm256_or_si256(matches0, matches1)) as u32;
            if aggregate != 0 {
                let mask0 = _mm256_movemask_epi8(matches0) as u32;
                if mask0 != 0 {
                    return offset + (mask0.trailing_zeros() as usize);
                }
                return offset + 32 + (aggregate.trailing_zeros() as usize);
            }
            offset += 64;
        }

        while input.len() - offset >= 32 {
            // SAFETY: The loop condition proves that 32 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast()) };
            let matches =
                _mm256_or_si256(_mm256_cmpeq_epi8(values, cr), _mm256_cmpeq_epi8(values, lf));
            let mask = _mm256_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize);
            }
            offset += 32;
        }

        offset + find_byte_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2,avx512f,avx512bw")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2, AVX-512F, and AVX-512BW with XMM/YMM, opmask, and ZMM state
    /// enabled. `input` must be a valid slice of initialized bytes for its
    /// entire length. The unaligned loads are confined to in-bounds portions
    /// of that slice.
    pub unsafe fn find_byte_cr_or_lf_avx512bw(input: &[u8]) -> usize {
        let cr = _mm512_set1_epi8(b'\r' as i8);
        let lf = _mm512_set1_epi8(b'\n' as i8);
        let mut offset = 0;

        while offset < BYTE_KERNEL_INTERNAL_BOUNDARY && input.len() - offset >= 64 {
            // SAFETY: The loop condition proves that 64 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm512_loadu_si512(input.as_ptr().add(offset).cast()) };
            let matches = _mm512_cmpeq_epi8_mask(values, cr) | _mm512_cmpeq_epi8_mask(values, lf);
            if matches != 0 {
                return offset + (matches.trailing_zeros() as usize);
            }
            offset += 64;
        }

        while input.len() - offset >= 64 {
            // SAFETY: The loop condition proves that 64 bytes remain in the
            // slice; the load itself permits any byte alignment.
            let values = unsafe { _mm512_loadu_si512(input.as_ptr().add(offset).cast()) };
            let matches = _mm512_cmpeq_epi8_mask(values, cr) | _mm512_cmpeq_epi8_mask(values, lf);
            if matches != 0 {
                return offset + (matches.trailing_zeros() as usize);
            }
            offset += 64;
        }

        offset + find_byte_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_cr_or_lf_avx128(input: &[u16]) -> usize {
        let cr = _mm_set1_epi16(b'\r' as i16);
        let lf = _mm_set1_epi16(b'\n' as i16);
        let mut offset = 0;

        while input.len() - offset >= 8 {
            // SAFETY: The loop condition proves that eight u16 values remain
            // in `input`; `_mm_loadu_si128` permits the slice address to be
            // unaligned.
            let values = unsafe { _mm_loadu_si128(input.as_ptr().add(offset).cast::<__m128i>()) };
            let matches = _mm_or_si128(_mm_cmpeq_epi16(values, cr), _mm_cmpeq_epi16(values, lf));
            let mask = _mm_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 8;
        }

        offset + find_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_markdown_special_avx128(input: &[u16]) -> usize {
        let mut offset = 0;

        while input.len() - offset >= 8 {
            // SAFETY: The loop condition proves that eight u16 values remain
            // in `input`; `_mm_loadu_si128` permits the slice address to be
            // unaligned.
            let values = unsafe { _mm_loadu_si128(input.as_ptr().add(offset).cast::<__m128i>()) };
            let mut matches = _mm_setzero_si128();
            for value in [b'\\', b'`', b'!', b'[', b'*', b'_', b'~', b'<', b'&', b'$'] {
                matches = _mm_or_si128(
                    matches,
                    _mm_cmpeq_epi16(values, _mm_set1_epi16(value as i16)),
                );
            }
            let mask = _mm_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 8;
        }

        offset + find_markdown_special_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_char_avx128(input: &[u16], target: u16) -> usize {
        let needle = _mm_set1_epi16(target as i16);
        let mut offset = 0;

        while input.len() - offset >= 8 {
            // SAFETY: The loop condition proves that eight u16 values remain
            // in `input`; `_mm_loadu_si128` permits the slice address to be
            // unaligned.
            let values = unsafe { _mm_loadu_si128(input.as_ptr().add(offset).cast::<__m128i>()) };
            let mask = _mm_movemask_epi8(_mm_cmpeq_epi16(values, needle)) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 8;
        }

        offset + find_char_scalar(&input[offset..], target)
    }

    #[inline]
    #[target_feature(enable = "avx2")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_cr_or_lf_avx2(input: &[u16]) -> usize {
        let cr = _mm256_set1_epi16(b'\r' as i16);
        let lf = _mm256_set1_epi16(b'\n' as i16);
        let mut offset = 0;

        while input.len() - offset >= 16 {
            // SAFETY: The loop condition proves that sixteen u16 values
            // remain in `input`; `_mm256_loadu_si256` permits an unaligned
            // slice address.
            let values =
                unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast::<__m256i>()) };
            let matches = _mm256_or_si256(
                _mm256_cmpeq_epi16(values, cr),
                _mm256_cmpeq_epi16(values, lf),
            );
            let mask = _mm256_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 16;
        }

        offset + find_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_markdown_special_avx2(input: &[u16]) -> usize {
        let mut offset = 0;

        while input.len() - offset >= 16 {
            // SAFETY: The loop condition proves that sixteen u16 values
            // remain in `input`; `_mm256_loadu_si256` permits an unaligned
            // slice address.
            let values =
                unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast::<__m256i>()) };
            let mut matches = _mm256_setzero_si256();
            for value in [b'\\', b'`', b'!', b'[', b'*', b'_', b'~', b'<', b'&', b'$'] {
                matches = _mm256_or_si256(
                    matches,
                    _mm256_cmpeq_epi16(values, _mm256_set1_epi16(value as i16)),
                );
            }
            let mask = _mm256_movemask_epi8(matches) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 16;
        }

        offset + find_markdown_special_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 with XMM/YMM state enabled. `input` must be a valid slice of
    /// initialized UTF-16 code units for its entire length. The unaligned
    /// loads are confined to in-bounds portions of that slice.
    pub unsafe fn find_char_avx2(input: &[u16], target: u16) -> usize {
        let needle = _mm256_set1_epi16(target as i16);
        let mut offset = 0;

        while input.len() - offset >= 16 {
            // SAFETY: The loop condition proves that sixteen u16 values
            // remain in `input`; `_mm256_loadu_si256` permits an unaligned
            // slice address.
            let values =
                unsafe { _mm256_loadu_si256(input.as_ptr().add(offset).cast::<__m256i>()) };
            let mask = _mm256_movemask_epi8(_mm256_cmpeq_epi16(values, needle)) as u32;
            if mask != 0 {
                return offset + (mask.trailing_zeros() as usize / 2);
            }
            offset += 16;
        }

        offset + find_char_scalar(&input[offset..], target)
    }

    #[inline]
    #[target_feature(enable = "avx2,avx512f,avx512bw")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 plus both AVX-512F and AVX-512BW with XMM/YMM, opmask, and ZMM
    /// state enabled.
    /// `input` must be a valid slice of initialized UTF-16 code units for its
    /// entire length. The unaligned loads are confined to in-bounds portions
    /// of that slice.
    pub unsafe fn find_cr_or_lf_avx512bw(input: &[u16]) -> usize {
        let cr = _mm512_set1_epi16(b'\r' as i16);
        let lf = _mm512_set1_epi16(b'\n' as i16);
        let mut offset = 0;

        while input.len() - offset >= 32 {
            // SAFETY: The loop condition proves that thirty-two u16 values
            // remain in `input`; `_mm512_loadu_si512` permits an unaligned
            // slice address.
            let values = unsafe { _mm512_loadu_si512(input.as_ptr().add(offset).cast()) };
            let matches = _mm512_cmpeq_epi16_mask(values, cr) | _mm512_cmpeq_epi16_mask(values, lf);
            if matches != 0 {
                return offset + (matches.trailing_zeros() as usize);
            }
            offset += 32;
        }

        offset + find_cr_or_lf_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2,avx512f,avx512bw")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 plus both AVX-512F and AVX-512BW with XMM/YMM, opmask, and ZMM
    /// state enabled.
    /// `input` must be a valid slice of initialized UTF-16 code units for its
    /// entire length. The unaligned loads are confined to in-bounds portions
    /// of that slice.
    pub unsafe fn find_markdown_special_avx512bw(input: &[u16]) -> usize {
        let mut offset = 0;

        while input.len() - offset >= 32 {
            // SAFETY: The loop condition proves that thirty-two u16 values
            // remain in `input`; `_mm512_loadu_si512` permits an unaligned
            // slice address.
            let values = unsafe { _mm512_loadu_si512(input.as_ptr().add(offset).cast()) };
            let mut matches = 0u32;
            for value in [b'\\', b'`', b'!', b'[', b'*', b'_', b'~', b'<', b'&', b'$'] {
                matches |= _mm512_cmpeq_epi16_mask(values, _mm512_set1_epi16(value as i16)) as u32;
            }
            if matches != 0 {
                return offset + (matches.trailing_zeros() as usize);
            }
            offset += 32;
        }

        offset + find_markdown_special_scalar(&input[offset..])
    }

    #[inline]
    #[target_feature(enable = "avx2,avx512f,avx512bw")]
    /// # Safety
    ///
    /// The caller must execute this function only when the CPU and OS expose
    /// AVX2 plus both AVX-512F and AVX-512BW with XMM/YMM, opmask, and ZMM
    /// state enabled.
    /// `input` must be a valid slice of initialized UTF-16 code units for its
    /// entire length. The unaligned loads are confined to in-bounds portions
    /// of that slice.
    pub unsafe fn find_char_avx512bw(input: &[u16], target: u16) -> usize {
        let needle = _mm512_set1_epi16(target as i16);
        let mut offset = 0;

        while input.len() - offset >= 32 {
            // SAFETY: The loop condition proves that thirty-two u16 values
            // remain in `input`; `_mm512_loadu_si512` permits an unaligned
            // slice address.
            let values = unsafe { _mm512_loadu_si512(input.as_ptr().add(offset).cast()) };
            let matches = _mm512_cmpeq_epi16_mask(values, needle);
            if matches != 0 {
                return offset + (matches.trailing_zeros() as usize);
            }
            offset += 32;
        }

        offset + find_char_scalar(&input[offset..], target)
    }

    #[allow(dead_code)]
    #[inline]
    pub fn markdown_special_is_used(value: u16) -> bool {
        is_markdown_special(value)
    }
}

#[inline]
/// Converts an FFI pointer/length pair after checking conditions that are
/// representable without dereferencing the pointer.
///
/// # Safety
///
/// For a non-zero `length` whose representational checks pass, the caller must
/// guarantee that `data` points to one allocation containing `length`
/// initialized `u16` values, that the allocation remains alive for the
/// returned borrow, and that no mutable access can occur for that duration.
/// This function checks null, alignment, byte-count, and address-overflow
/// conditions, but it cannot inspect whether an aligned, non-overflowing
/// address is mapped or whether its contents are initialized. A zero-length
/// span does not require an allocation and does not dereference `data`.
unsafe fn checked_span<'a>(data: *const u16, length: usize) -> Option<&'a [u16]> {
    if length == 0 {
        return Some(&[]);
    }

    let address = data as usize;
    if address == 0 || !address.is_multiple_of(mem::align_of::<u16>()) {
        return None;
    }

    let byte_length = length.checked_mul(U16_BYTES)?;
    if byte_length > isize::MAX as usize || address.checked_add(byte_length).is_none() {
        return None;
    }

    // SAFETY: The C++ ABI contract requires `data..data+length` to be one
    // initialized, immutable, aligned allocation. The checks above reject
    // representationally invalid spans before this conversion; ownership and
    // lifetime remain the caller's responsibility and no pointer is retained.
    Some(unsafe { slice::from_raw_parts(data, length) })
}

#[inline]
/// Converts a byte FFI pointer/length pair after checking conditions that are
/// representable without dereferencing the pointer.
///
/// # Safety
///
/// For a non-zero `length` whose representational checks pass, the caller must
/// guarantee that `data` points to one allocation containing `length`
/// initialized bytes, that the allocation remains alive for the returned
/// borrow, and that no mutable access can occur for that duration. A byte span
/// has no stricter input alignment than one byte, but null and overflowing
/// spans are still rejected. A zero-length span does not require an allocation.
unsafe fn checked_byte_span<'a>(data: *const u8, length: usize) -> Option<&'a [u8]> {
    if length == 0 {
        return Some(&[]);
    }

    let address = data as usize;
    if address == 0 || length > isize::MAX as usize || address.checked_add(length).is_none() {
        return None;
    }

    // SAFETY: The C++ ABI contract requires `data..data+length` to be one
    // initialized, immutable allocation. The checks above reject
    // representationally invalid spans before this conversion; ownership and
    // lifetime remain the caller's responsibility and no pointer is retained.
    Some(unsafe { slice::from_raw_parts(data, length) })
}

#[inline]
/// Applies one of the private SIMD adapters to an FFI span and clamps an
/// invalid or out-of-range result to the ABI sentinel.
///
/// # Safety
///
/// The caller must satisfy the allocation, initialization, lifetime, and
/// immutability requirements of [`checked_span`] whenever its representational
/// checks accept a non-zero span. `scan` must be a private adapter whose target
/// ISA is enabled by the caller's CPU/OS feature state and whose contract
/// accepts the resulting valid slice.
unsafe fn scan_ffi(data: *const u16, length: usize, scan: unsafe fn(&[u16]) -> usize) -> usize {
    // SAFETY: The caller of `scan_ffi` supplies the allocation/lifetime/
    // immutability contract documented above; this check then rejects all
    // representationally invalid spans before constructing the slice.
    let input = match unsafe { checked_span(data, length) } {
        Some(value) => value,
        None => return length,
    };

    // SAFETY: `scan` is one of the private SIMD entry points in this crate,
    // and its caller has supplied a valid Rust slice.
    let result = unsafe { scan(input) };
    if result <= length {
        result
    } else {
        length
    }
}

#[inline]
/// Applies one byte SIMD adapter to an FFI span and clamps an invalid or
/// out-of-range result to the byte-length sentinel.
///
/// # Safety
///
/// The caller must satisfy the allocation, initialization, lifetime, and
/// immutability requirements of [`checked_byte_span`] whenever its
/// representational checks accept a non-zero span. `scan` must be a private
/// adapter whose target ISA is enabled by the caller's CPU/OS feature state
/// and whose contract accepts the resulting valid slice.
unsafe fn scan_byte_ffi(data: *const u8, length: usize, scan: unsafe fn(&[u8]) -> usize) -> usize {
    // SAFETY: The caller supplies the allocation/lifetime/immutability
    // contract documented above; this check rejects invalid representations
    // before constructing the slice.
    let input = match unsafe { checked_byte_span(data, length) } {
        Some(value) => value,
        None => return length,
    };

    // SAFETY: `scan` is one of the private SIMD entry points in this crate,
    // and its caller has supplied a valid Rust slice.
    let result = unsafe { scan(input) };
    if result <= length {
        result
    } else {
        length
    }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX and OS XMM/YMM
/// state enabled. `input` must be a valid slice of initialized UTF-16 code
/// units; the delegated kernel reads only in-bounds units.
unsafe fn avx128_cr_or_lf(input: &[u16]) -> usize {
    // SAFETY: The caller selected the AVX-capable implementation and the
    // function only performs unaligned reads within `input`.
    unsafe { x86_64_kernels::find_cr_or_lf_avx128(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX and OS XMM/YMM
/// state enabled. `input` must be a valid initialized byte slice; the
/// delegated kernel reads only in-bounds bytes.
unsafe fn avx128_byte_cr_or_lf(input: &[u8]) -> usize {
    // SAFETY: The caller selected the AVX-capable implementation and the
    // function only performs unaligned reads within `input`.
    unsafe { x86_64_kernels::find_byte_cr_or_lf_avx128(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX and OS XMM/YMM
/// state enabled. `input` must be a valid slice of initialized UTF-16 code
/// units; the delegated kernel reads only in-bounds units.
unsafe fn avx128_markdown_special(input: &[u16]) -> usize {
    // SAFETY: See `avx128_cr_or_lf`.
    unsafe { x86_64_kernels::find_markdown_special_avx128(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX and OS XMM/YMM
/// state enabled. `input` must be a valid slice of initialized UTF-16 code
/// units; the delegated kernel reads only in-bounds units.
unsafe fn avx128_find_char(input: &[u16], target: u16) -> usize {
    // SAFETY: See `avx128_cr_or_lf`.
    unsafe { x86_64_kernels::find_char_avx128(input, target) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2 and OS
/// XMM/YMM state enabled. `input` must be a valid slice of initialized UTF-16
/// code units; the delegated kernel reads only in-bounds units.
unsafe fn avx2_cr_or_lf(input: &[u16]) -> usize {
    // SAFETY: The caller selected the AVX2-capable implementation.
    unsafe { x86_64_kernels::find_cr_or_lf_avx2(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2 and OS
/// XMM/YMM state enabled. `input` must be a valid initialized byte slice; the
/// delegated kernel reads only in-bounds bytes.
unsafe fn avx2_byte_cr_or_lf(input: &[u8]) -> usize {
    // SAFETY: The caller selected the AVX2-capable implementation.
    unsafe { x86_64_kernels::find_byte_cr_or_lf_avx2(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2 and OS
/// XMM/YMM state enabled. `input` must be a valid slice of initialized UTF-16
/// code units; the delegated kernel reads only in-bounds units.
unsafe fn avx2_markdown_special(input: &[u16]) -> usize {
    // SAFETY: See `avx2_cr_or_lf`.
    unsafe { x86_64_kernels::find_markdown_special_avx2(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2 and OS
/// XMM/YMM state enabled. `input` must be a valid slice of initialized UTF-16
/// code units; the delegated kernel reads only in-bounds units.
unsafe fn avx2_find_char(input: &[u16], target: u16) -> usize {
    // SAFETY: See `avx2_cr_or_lf`.
    unsafe { x86_64_kernels::find_char_avx2(input, target) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2, AVX-512F, and
/// AVX-512BW plus OS XMM/YMM, opmask, and ZMM state enabled. `input` must be a
/// valid slice of initialized UTF-16 code units; the delegated kernel reads
/// only in-bounds units.
unsafe fn avx512bw_cr_or_lf(input: &[u16]) -> usize {
    // SAFETY: The caller selected the AVX2/AVX-512F/BW-capable implementation.
    unsafe { x86_64_kernels::find_cr_or_lf_avx512bw(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2, AVX-512F,
/// and AVX-512BW plus OS XMM/YMM, opmask, and ZMM state enabled. `input` must
/// be a valid initialized byte slice; the delegated kernel reads only
/// in-bounds bytes.
unsafe fn avx512bw_byte_cr_or_lf(input: &[u8]) -> usize {
    // SAFETY: The caller selected the AVX2/AVX-512F/BW-capable implementation.
    unsafe { x86_64_kernels::find_byte_cr_or_lf_avx512bw(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2, AVX-512F, and
/// AVX-512BW plus OS XMM/YMM, opmask, and ZMM state enabled. `input` must be a
/// valid slice of initialized UTF-16 code units; the delegated kernel reads
/// only in-bounds units.
unsafe fn avx512bw_markdown_special(input: &[u16]) -> usize {
    // SAFETY: See `avx512bw_cr_or_lf`.
    unsafe { x86_64_kernels::find_markdown_special_avx512bw(input) }
}

#[cfg(target_arch = "x86_64")]
#[inline]
/// # Safety
///
/// The caller must have selected an execution context with AVX2, AVX-512F, and
/// AVX-512BW plus OS XMM/YMM, opmask, and ZMM state enabled. `input` must be a
/// valid slice of initialized UTF-16 code units; the delegated kernel reads
/// only in-bounds units.
unsafe fn avx512bw_find_char(input: &[u16], target: u16) -> usize {
    // SAFETY: See `avx512bw_cr_or_lf`.
    unsafe { x86_64_kernels::find_char_avx512bw(input, target) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
///
/// The caller must execute this function only when CPUID reports AVX and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized bytes; that
/// allocation must remain alive and immutable for the call. Rejected null,
/// size, or address-overflow representations return `length` without
/// dereference. The byte candidate accepts every input alignment.
pub unsafe fn sakura_byte_find_cr_or_lf_avx128_candidate_v1(
    data: *const u8,
    length: usize,
) -> usize {
    // SAFETY: This entry point's contract supplies the CPU/OS feature state
    // and caller-owned allocation contract required by `scan_byte_ffi`.
    unsafe { scan_byte_ffi(data, length, avx128_byte_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
///
/// The caller must execute this function only when CPUID reports AVX2 and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized bytes; that
/// allocation must remain alive and immutable for the call. Rejected null,
/// size, or address-overflow representations return `length` without
/// dereference. The byte candidate accepts every input alignment.
pub unsafe fn sakura_byte_find_cr_or_lf_avx2_candidate_v1(data: *const u8, length: usize) -> usize {
    // SAFETY: This entry point's contract supplies the CPU/OS feature state
    // and caller-owned allocation contract required by `scan_byte_ffi`.
    unsafe { scan_byte_ffi(data, length, avx2_byte_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
///
/// The caller must execute this function only when CPUID reports AVX2,
/// AVX-512F, and AVX-512BW, with OSXSAVE and XCR0 XMM/YMM plus opmask/ZMM
/// state enabled. For non-zero `length`, `data` must identify one allocation
/// containing `length` initialized bytes; that allocation must remain alive
/// and immutable for the call. Rejected null, size, or address-overflow
/// representations return `length` without dereference. The byte candidate
/// accepts every input alignment.
pub unsafe fn sakura_byte_find_cr_or_lf_avx512bw_candidate_v1(
    data: *const u8,
    length: usize,
) -> usize {
    // SAFETY: This entry point's contract supplies the CPU/OS feature state
    // and caller-owned allocation contract required by `scan_byte_ffi`.
    unsafe { scan_byte_ffi(data, length, avx512bw_byte_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_cr_or_lf_avx128_v1(data: *const u16, length: usize) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx128_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_markdown_special_avx128_v1(
    data: *const u16,
    length: usize,
) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx128_markdown_special) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference;
/// `target` is one UTF-16 code unit.
pub unsafe fn sakura_utf16_find_char_avx128_v1(
    data: *const u16,
    length: usize,
    target: u16,
) -> usize {
    // SAFETY: The entry point's Safety contract supplies the caller-owned
    // allocation, initialization, lifetime, and immutability guarantees that
    // representational checks cannot establish themselves.
    let input = match unsafe { checked_span(data, length) } {
        Some(value) => value,
        None => return length,
    };
    // SAFETY: `input` was produced by `checked_span`, and this private kernel
    // reads only within that slice.
    let result = unsafe { avx128_find_char(input, target) };
    if result <= length {
        result
    } else {
        length
    }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2 and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_cr_or_lf_avx2_v1(data: *const u16, length: usize) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx2_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2 and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_markdown_special_avx2_v1(data: *const u16, length: usize) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx2_markdown_special) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2 and
/// OSXSAVE with XCR0 XMM/YMM state enabled. For non-zero `length`, `data`
/// must identify one allocation containing `length` initialized `u16` values;
/// that allocation must remain alive and immutable for the call. This function
/// can reject null, misalignment, size, and address-overflow representations,
/// but it cannot prove allocation validity, initialization, lifetime, or
/// immutability. Rejected representations return `length` without dereference;
/// `target` is one UTF-16 code unit.
pub unsafe fn sakura_utf16_find_char_avx2_v1(
    data: *const u16,
    length: usize,
    target: u16,
) -> usize {
    // SAFETY: The entry point's Safety contract supplies the caller-owned
    // allocation, initialization, lifetime, and immutability guarantees that
    // representational checks cannot establish themselves.
    let input = match unsafe { checked_span(data, length) } {
        Some(value) => value,
        None => return length,
    };
    // SAFETY: See the corresponding AVX-128 entry point.
    let result = unsafe { avx2_find_char(input, target) };
    if result <= length {
        result
    } else {
        length
    }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2,
/// AVX-512F, and AVX-512BW, with OSXSAVE and XCR0 XMM/YMM plus opmask/ZMM
/// state enabled. AVX2 is part of the global AVX-512 dispatch-tier contract
/// because the C++ byte scanner's tail delegates to its AVX2 implementation.
/// For non-zero `length`, `data` must identify one allocation containing
/// `length` initialized `u16` values; that allocation must remain alive and
/// immutable for the call. This function can reject null, misalignment, size,
/// and address-overflow representations, but it cannot prove allocation
/// validity, initialization, lifetime, or immutability. Rejected
/// representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_cr_or_lf_avx512bw_v1(data: *const u16, length: usize) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx512bw_cr_or_lf) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2,
/// AVX-512F, and AVX-512BW, with OSXSAVE and XCR0 XMM/YMM plus opmask/ZMM
/// state enabled. AVX2 is part of the global AVX-512 dispatch-tier contract
/// because the C++ byte scanner's tail delegates to its AVX2 implementation.
/// For non-zero `length`, `data` must identify one allocation containing
/// `length` initialized `u16` values; that allocation must remain alive and
/// immutable for the call. This function can reject null, misalignment, size,
/// and address-overflow representations, but it cannot prove allocation
/// validity, initialization, lifetime, or immutability. Rejected
/// representations return `length` without dereference.
pub unsafe fn sakura_utf16_find_markdown_special_avx512bw_v1(
    data: *const u16,
    length: usize,
) -> usize {
    // SAFETY: This entry point's Safety contract supplies the CPU/OS feature
    // state and caller-owned allocation contract required by `scan_ffi`.
    unsafe { scan_ffi(data, length, avx512bw_markdown_special) }
}

#[cfg(target_arch = "x86_64")]
/// # Safety
/// The caller must execute this function only when CPUID reports AVX2,
/// AVX-512F, and AVX-512BW, with OSXSAVE and XCR0 XMM/YMM plus opmask/ZMM
/// state enabled. AVX2 is part of the global AVX-512 dispatch-tier contract
/// because the C++ byte scanner's tail delegates to its AVX2 implementation.
/// For non-zero `length`, `data` must identify one allocation containing
/// `length` initialized `u16` values; that allocation must remain alive and
/// immutable for the call. This function can reject null, misalignment, size,
/// and address-overflow representations, but it cannot prove allocation
/// validity, initialization, lifetime, or immutability. Rejected
/// representations return `length` without dereference; `target` is one
/// UTF-16 code unit.
pub unsafe fn sakura_utf16_find_char_avx512bw_v1(
    data: *const u16,
    length: usize,
    target: u16,
) -> usize {
    // SAFETY: The entry point's Safety contract supplies the caller-owned
    // allocation, initialization, lifetime, and immutability guarantees that
    // representational checks cannot establish themselves.
    let input = match unsafe { checked_span(data, length) } {
        Some(value) => value,
        None => return length,
    };
    // SAFETY: See the corresponding AVX-128 entry point.
    let result = unsafe { avx512bw_find_char(input, target) };
    if result <= length {
        result
    } else {
        length
    }
}

#[cfg(not(target_arch = "x86_64"))]
compile_error!("sakura-simd currently targets x86_64-pc-windows-msvc only");

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec;

    const EXTENDED_TEST_LENGTHS: &[usize] = &[255, 256, 257, 511, 512, 513, 4095, 4096, 4097];
    const TARGET_VALUES: &[u16] = &[
        0x0000, 0x0001, 0x007f, 0x0080, 0x00ff, 0xd7ff, 0xd800, 0xdbff, 0xdc00, 0xdfff, 0xe000,
        0xffff,
    ];
    const MARKDOWN_SPECIALS: &[u16] = &[
        b'\\' as u16,
        b'`' as u16,
        b'!' as u16,
        b'[' as u16,
        b'*' as u16,
        b'_' as u16,
        b'~' as u16,
        b'<' as u16,
        b'&' as u16,
        b'$' as u16,
    ];

    const ALIGNMENT_TEST_LENGTHS: &[usize] = &[
        0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511,
        512, 513, 4095, 4096, 4097,
    ];
    const RANDOM_LENGTHS: &[usize] = &[
        0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 192, 255, 256, 257,
        511, 512, 513, 1024, 2048, 3072,
    ];

    fn test_lengths() -> impl Iterator<Item = usize> {
        (0..=129).chain(EXTENDED_TEST_LENGTHS.iter().copied())
    }

    fn check_result(actual: usize, expected: usize, length: usize) {
        assert_eq!(actual, expected);
        assert!(actual <= length);
    }

    fn call_scan(
        scan: unsafe fn(*const u16, usize) -> usize,
        data: *const u16,
        length: usize,
    ) -> usize {
        // SAFETY: The tests intentionally exercise both valid and invalid
        // spans. The exported ABI functions are required to fail closed for
        // invalid representational spans and to read only valid spans.
        unsafe { scan(data, length) }
    }

    fn call_byte_scan(
        scan: unsafe fn(*const u8, usize) -> usize,
        data: *const u8,
        length: usize,
    ) -> usize {
        // SAFETY: The tests intentionally exercise both valid and invalid
        // spans. The byte candidate functions fail closed for invalid
        // representational spans and read only valid spans.
        unsafe { scan(data, length) }
    }

    fn call_find_char(
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
        data: *const u16,
        length: usize,
        target: u16,
    ) -> usize {
        // SAFETY: See `call_scan`; this wrapper centralizes the documented FFI
        // contract for the test matrix.
        unsafe { find_char(data, length, target) }
    }

    fn run_exhaustive_short_cases(
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
    ) {
        let mut input = vec![0x1234_u16; 129];
        for length in 0..=129 {
            let input = &mut input[..length];
            input.fill(0x1234);
            check_result(call_scan(cr_or_lf, input.as_ptr(), length), length, length);
            check_result(call_scan(markdown, input.as_ptr(), length), length, length);
            check_result(
                call_find_char(find_char, input.as_ptr(), length, b'?' as u16),
                length,
                length,
            );

            for &value in &[b'\r' as u16, b'\n' as u16] {
                for position in 0..length {
                    input.fill(0x1234);
                    input[position] = value;
                    check_result(
                        call_scan(cr_or_lf, input.as_ptr(), length),
                        position,
                        length,
                    );
                }
            }

            for &special in MARKDOWN_SPECIALS {
                for position in 0..length {
                    input.fill(0x1234);
                    input[position] = special;
                    check_result(
                        call_scan(markdown, input.as_ptr(), length),
                        position,
                        length,
                    );
                }
            }

            for &target in TARGET_VALUES {
                for position in 0..length {
                    input.fill(0x1234);
                    input[position] = target;
                    check_result(
                        call_find_char(find_char, input.as_ptr(), length, target),
                        position,
                        length,
                    );
                }
            }

            if length > 1 {
                input.fill(0x1234);
                input[0] = b'\r' as u16;
                input[length - 1] = b'\n' as u16;
                check_result(call_scan(cr_or_lf, input.as_ptr(), length), 0, length);

                input.fill(0x1234);
                input[0] = MARKDOWN_SPECIALS[0];
                input[length - 1] = MARKDOWN_SPECIALS[1];
                check_result(call_scan(markdown, input.as_ptr(), length), 0, length);

                input.fill(0x1234);
                input[0] = TARGET_VALUES[0];
                input[length - 1] = TARGET_VALUES[0];
                check_result(
                    call_find_char(find_char, input.as_ptr(), length, TARGET_VALUES[0]),
                    0,
                    length,
                );
            }
        }
    }

    fn run_alignment_cases(
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
    ) {
        for &length in ALIGNMENT_TEST_LENGTHS {
            for byte_offset in (0..64).step_by(2) {
                let mut storage = vec![0u16; length + 40];
                let pointer = {
                    // SAFETY: `byte_offset` is even and at most 62, so the
                    // pointer remains u16-aligned and the requested `length`
                    // units fit within the extra 40-unit allocation padding.
                    unsafe { (storage.as_mut_ptr() as *mut u8).add(byte_offset) as *mut u16 }
                };
                let input = {
                    // SAFETY: `pointer` addresses `length` initialized units
                    // within `storage`, and no other slice is used while this
                    // mutable slice is alive.
                    unsafe { slice::from_raw_parts_mut(pointer, length) }
                };
                input.fill(0x1234);
                check_result(call_scan(cr_or_lf, input.as_ptr(), length), length, length);
                check_result(call_scan(markdown, input.as_ptr(), length), length, length);
                check_result(
                    call_find_char(find_char, input.as_ptr(), length, 0xd800),
                    length,
                    length,
                );

                if length > 0 {
                    input.fill(0x1234);
                    input[0] = b'\r' as u16;
                    check_result(call_scan(cr_or_lf, input.as_ptr(), length), 0, length);
                    input.fill(0x1234);
                    input[length - 1] = b'\n' as u16;
                    check_result(
                        call_scan(cr_or_lf, input.as_ptr(), length),
                        length - 1,
                        length,
                    );

                    input.fill(0x1234);
                    input[0] = MARKDOWN_SPECIALS[0];
                    check_result(call_scan(markdown, input.as_ptr(), length), 0, length);
                    input.fill(0x1234);
                    input[length - 1] = MARKDOWN_SPECIALS[1];
                    check_result(
                        call_scan(markdown, input.as_ptr(), length),
                        length - 1,
                        length,
                    );

                    input.fill(0x1234);
                    input[0] = 0xd800;
                    check_result(
                        call_find_char(find_char, input.as_ptr(), length, 0xd800),
                        0,
                        length,
                    );
                    input.fill(0x1234);
                    input[length - 1] = 0xd800;
                    check_result(
                        call_find_char(find_char, input.as_ptr(), length, 0xd800),
                        length - 1,
                        length,
                    );
                }
            }
        }
    }

    fn run_extended_boundary_cases(
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
    ) {
        for &length in EXTENDED_TEST_LENGTHS {
            let mut input = vec![0x1234_u16; length];
            check_result(call_scan(cr_or_lf, input.as_ptr(), length), length, length);
            check_result(call_scan(markdown, input.as_ptr(), length), length, length);
            check_result(
                call_find_char(find_char, input.as_ptr(), length, 0xd800),
                length,
                length,
            );
            let positions = [0, 1.min(length - 1), length / 2, length - 2, length - 1];
            for &position in &positions {
                input.fill(0x1234);
                input[position] = b'\r' as u16;
                check_result(
                    call_scan(cr_or_lf, input.as_ptr(), length),
                    position,
                    length,
                );
                input.fill(0x1234);
                input[position] = b'\n' as u16;
                check_result(
                    call_scan(cr_or_lf, input.as_ptr(), length),
                    position,
                    length,
                );
                input.fill(0x1234);
                input[position] = MARKDOWN_SPECIALS[position % MARKDOWN_SPECIALS.len()];
                check_result(
                    call_scan(markdown, input.as_ptr(), length),
                    position,
                    length,
                );
                for &target in TARGET_VALUES {
                    input.fill(0x1234);
                    input[position] = target;
                    check_result(
                        call_find_char(find_char, input.as_ptr(), length, target),
                        position,
                        length,
                    );
                }
            }
            input.fill(0x1234);
            input[0] = MARKDOWN_SPECIALS[0];
            input[length - 1] = MARKDOWN_SPECIALS[1];
            check_result(call_scan(markdown, input.as_ptr(), length), 0, length);
        }
    }

    fn run_mixed_match_cases(
        vector_width: usize,
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
    ) {
        let length = 2 * vector_width + MARKDOWN_SPECIALS.len() + 2;
        let mut input = vec![0x1234_u16; length];

        // Mixed CR/LF values inside one vector choose the earlier code unit.
        input[2] = b'\n' as u16;
        input[vector_width - 2] = b'\r' as u16;
        check_result(call_scan(cr_or_lf, input.as_ptr(), length), 2, length);

        // The two kinds straddle the first vector boundary. Removing the
        // earlier one proves the match on the far side is still found.
        input.fill(0x1234);
        input[vector_width - 1] = b'\r' as u16;
        input[vector_width] = b'\n' as u16;
        check_result(
            call_scan(cr_or_lf, input.as_ptr(), length),
            vector_width - 1,
            length,
        );
        input[vector_width - 1] = 0x1234;
        check_result(
            call_scan(cr_or_lf, input.as_ptr(), length),
            vector_width,
            length,
        );

        // First and last code units use different CR/LF values.
        input.fill(0x1234);
        input[0] = b'\n' as u16;
        input[length - 1] = b'\r' as u16;
        check_result(call_scan(cr_or_lf, input.as_ptr(), length), 0, length);
        input[0] = 0x1234;
        check_result(
            call_scan(cr_or_lf, input.as_ptr(), length),
            length - 1,
            length,
        );

        // One input contains all ten Markdown special kinds and crosses a
        // vector boundary; the scanner must return the earliest kind.
        input.fill(0x1234);
        let markdown_start = vector_width - 1;
        for (offset, &special) in MARKDOWN_SPECIALS.iter().enumerate() {
            input[markdown_start + offset] = special;
        }
        check_result(
            call_scan(markdown, input.as_ptr(), length),
            markdown_start,
            length,
        );
        input[markdown_start] = 0x1234;
        check_result(
            call_scan(markdown, input.as_ptr(), length),
            markdown_start + 1,
            length,
        );
    }

    fn run_executable_isa_cases(
        name: &str,
        vector_width: usize,
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
    ) {
        std::println!("ISA_EXECUTION {name}=executed");
        run_exhaustive_short_cases(cr_or_lf, markdown, find_char);
        run_alignment_cases(cr_or_lf, markdown, find_char);
        run_extended_boundary_cases(cr_or_lf, markdown, find_char);
        run_mixed_match_cases(vector_width, cr_or_lf, markdown);
    }

    #[test]
    fn scalar_reference_exhaustive_lengths_positions_and_values() {
        let values = [
            b'\r' as u16,
            b'\n' as u16,
            b'\\' as u16,
            b'`' as u16,
            b'!' as u16,
            b'[' as u16,
            b'*' as u16,
            b'_' as u16,
            b'~' as u16,
            b'<' as u16,
            b'&' as u16,
            b'$' as u16,
            0,
            0xd800,
            0xdc00,
            0xffff,
            b'x' as u16,
        ];
        for length in test_lengths() {
            for &value in &values {
                let mut input = vec![0x1234_u16; length];
                assert_eq!(find_char_scalar(&input, value), length);
                for position in 0..length {
                    input[position] = value;
                    assert_eq!(find_char_scalar(&input, value), position);
                    input[position] = 0x1234_u16;
                }
            }
        }
    }

    #[test]
    fn byte_candidates_match_scalar_at_all_alignments_and_boundaries() {
        type ByteScan = unsafe fn(*const u8, usize) -> usize;
        let mut storage = vec![b'x'; 4097 + 64];
        let lengths = [
            0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513,
            1023, 4096,
        ];
        let implementations: &[(&str, bool, ByteScan)] = &[
            (
                "avx128",
                std::is_x86_feature_detected!("avx"),
                sakura_byte_find_cr_or_lf_avx128_candidate_v1,
            ),
            (
                "avx2",
                std::is_x86_feature_detected!("avx2"),
                sakura_byte_find_cr_or_lf_avx2_candidate_v1,
            ),
            (
                "avx512bw",
                std::is_x86_feature_detected!("avx2")
                    && std::is_x86_feature_detected!("avx512f")
                    && std::is_x86_feature_detected!("avx512bw"),
                sakura_byte_find_cr_or_lf_avx512bw_candidate_v1,
            ),
        ];

        for &(name, enabled, scan) in implementations {
            if !enabled {
                std::println!("BYTE_ISA_EXECUTION {name}=skipped");
                continue;
            }
            std::println!("BYTE_ISA_EXECUTION {name}=executed");
            for alignment in 0..64 {
                for &length in &lengths {
                    let input = &mut storage[alignment..alignment + length];
                    input.fill(b'x');
                    assert_eq!(
                        length,
                        call_byte_scan(scan, input.as_ptr(), length),
                        "name={name} alignment={alignment} length={length}",
                    );

                    for position in [
                        0,
                        length.saturating_sub(1),
                        15.min(length.saturating_sub(1)),
                        16.min(length.saturating_sub(1)),
                        31.min(length.saturating_sub(1)),
                        32.min(length.saturating_sub(1)),
                        63.min(length.saturating_sub(1)),
                        64.min(length.saturating_sub(1)),
                        511.min(length.saturating_sub(1)),
                        512.min(length.saturating_sub(1)),
                    ] {
                        if length == 0 {
                            break;
                        }
                        input.fill(b'x');
                        input[position] = if position & 1 == 0 { b'\r' } else { b'\n' };
                        assert_eq!(
                            position,
                            call_byte_scan(scan, input.as_ptr(), length),
                            "name={name} alignment={alignment} length={length} position={position}",
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn executable_isas_match_scalar_with_reviewed_matrix() {
        if std::is_x86_feature_detected!("avx") {
            run_executable_isa_cases(
                "avx128",
                8,
                sakura_utf16_find_cr_or_lf_avx128_v1,
                sakura_utf16_find_markdown_special_avx128_v1,
                sakura_utf16_find_char_avx128_v1,
            );
        } else {
            std::println!("ISA_EXECUTION avx128=skipped");
        }
        if std::is_x86_feature_detected!("avx2") {
            run_executable_isa_cases(
                "avx2",
                16,
                sakura_utf16_find_cr_or_lf_avx2_v1,
                sakura_utf16_find_markdown_special_avx2_v1,
                sakura_utf16_find_char_avx2_v1,
            );
        } else {
            std::println!("ISA_EXECUTION avx2=skipped");
        }
        if std::is_x86_feature_detected!("avx2")
            && std::is_x86_feature_detected!("avx512f")
            && std::is_x86_feature_detected!("avx512bw")
        {
            run_executable_isa_cases(
                "avx512bw",
                32,
                sakura_utf16_find_cr_or_lf_avx512bw_v1,
                sakura_utf16_find_markdown_special_avx512bw_v1,
                sakura_utf16_find_char_avx512bw_v1,
            );
        } else {
            std::println!("ISA_EXECUTION avx512bw=skipped");
        }
    }

    #[test]
    fn deterministic_random_differential_has_at_least_fifty_thousand_cases() {
        let mut state = 0x2395_16f1_u64;
        let mut executed = 0usize;
        let avx128_enabled = std::is_x86_feature_detected!("avx");
        let avx2_enabled = std::is_x86_feature_detected!("avx2");
        let avx512_global_tier_enabled = std::is_x86_feature_detected!("avx2")
            && std::is_x86_feature_detected!("avx512f")
            && std::is_x86_feature_detected!("avx512bw");

        let mut input_storage = vec![0u16; 4097];
        for case_index in 0..50_000 {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            let length = if case_index % 1024 < EXTENDED_TEST_LENGTHS.len() {
                EXTENDED_TEST_LENGTHS[case_index % EXTENDED_TEST_LENGTHS.len()]
            } else {
                RANDOM_LENGTHS[(state as usize) % RANDOM_LENGTHS.len()]
            };
            let input = &mut input_storage[..length];
            for value in input.iter_mut() {
                state = state.rotate_left(17).wrapping_add(0x9e3779b97f4a7c15);
                *value = state as u16;
            }
            state = state.rotate_left(23);
            let target = state as u16;
            let expected_char = find_char_scalar(input, target);
            let expected_cr = find_cr_or_lf_scalar(input);
            let expected_markdown = find_markdown_special_scalar(input);
            if avx128_enabled {
                check_result(
                    call_find_char(
                        sakura_utf16_find_char_avx128_v1,
                        input.as_ptr(),
                        length,
                        target,
                    ),
                    expected_char,
                    length,
                );
                check_result(
                    call_scan(sakura_utf16_find_cr_or_lf_avx128_v1, input.as_ptr(), length),
                    expected_cr,
                    length,
                );
                check_result(
                    call_scan(
                        sakura_utf16_find_markdown_special_avx128_v1,
                        input.as_ptr(),
                        length,
                    ),
                    expected_markdown,
                    length,
                );
            }
            if avx2_enabled {
                check_result(
                    call_find_char(
                        sakura_utf16_find_char_avx2_v1,
                        input.as_ptr(),
                        length,
                        target,
                    ),
                    expected_char,
                    length,
                );
                check_result(
                    call_scan(sakura_utf16_find_cr_or_lf_avx2_v1, input.as_ptr(), length),
                    expected_cr,
                    length,
                );
                check_result(
                    call_scan(
                        sakura_utf16_find_markdown_special_avx2_v1,
                        input.as_ptr(),
                        length,
                    ),
                    expected_markdown,
                    length,
                );
            }
            if avx512_global_tier_enabled {
                check_result(
                    call_find_char(
                        sakura_utf16_find_char_avx512bw_v1,
                        input.as_ptr(),
                        length,
                        target,
                    ),
                    expected_char,
                    length,
                );
                check_result(
                    call_scan(
                        sakura_utf16_find_cr_or_lf_avx512bw_v1,
                        input.as_ptr(),
                        length,
                    ),
                    expected_cr,
                    length,
                );
                check_result(
                    call_scan(
                        sakura_utf16_find_markdown_special_avx512bw_v1,
                        input.as_ptr(),
                        length,
                    ),
                    expected_markdown,
                    length,
                );
            }
            executed += 1;
        }

        assert!(executed >= 50_000);
        std::println!(
            "RUST_RANDOM_DIFFERENTIAL cases={executed} avx128={} avx2={} avx512bw={} avx512bw_requires_avx2=true",
            if avx128_enabled {
                "executed"
            } else {
                "skipped"
            },
            if avx2_enabled { "executed" } else { "skipped" },
            if avx512_global_tier_enabled {
                "executed"
            } else {
                "skipped"
            },
        );
    }

    fn run_invalid_spans_for_isa(
        name: &str,
        cr_or_lf: unsafe fn(*const u16, usize) -> usize,
        markdown: unsafe fn(*const u16, usize) -> usize,
        find_char: unsafe fn(*const u16, usize, u16) -> usize,
    ) {
        std::println!("FFI_ISA_EXECUTION {name}=executed");
        let aligned_storage = [0u16; 2];
        let misaligned = (aligned_storage.as_ptr() as *const u8)
            .wrapping_add(1)
            .cast::<u16>();
        let aligned_dummy = [0u16; 1];
        let oversized = (isize::MAX as usize / U16_BYTES) + 1;

        for scan in [cr_or_lf, markdown] {
            assert_eq!(call_scan(scan, core::ptr::null(), 0), 0);
            assert_eq!(call_scan(scan, core::ptr::dangling(), 0), 0);
            assert_eq!(call_scan(scan, core::ptr::null(), 1), 1);
            assert_eq!(call_scan(scan, misaligned, 1), 1);
            assert_eq!(call_scan(scan, usize::MAX as *const u16, 1), 1);
            assert_eq!(call_scan(scan, (usize::MAX - 1) as *const u16, 2), 2);
            assert_eq!(call_scan(scan, core::ptr::null(), usize::MAX), usize::MAX);
            assert_eq!(
                call_scan(scan, aligned_dummy.as_ptr(), usize::MAX),
                usize::MAX
            );
            assert_eq!(
                call_scan(scan, aligned_dummy.as_ptr(), oversized),
                oversized
            );
        }
        for target in [0u16, 0xd800] {
            assert_eq!(call_find_char(find_char, core::ptr::null(), 0, target), 0);
            assert_eq!(
                call_find_char(find_char, core::ptr::dangling(), 0, target),
                0
            );
            assert_eq!(call_find_char(find_char, core::ptr::null(), 1, target), 1);
            assert_eq!(call_find_char(find_char, misaligned, 1, target), 1);
            assert_eq!(
                call_find_char(find_char, usize::MAX as *const u16, 1, target),
                1
            );
            assert_eq!(
                call_find_char(find_char, (usize::MAX - 1) as *const u16, 2, target),
                2
            );
            assert_eq!(
                call_find_char(find_char, core::ptr::null(), usize::MAX, target),
                usize::MAX
            );
            assert_eq!(
                call_find_char(find_char, aligned_dummy.as_ptr(), usize::MAX, target),
                usize::MAX
            );
            assert_eq!(
                call_find_char(find_char, aligned_dummy.as_ptr(), oversized, target),
                oversized
            );
        }

        let mut input = [b'x' as u16, b'y' as u16];
        assert_eq!(
            call_scan(cr_or_lf, input.as_ptr(), input.len()),
            input.len()
        );
        assert_eq!(
            call_scan(markdown, input.as_ptr(), input.len()),
            input.len()
        );
        assert_eq!(
            call_find_char(find_char, input.as_ptr(), input.len(), b'?' as u16),
            input.len()
        );
        input[1] = b'\r' as u16;
        assert_eq!(call_scan(cr_or_lf, input.as_ptr(), input.len()), 1);
        input[1] = MARKDOWN_SPECIALS[0];
        assert_eq!(call_scan(markdown, input.as_ptr(), input.len()), 1);
        input[1] = 0xd800;
        assert_eq!(
            call_find_char(find_char, input.as_ptr(), input.len(), 0xd800),
            1
        );
    }

    #[test]
    fn ffi_invalid_spans_fail_closed_before_dereference() {
        if std::is_x86_feature_detected!("avx") {
            run_invalid_spans_for_isa(
                "avx128",
                sakura_utf16_find_cr_or_lf_avx128_v1,
                sakura_utf16_find_markdown_special_avx128_v1,
                sakura_utf16_find_char_avx128_v1,
            );
        } else {
            std::println!("FFI_ISA_EXECUTION avx128=skipped");
        }
        if std::is_x86_feature_detected!("avx2") {
            run_invalid_spans_for_isa(
                "avx2",
                sakura_utf16_find_cr_or_lf_avx2_v1,
                sakura_utf16_find_markdown_special_avx2_v1,
                sakura_utf16_find_char_avx2_v1,
            );
        } else {
            std::println!("FFI_ISA_EXECUTION avx2=skipped");
        }
        if std::is_x86_feature_detected!("avx2")
            && std::is_x86_feature_detected!("avx512f")
            && std::is_x86_feature_detected!("avx512bw")
        {
            run_invalid_spans_for_isa(
                "avx512bw",
                sakura_utf16_find_cr_or_lf_avx512bw_v1,
                sakura_utf16_find_markdown_special_avx512bw_v1,
                sakura_utf16_find_char_avx512bw_v1,
            );
        } else {
            std::println!("FFI_ISA_EXECUTION avx512bw=skipped");
        }
    }
}
