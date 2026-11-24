//! Shared validation for caller-owned pointers that cross the C ABI.
//!
//! Every export in this crate has the same obligation before it writes through
//! a caller pointer: reject null, reject a misaligned address, and reject a
//! range that would run past the end of the address space.  The rule is one
//! rule, so it lives in one place rather than being restated by each export
//! family. The address-range and overlap helpers that exports with several
//! caller buffers share live here for the same reason.

use std::mem::{align_of, size_of};

/// Reports whether `pointer` satisfies the alignment `T` requires.
pub(crate) fn is_aligned<T>(pointer: *const T) -> bool {
    (pointer as usize).is_multiple_of(align_of::<T>())
}

/// Reports whether one complete `T` may be read or written at `pointer`.
///
/// This is an address-range test only.  It proves that the caller did not hand
/// us a null or misaligned slot and that `pointer + size_of::<T>()` does not
/// wrap; it cannot prove the caller owns the storage, which stays the caller's
/// documented obligation on each export.
pub(crate) fn is_valid_pointer<T>(pointer: *const T) -> bool {
    if pointer.is_null() || !is_aligned(pointer) {
        return false;
    }
    (pointer as usize)
        .checked_add(size_of::<T>())
        .is_some_and(|end| end <= isize::MAX as usize)
}

/// A half-open range of caller addresses, `[start, end)`.
#[derive(Clone, Copy)]
pub(crate) struct AddressRange {
    pub(crate) start: usize,
    pub(crate) end: usize,
}

/// Returns the address range of one complete `T` at `pointer`, or `None` when
/// the pointer is null, misaligned, or the slot would run past the end of the
/// address space.
pub(crate) fn checked_address_range<T>(pointer: *const T) -> Option<AddressRange> {
    if pointer.is_null() || !is_aligned(pointer) {
        return None;
    }
    let start = pointer as usize;
    let end = start.checked_add(size_of::<T>())?;
    (end <= isize::MAX as usize).then_some(AddressRange { start, end })
}

/// Returns the address range of `length` consecutive `T` at `pointer`.
///
/// A zero length is always valid and yields an empty range, whatever the
/// pointer is. Otherwise the pointer must be non-null and aligned, and both
/// the byte length and the end address must stay within `isize::MAX`.
pub(crate) fn checked_slice_range<T>(pointer: *const T, length: usize) -> Option<AddressRange> {
    if length == 0 {
        return Some(AddressRange { start: 0, end: 0 });
    }
    if pointer.is_null() || !is_aligned(pointer) {
        return None;
    }
    let byte_length = length.checked_mul(size_of::<T>())?;
    if byte_length > isize::MAX as usize {
        return None;
    }
    let start = pointer as usize;
    let end = start.checked_add(byte_length)?;
    (end <= isize::MAX as usize).then_some(AddressRange { start, end })
}

