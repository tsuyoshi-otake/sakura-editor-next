//! Conventions shared by every document-codec export of this crate (#441, #443).
//!
//! The encoded-EOL scan and the UTF-8/CESU-8 conversion are two capabilities of one C ABI family. They answer with
//! one status space, speak one ABI version, apply one rule to the header of every descriptor and contain panics the
//! same way, so those live here once and a new capability cannot drift from the others. What a capability checks
//! beyond them (its own fields, ranges, ceilings and publication) stays in its module. Nothing here allocates or
//! retains a caller pointer.

use std::mem::size_of;
use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::ffi_pointer::{checked_address_range, AddressRange};

pub const DOCUMENT_CODEC_ABI_VERSION_V1: u32 = 1;

/// Typed status of the document-codec exports. The numbers share the candidate status space of the URI candidate.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraDocumentCodecStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidCapacity = 2,
    InternalError = 12,
}

use SakuraDocumentCodecStatus::{InternalError, InvalidArgument};

/// Runs the body of one export and reports its outcome as a status. A panic is contained here and becomes
/// `InternalError`; no panic crosses the C ABI.
pub(crate) fn catch_status<F>(operation: F) -> SakuraDocumentCodecStatus
where
    F: FnOnce() -> Result<(), SakuraDocumentCodecStatus>,
{
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(())) => SakuraDocumentCodecStatus::Ok,
        Ok(Err(status)) => status,
        Err(_) => InternalError,
    }
}

/// Copies one caller descriptor and returns the address range it occupies, or `InvalidArgument` when the pointer is
/// null, misaligned or would run past the end of the address space.
pub(crate) fn read_descriptor<T: Copy>(
    pointer: *const T,
) -> Result<(T, AddressRange), SakuraDocumentCodecStatus> {
    let range = checked_address_range(pointer).ok_or(InvalidArgument)?;
    // SAFETY: The address range was checked. The ABI requires that descriptors
    // at accepted addresses are fully initialized and readable during the call.
    Ok((unsafe { pointer.read() }, range))
}

/// The header rule of every descriptor: the size of `T`, ABI version 1 and zeroed reserved fields. Only the caller
/// knows how its descriptor lays out the reserved fields, so it passes whether they are all zero.
pub(crate) fn validate_header<T>(
    struct_size: u32,
    abi_version: u32,
    reserved_is_zero: bool,
) -> Result<(), SakuraDocumentCodecStatus> {
    if u32::try_from(size_of::<T>()) != Ok(struct_size)
        || abi_version != DOCUMENT_CODEC_ABI_VERSION_V1
        || !reserved_is_zero
    {
        return Err(InvalidArgument);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use SakuraDocumentCodecStatus::InvalidCapacity;

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct Header {
        struct_size: u32,
        abi_version: u32,
    }

    #[test]
    fn the_wire_numbers_are_pinned() {
        assert_eq!(SakuraDocumentCodecStatus::Ok as u32, 0);
        assert_eq!(InvalidArgument as u32, 1);
        assert_eq!(InvalidCapacity as u32, 2);
        assert_eq!(InternalError as u32, 12);
        assert_eq!(DOCUMENT_CODEC_ABI_VERSION_V1, 1);
    }

    #[test]
    fn a_body_outcome_becomes_its_status() {
        assert_eq!(catch_status(|| Ok(())), SakuraDocumentCodecStatus::Ok);
        for status in [InvalidArgument, InvalidCapacity, InternalError] {
            assert_eq!(catch_status(|| Err(status)), status);
        }
    }

    #[test]
    fn a_panic_is_contained_as_an_internal_error() {
        assert_eq!(catch_status(|| panic!("contained")), InternalError);
        // The next call is unaffected.
        assert_eq!(catch_status(|| Ok(())), SakuraDocumentCodecStatus::Ok);
    }

    #[test]
    fn a_descriptor_is_copied_with_its_range_and_bad_addresses_are_refused() {
        let header = Header {
            struct_size: 8,
            abi_version: 1,
        };
        let (copy, range) = read_descriptor(&raw const header).expect("a live descriptor is read");
        assert_eq!((copy.struct_size, copy.abi_version), (8, 1));
        assert_eq!(range.start, (&raw const header) as usize);
        assert_eq!(range.end, range.start + size_of::<Header>());

        assert_eq!(
            read_descriptor(std::ptr::null::<Header>()).err(),
            Some(InvalidArgument)
        );
        let misaligned = std::ptr::without_provenance::<Header>(1);
        assert_eq!(read_descriptor(misaligned).err(), Some(InvalidArgument));
    }

    #[test]
    fn the_header_rule_checks_size_version_and_reserved_fields() {
        let size = u32::try_from(size_of::<Header>()).unwrap();
        assert_eq!(validate_header::<Header>(size, 1, true), Ok(()));
        for (struct_size, abi_version, reserved_is_zero) in [
            (size + 1, 1, true),
            (size - 1, 1, true),
            (0, 1, true),
            (size, 0, true),
            (size, 2, true),
            (size, 1, false),
        ] {
            assert_eq!(
                validate_header::<Header>(struct_size, abi_version, reserved_is_zero),
                Err(InvalidArgument),
                "{struct_size} {abi_version} {reserved_is_zero}"
            );
        }
    }

    #[test]
    fn panic_containment_is_defined_exactly_once() {
        let source = include_str!("document_codec_abi.rs");
        let (production, _) = source.split_once("mod tests {").unwrap();
        assert_eq!(production.matches("catch_unwind(").count(), 1);
        assert!(!production.contains("unwrap("));
        assert!(!production.contains("expect("));
    }
}
