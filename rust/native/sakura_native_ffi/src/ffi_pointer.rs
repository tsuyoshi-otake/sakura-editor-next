//! Shared validation for caller-owned pointers that cross the C ABI.
//!
//! Every export in this crate has the same obligation before it writes through
//! a caller pointer: reject null, reject a misaligned address, and reject a
//! range that would run past the end of the address space.  The rule is one
//! rule, so it lives in one place rather than being restated by each export
//! family.

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
}