/// Reports whether two non-empty ranges share at least one byte.
pub(crate) fn overlaps(left: AddressRange, right: AddressRange) -> bool {
    left.start != left.end
        && right.start != right.end
        && left.start < right.end
        && right.start < left.end
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Storage whose base address is forced to `u64` alignment, so offsetting
    /// it by one byte is misaligned by construction rather than by luck.
    #[repr(align(8))]
    struct AlignedBytes([u8; size_of::<u64>() + 1]);

    #[test]
    fn null_is_never_valid() {
        assert!(!is_valid_pointer(std::ptr::null::<u64>()));
    }

    #[test]
    fn misaligned_is_never_valid() {
        let mut storage = AlignedBytes([0; size_of::<u64>() + 1]);
        let base = storage.0.as_mut_ptr();
        // SAFETY: The offset stays inside `storage`, and the result is only
        // used as an address, never dereferenced.
        let misaligned = unsafe { base.add(1) }.cast::<u64>();
        assert!(!is_aligned(misaligned), "the test premise itself failed");
        assert!(!is_valid_pointer(misaligned));
    }

    #[test]
    fn aligned_caller_storage_is_valid() {
        let mut storage = 0_u64;
        let slot = std::ptr::addr_of_mut!(storage).cast_const();
        assert!(is_aligned(slot), "the test premise itself failed");
        assert!(is_valid_pointer(slot));
    }

    #[test]
    fn an_aligned_range_that_would_wrap_is_never_valid() {
        let last = ((isize::MAX as usize) - (size_of::<u64>() - 1)) as *const u64;
        assert!(is_aligned(last), "the test premise itself failed");
        assert!(!is_valid_pointer(last));
    }

    #[test]
    fn address_range_of_a_slot_spans_exactly_one_value() {
        let mut storage = 0_u64;
        let slot = std::ptr::addr_of_mut!(storage).cast_const();
        let range = checked_address_range(slot).expect("an aligned slot is accepted");
        assert_eq!(range.start, slot as usize);
        assert_eq!(range.end, slot as usize + size_of::<u64>());
    }

    #[test]
    fn address_range_rejects_null_misaligned_and_wrapping_slots() {
        assert!(checked_address_range(std::ptr::null::<u64>()).is_none());
        let mut storage = AlignedBytes([0; size_of::<u64>() + 1]);
        let base = storage.0.as_mut_ptr();
        // SAFETY: The offset stays inside `storage`, and the result is only
        // used as an address, never dereferenced.
        let misaligned = unsafe { base.add(1) }.cast::<u64>().cast_const();
        assert!(checked_address_range(misaligned).is_none());
        let last = ((isize::MAX as usize) - (size_of::<u64>() - 1)) as *const u64;
        assert!(checked_address_range(last).is_none());
    }

    #[test]
    fn slice_range_of_zero_length_is_empty_at_any_address() {
        for pointer in [
            std::ptr::null::<u64>(),
            std::ptr::without_provenance::<u64>(3),
        ] {
            let range = checked_slice_range(pointer, 0).expect("an empty slice is accepted");
            assert_eq!((range.start, range.end), (0, 0));
        }
    }

    #[test]
    fn slice_range_spans_exactly_length_values() {
        let storage = [0_u64; 4];
        let range = checked_slice_range(storage.as_ptr(), 4).expect("a real slice is accepted");
        assert_eq!(range.start, storage.as_ptr() as usize);
        assert_eq!(range.end, storage.as_ptr() as usize + 4 * size_of::<u64>());
    }

    #[test]
    fn slice_range_rejects_null_misaligned_and_oversized_slices() {
        assert!(checked_slice_range(std::ptr::null::<u64>(), 1).is_none());

        let mut bytes = AlignedBytes([0; size_of::<u64>() + 1]);
        let base = bytes.0.as_mut_ptr();
        // SAFETY: The offset stays inside `bytes`, and the result is only
        // used as an address, never dereferenced.
        let misaligned = unsafe { base.add(1) }.cast::<u64>().cast_const();
        assert!(checked_slice_range(misaligned, 1).is_none());

        let storage = [0_u64; 2];
        let aligned = storage.as_ptr();
        // The byte length overflows `usize`.
        assert!(checked_slice_range(aligned, usize::MAX).is_none());
        // The byte length alone is above `isize::MAX`.
        assert!(checked_slice_range(aligned, isize::MAX as usize / size_of::<u64>() + 1).is_none());
        // The end address would be above `isize::MAX`.
        let near_end = ((isize::MAX as usize) - (size_of::<u64>() - 1)) as *const u64;
        assert!(checked_slice_range(near_end, 1).is_none());
    }

    #[test]
    fn overlap_needs_a_shared_byte_between_two_non_empty_ranges() {
        let range = |start, end| AddressRange { start, end };
        assert!(overlaps(range(0, 5), range(4, 8)));
        assert!(overlaps(range(4, 8), range(0, 5)));
        assert!(overlaps(range(0, 8), range(2, 3)));
        assert!(!overlaps(range(0, 4), range(4, 8)));
        assert!(!overlaps(range(4, 8), range(0, 4)));
        assert!(!overlaps(range(0, 0), range(0, 8)));
        assert!(!overlaps(range(0, 8), range(3, 3)));
    }

    #[test]
    fn address_range_accepts_a_slot_that_ends_exactly_at_isize_max() {
        // `isize::MAX` is odd, so only a value with alignment 1 can end there.
        let start = isize::MAX as usize - 1;
        let range = checked_address_range(std::ptr::without_provenance::<u8>(start))
            .expect("a slot ending at isize::MAX is accepted");
        assert_eq!((range.start, range.end), (start, isize::MAX as usize));
        let one_past = std::ptr::without_provenance::<u8>(isize::MAX as usize);
        assert!(checked_address_range(one_past).is_none());
    }

    #[test]
    fn address_range_rejects_a_slot_whose_end_address_wraps_around() {
        let last_slot = std::ptr::without_provenance::<u64>(usize::MAX - (size_of::<u64>() - 1));
        assert!(is_aligned(last_slot), "the test premise itself failed");
        assert!(checked_address_range(last_slot).is_none());
    }

    #[test]
    fn slice_range_accepts_a_slice_that_ends_exactly_at_isize_max() {
        let start = isize::MAX as usize - 3;
        let range = checked_slice_range(std::ptr::without_provenance::<u8>(start), 3)
            .expect("a slice ending at isize::MAX is accepted");
        assert_eq!((range.start, range.end), (start, isize::MAX as usize));
        assert!(checked_slice_range(std::ptr::without_provenance::<u8>(start), 4).is_none());
    }

    #[test]
    fn slice_range_rejects_a_byte_length_that_wraps_around() {
        let storage = [0_u64; 2];
        // Eight bytes per value: this product exceeds `usize::MAX`, and a
        // wrapping multiply would turn it into a small, plausible length.
        let length = usize::MAX / size_of::<u64>() + 2;
        assert!(checked_slice_range(storage.as_ptr(), length).is_none());
    }

    #[test]
    fn slice_range_rejects_an_end_address_that_wraps_around() {
        // The byte length is small; only `start + length` overflows.
        let near_top = std::ptr::without_provenance::<u8>(usize::MAX - 1);
        assert!(checked_slice_range(near_top, 4).is_none());
    }

    #[test]
    fn overlap_ignores_an_empty_range_inside_a_non_empty_one() {
        let range = |start, end| AddressRange { start, end };
        assert!(!overlaps(range(3, 3), range(0, 8)));
        assert!(!overlaps(range(0, 8), range(3, 3)));
    }
}
