//! Sole-authority OutputService provider exports.
//!
//! Requests and snapshots cross the ABI as bounded copied spans.  The Rust
//! model retains only owned values.  Provider tokens use a separate tagged
//! registry from the migration shadow so the two opaque-handle families can
//! never be confused.

#![allow(dead_code)]

use std::collections::BTreeMap;
use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, MutexGuard, OnceLock};

use crate::output_shadow;

pub type SakuraOutputProviderStatus = output_shadow::SakuraOutputShadowStatus;
pub type SakuraOutputProviderOperationStatus = output_shadow::SakuraOutputShadowOperationStatus;
pub type SakuraOutputProviderReason = output_shadow::SakuraOutputShadowReason;
pub type SakuraOutputProviderSpanV1 = output_shadow::SakuraOutputShadowSpanV1;
pub type SakuraOutputProviderLimitsV1 = output_shadow::SakuraOutputShadowLimitsV1;
pub type SakuraOutputProviderLogEntryV1 = output_shadow::SakuraOutputShadowLogEntryV1;
pub type SakuraOutputProviderRequestV1 = output_shadow::SakuraOutputShadowRequestV1;
pub type SakuraOutputProviderApplyResultV1 = output_shadow::SakuraOutputShadowApplyResultV1;
pub type SakuraOutputProviderSnapshotInfoV1 = output_shadow::SakuraOutputShadowSnapshotInfoV1;
pub type SakuraOutputProviderSnapshotBufferV1 = output_shadow::SakuraOutputShadowSnapshotBufferV1;
pub type SakuraOutputProviderActiveChannelV1 = output_shadow::SakuraOutputShadowActiveChannelV1;

const PROVIDER_TOKEN_TAG: u64 = 1_u64 << 63;

struct ProviderRegistry {
    next_token: u64,
    providers: BTreeMap<u64, u64>,
}

static PROVIDERS: OnceLock<Mutex<ProviderRegistry>> = OnceLock::new();

fn providers() -> &'static Mutex<ProviderRegistry> {
    PROVIDERS.get_or_init(|| {
        Mutex::new(ProviderRegistry {
            next_token: PROVIDER_TOKEN_TAG | 1,
            providers: BTreeMap::new(),
        })
    })
}

fn lock_providers() -> MutexGuard<'static, ProviderRegistry> {
    providers()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn is_aligned<T>(pointer: *const T) -> bool {
    (pointer as usize).is_multiple_of(align_of::<T>())
}

fn is_valid_pointer<T>(pointer: *const T) -> bool {
    !pointer.is_null()
        && is_aligned(pointer)
        && (pointer as usize)
            .checked_add(size_of::<T>())
            .is_some_and(|end| end <= isize::MAX as usize)
}

fn poison_result() -> SakuraOutputProviderApplyResultV1 {
    SakuraOutputProviderApplyResultV1 {
        struct_size: size_of::<SakuraOutputProviderApplyResultV1>() as u32,
        abi_version: 1,
        status: SakuraOutputProviderOperationStatus::RevisionExhausted as u32,
        reason: SakuraOutputProviderReason::InvalidPayload as u32,
        revision: u64::MAX,
        callback_drain_deferred: 0,
        reserved: [0; 7],
    }
}

fn poison_info() -> SakuraOutputProviderSnapshotInfoV1 {
    SakuraOutputProviderSnapshotInfoV1 {
        struct_size: size_of::<SakuraOutputProviderSnapshotInfoV1>() as u32,
        abi_version: 1,
        revision: u64::MAX,
        stopped: 0xff,
        active_channel_present: 0xff,
        reserved0: [0; 6],
        dropped_notification_count: u64::MAX,
        channel_count: u64::MAX,
        encoded_size: u64::MAX,
        reserved: [0; 2],
    }
}

fn poison_active() -> SakuraOutputProviderActiveChannelV1 {
    SakuraOutputProviderActiveChannelV1 {
        struct_size: size_of::<SakuraOutputProviderActiveChannelV1>() as u32,
        abi_version: 1,
        revision: u64::MAX,
        present: 0xff,
        reserved0: [0; 7],
        data: std::ptr::null_mut(),
        capacity: u64::MAX,
        length: u64::MAX,
        reserved: [0; 2],
    }
}

/// Creates one callback-free Rust OutputService provider.
///
/// # Safety
///
/// The limits and output token are borrowed only for this call.  All limits
/// are copied and no caller pointer is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_create_v1(
    limits: *const SakuraOutputProviderLimitsV1,
    token: *mut u64,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The token slot is validated and caller-owned for this call.
        unsafe { token.write(0) };
        let mut shadow_token = 0_u64;
        // SAFETY: The provider-neutral model entrypoint strictly validates
        // and copies the limits.
        let status = unsafe { output_shadow::model_create_v1(limits.cast(), &mut shadow_token) };
        if status != SakuraOutputProviderStatus::Ok || shadow_token == 0 {
            return if status == SakuraOutputProviderStatus::Ok {
                SakuraOutputProviderStatus::InternalError
            } else {
                status
            };
        }
        let mut registry = lock_providers();
        let provider_token = registry.next_token;
        if provider_token == 0 || provider_token & PROVIDER_TOKEN_TAG == 0 {
            drop(registry);
            // SAFETY: `shadow_token` was returned by the successful create
            // call above and is held in local writable storage.
            let _ = unsafe { output_shadow::model_destroy_v1(&mut shadow_token) };
            return SakuraOutputProviderStatus::InternalError;
        }
        registry.next_token = provider_token.checked_add(1).unwrap_or(0);
        registry.providers.insert(provider_token, shadow_token);
        // SAFETY: The token slot remains caller-owned for this call.
        unsafe { token.write(provider_token) };
        SakuraOutputProviderStatus::Ok
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Applies one copied mutation to the Rust provider.
///
/// # Safety
///
/// `request` and `result`, including nested spans and log arrays, follow the
/// V1 copied input/output contract.  No caller address is retained.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_apply_v1(
    token: u64,
    request: *const SakuraOutputProviderRequestV1,
    result: *mut SakuraOutputProviderApplyResultV1,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(result.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The result slot is validated and caller-owned for this call.
        unsafe { result.write(poison_result()) };
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export validates all request/span/range fields
        // and copies every input before changing the Rust-owned model.
        unsafe { output_shadow::model_apply_v1(shadow_token, request.cast(), result.cast()) }
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Measures a copied canonical provider snapshot.
///
/// # Safety
///
/// `info` must point to writable V1 storage for this call only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_snapshot_measure_v1(
    token: u64,
    info: *mut SakuraOutputProviderSnapshotInfoV1,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(info.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The info slot is validated and caller-owned for this call.
        unsafe { info.write(poison_info()) };
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export validates and writes only the V1 result.
        unsafe { output_shadow::model_snapshot_measure_v1(shadow_token, info.cast()) }
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Writes a measured canonical provider snapshot into caller-owned storage.
///
/// # Safety
///
/// `buffer` and its destination span follow the V1 copied output contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_snapshot_write_v1(
    token: u64,
    buffer: *mut SakuraOutputProviderSnapshotBufferV1,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(buffer.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The descriptor slot is validated and caller-owned.
        unsafe { (*buffer).length = u64::MAX };
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export checks all destination metadata and
        // retains no output pointer.
        unsafe { output_shadow::model_snapshot_write_v1(shadow_token, buffer.cast()) }
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Copies only the active-channel identifier for an advisory notification.
/// This is O(identifier length), not O(retained channel text).
///
/// # Safety
///
/// `active` and its optional destination span follow the V1 copied output
/// contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_active_channel_v1(
    token: u64,
    active: *mut SakuraOutputProviderActiveChannelV1,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(active.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            // SAFETY: The active descriptor was validated and remains
            // caller-owned for this call. Poison only the invalid-handle path;
            // the model helper must retain the original destination metadata
            // for valid-token calls.
            unsafe { active.write(poison_active()) };
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export validates descriptor/range fields and
        // copies no destination pointer into the model.
        unsafe { output_shadow::read_active_channel_internal(shadow_token, active.cast()) }
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Stops one provider.  Stop is idempotent and leaves a valid stopped token
/// for final snapshot inspection until the explicit destroy call.
///
/// # Safety
///
/// `result` must point to writable V1 storage for this call only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_stop_v1(
    token: u64,
    result: *mut SakuraOutputProviderApplyResultV1,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(result.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The result slot is validated and caller-owned.
        unsafe { result.write(poison_result()) };
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export performs terminal-state handling.
        unsafe { output_shadow::model_stop_v1(shadow_token, result.cast()) }
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

/// Destroys one provider token and consumes the caller's token on success.
///
/// # Safety
///
/// `token` must point to writable token storage for this call only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_destroy_v1(
    token: *mut u64,
) -> SakuraOutputProviderStatus {
    catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The token slot is validated and caller-owned.
        let value = unsafe { token.read() };
        if value == 0 {
            return SakuraOutputProviderStatus::InvalidHandle;
        }
        let mut registry = lock_providers();
        let Some(&stored_shadow_token) = registry.providers.get(&value) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        let mut shadow_token = stored_shadow_token;
        // SAFETY: The wrapped export consumes the Rust-owned temporary token.
        let status = unsafe { output_shadow::model_destroy_v1(&mut shadow_token) };
        if status != SakuraOutputProviderStatus::Ok {
            return status;
        }
        registry.providers.remove(&value);
        // SAFETY: Consume the caller token only after successful destruction.
        unsafe { token.write(0) };
        SakuraOutputProviderStatus::Ok
    }))
    .unwrap_or(SakuraOutputProviderStatus::InternalError)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::output_shadow;
    use std::ptr;
    use std::sync::{Arc, Barrier};
    use std::thread;

    fn limits() -> SakuraOutputProviderLimitsV1 {
        SakuraOutputProviderLimitsV1 {
            struct_size: size_of::<SakuraOutputProviderLimitsV1>() as u32,
            abi_version: 1,
            maximum_owners: 4,
            maximum_channels: 4,
            maximum_text_bytes_per_channel: 64,
            maximum_payload_bytes: 128,
            maximum_log_entries_per_channel: 4,
            maximum_remembered_operations: 8,
            reserved: [0; 3],
        }
    }

    fn span(bytes: &[u8]) -> SakuraOutputProviderSpanV1 {
        SakuraOutputProviderSpanV1 {
            struct_size: size_of::<SakuraOutputProviderSpanV1>() as u32,
            abi_version: 1,
            data: if bytes.is_empty() {
                ptr::null()
            } else {
                bytes.as_ptr()
            },
            length: bytes.len() as u64,
            reserved: [0; 2],
        }
    }

    fn create_request<'a>(
        operation_id: &'a [u8],
        owner_id: &'a [u8],
        channel_id: &'a [u8],
        label: &'a [u8],
    ) -> SakuraOutputProviderRequestV1 {
        SakuraOutputProviderRequestV1 {
            struct_size: size_of::<SakuraOutputProviderRequestV1>() as u32,
            abi_version: 1,
            operation_kind: 1,
            channel_kind: 0,
            flags: 0,
            operation_id: span(operation_id),
            expected_revision: 0,
            owner_id: span(owner_id),
            owner_generation: 1,
            channel_id: span(channel_id),
            label: span(label),
            metadata_language_id: span(&[]),
            metadata_source: span(&[]),
            payload: span(&[]),
            log_entries: ptr::null(),
            log_entry_count: 0,
            reserved: [0; 4],
        }
    }

    fn empty_request(operation_id: &[u8], operation_kind: u32) -> SakuraOutputProviderRequestV1 {
        let mut request = create_request(operation_id, b"owner", b"channel", b"Label");
        request.operation_kind = operation_kind;
        request.label = span(&[]);
        request
    }

    fn append_log_request(
        operation_id: &[u8],
        channel_id: &[u8],
        entries: &[SakuraOutputProviderLogEntryV1],
    ) -> SakuraOutputProviderRequestV1 {
        let mut request = empty_request(operation_id, 4);
        request.channel_id = span(channel_id);
        request.log_entries = entries.as_ptr();
        request.log_entry_count = entries.len() as u64;
        request
    }

    fn create_provider_token() -> u64 {
        let raw_limits = limits();
        let mut token = 0_u64;
        // SAFETY: The limits and token are initialized caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_create_v1(&raw_limits, &mut token)
        });
        assert_ne!(0, token);
        token
    }

    fn valid_log_entry(message: &[u8]) -> SakuraOutputProviderLogEntryV1 {
        SakuraOutputProviderLogEntryV1 {
            struct_size: size_of::<SakuraOutputProviderLogEntryV1>() as u32,
            abi_version: 1,
            level: 2,
            flags: 0,
            message: span(message),
            source: span(&[]),
            reserved: [0; 2],
        }
    }

    fn snapshot_buffer(storage: &mut [u8]) -> SakuraOutputProviderSnapshotBufferV1 {
        SakuraOutputProviderSnapshotBufferV1 {
            struct_size: size_of::<SakuraOutputProviderSnapshotBufferV1>() as u32,
            abi_version: 1,
            data: if storage.is_empty() {
                ptr::null_mut()
            } else {
                storage.as_mut_ptr()
            },
            capacity: storage.len() as u64,
            length: 0,
            reserved: [0; 2],
        }
    }

    fn active_buffer(storage: &mut [u8]) -> SakuraOutputProviderActiveChannelV1 {
        SakuraOutputProviderActiveChannelV1 {
            struct_size: size_of::<SakuraOutputProviderActiveChannelV1>() as u32,
            abi_version: 1,
            revision: 0,
            present: 0,
            reserved0: [0; 7],
            data: if storage.is_empty() {
                ptr::null_mut()
            } else {
                storage.as_mut_ptr()
            },
            capacity: storage.len() as u64,
            length: 0,
            reserved: [0; 2],
        }
    }

    fn assert_poison_result(result: &SakuraOutputProviderApplyResultV1) {
        assert_eq!(
            size_of::<SakuraOutputProviderApplyResultV1>() as u32,
            result.struct_size
        );
        assert_eq!(1, result.abi_version);
        assert_eq!(
            SakuraOutputProviderOperationStatus::RevisionExhausted as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidPayload as u32,
            result.reason
        );
        assert_eq!(u64::MAX, result.revision);
        assert_eq!(0, result.callback_drain_deferred);
        assert_eq!([0; 7], result.reserved);
    }

    fn assert_poison_info(info: &SakuraOutputProviderSnapshotInfoV1) {
        assert_eq!(
            size_of::<SakuraOutputProviderSnapshotInfoV1>() as u32,
            info.struct_size
        );
        assert_eq!(1, info.abi_version);
        assert_eq!(u64::MAX, info.revision);
        assert_eq!(0xff, info.stopped);
        assert_eq!(0xff, info.active_channel_present);
        assert_eq!([0; 6], info.reserved0);
        assert_eq!(u64::MAX, info.dropped_notification_count);
        assert_eq!(u64::MAX, info.channel_count);
        assert_eq!(u64::MAX, info.encoded_size);
        assert_eq!([0; 2], info.reserved);
    }

    fn assert_poison_active(active: &SakuraOutputProviderActiveChannelV1) {
        assert_eq!(
            size_of::<SakuraOutputProviderActiveChannelV1>() as u32,
            active.struct_size
        );
        assert_eq!(1, active.abi_version);
        assert_eq!(u64::MAX, active.revision);
        assert_eq!(0xff, active.present);
        assert_eq!([0; 7], active.reserved0);
        assert!(active.data.is_null());
        assert_eq!(u64::MAX, active.capacity);
        assert_eq!(u64::MAX, active.length);
        assert_eq!([0; 2], active.reserved);
    }

    fn assert_poison_snapshot_length(buffer: &SakuraOutputProviderSnapshotBufferV1) {
        assert_eq!(u64::MAX, buffer.length);
    }

    #[test]
    fn provider_create_rejects_bad_headers_and_pointer_ranges() {
        let good_limits = limits();
        let mut token = 0xfeed_beef_u64;

        let mut wrong_size = good_limits;
        wrong_size.struct_size -= 1;
        // SAFETY: The limits and token are writable local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&wrong_size, &mut token)
        });
        assert_eq!(0, token);

        let mut wrong_version = good_limits;
        wrong_version.abi_version = 2;
        token = 0xfeed_beef;
        // SAFETY: The limits and token are writable local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&wrong_version, &mut token)
        });
        assert_eq!(0, token);

        let mut nonzero_reserved = good_limits;
        nonzero_reserved.reserved[1] = 1;
        token = 0xfeed_beef;
        // SAFETY: The limits and token are writable local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&nonzero_reserved, &mut token)
        });
        assert_eq!(0, token);

        token = 0xfeed_beef;
        // SAFETY: A null limits pointer must be rejected before dereference.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(ptr::null(), &mut token)
        });
        assert_eq!(0, token);

        let misaligned_limits = (&good_limits as *const SakuraOutputProviderLimitsV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderLimitsV1>();
        token = 0xfeed_beef;
        // SAFETY: The deliberately misaligned limits pointer is rejected by
        // the complete top-level range/alignment check before it is read.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(misaligned_limits, &mut token)
        });
        assert_eq!(0, token);

        let overflow_limits = (isize::MAX as usize + 1) as *const SakuraOutputProviderLimitsV1;
        token = 0xfeed_beef;
        // SAFETY: The address is outside the addressable isize range and must
        // be rejected before any dereference.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(overflow_limits, &mut token)
        });
        assert_eq!(0, token);

        // SAFETY: A null output token pointer must be rejected before write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&good_limits, ptr::null_mut())
        });

        let mut token_slot = 0xfeed_beef_u64;
        let misaligned_token = (&mut token_slot as *mut u64)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<u64>();
        // SAFETY: The deliberately misaligned token pointer is rejected
        // before it is read or written.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&good_limits, misaligned_token)
        });

        let overflow_token = (isize::MAX as usize + 1) as *mut u64;
        // SAFETY: The address is outside the addressable isize range and must
        // be rejected before any dereference.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_create_v1(&good_limits, overflow_token)
        });
    }

    #[test]
    fn provider_every_export_rejects_invalid_top_level_pointers() {
        let token = create_provider_token();
        let request = create_request(b"top-level-pointers", b"owner", b"channel", b"Label");
        let mut result = poison_result();

        // SAFETY: A null result pointer must be rejected before any write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &request, ptr::null_mut())
        });

        // SAFETY: A null snapshot-info pointer must be rejected before any
        // write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, ptr::null_mut())
        });

        // SAFETY: A null snapshot-buffer descriptor must be rejected before
        // any write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, ptr::null_mut())
        });

        // SAFETY: A null active-channel descriptor must be rejected before any
        // write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, ptr::null_mut())
        });

        // SAFETY: A null Stop result pointer must be rejected before any
        // write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_stop_v1(token, ptr::null_mut())
        });

        // SAFETY: A null destroy-token pointer must be rejected before any
        // read or write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_destroy_v1(ptr::null_mut())
        });

        let misaligned_stop_result = (&mut result as *mut SakuraOutputProviderApplyResultV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderApplyResultV1>();
        // SAFETY: The deliberately misaligned Stop result pointer is rejected
        // before it is written.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_stop_v1(token, misaligned_stop_result)
        });

        let overflow_stop_result =
            (isize::MAX as usize + 1) as *mut SakuraOutputProviderApplyResultV1;
        // SAFETY: The Stop result address is outside the addressable isize
        // range and must be rejected before any write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_stop_v1(token, overflow_stop_result)
        });

        let mut token_slot = token;
        let misaligned_destroy_token = (&mut token_slot as *mut u64)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<u64>();
        // SAFETY: The deliberately misaligned destroy-token pointer is
        // rejected before it is read or written.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_destroy_v1(misaligned_destroy_token)
        });
        assert_eq!(token, token_slot);

        let overflow_destroy_token = (isize::MAX as usize + 1) as *mut u64;
        // SAFETY: The destroy-token address is outside the addressable isize
        // range and must be rejected before any read or write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_destroy_v1(overflow_destroy_token)
        });
        assert_eq!(token, token_slot);

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after the pointer boundary cases complete.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token_slot)
        });
        assert_eq!(0, token_slot);
    }

    #[test]
    fn provider_apply_rejects_malformed_inputs_and_poison_outputs() {
        let token = create_provider_token();
        let valid = create_request(b"create", b"owner", b"channel", b"Label");

        let mut result = poison_result();
        // SAFETY: The result is writable local storage; a null request is an
        // intentionally invalid ABI input.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, ptr::null(), &mut result)
        });
        assert_poison_result(&result);

        let mut wrong_size = valid;
        wrong_size.struct_size -= 1;
        // SAFETY: The request and result are local storage with an invalid
        // request header deliberately supplied for this test.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &wrong_size, &mut result)
        });
        assert_poison_result(&result);

        let mut wrong_version = valid;
        wrong_version.abi_version = 2;
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &wrong_version, &mut result)
        });
        assert_poison_result(&result);

        let mut nonzero_reserved = valid;
        nonzero_reserved.reserved[0] = 1;
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &nonzero_reserved, &mut result)
        });
        assert_poison_result(&result);

        let mut bad_span_header = valid;
        bad_span_header.operation_id.struct_size -= 1;
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &bad_span_header, &mut result)
        });
        assert_poison_result(&result);

        let mut bad_span_version = valid;
        bad_span_version.owner_id.abi_version = 2;
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &bad_span_version, &mut result)
        });
        assert_poison_result(&result);

        let mut bad_span_reserved = valid;
        bad_span_reserved.channel_id.reserved[0] = 1;
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &bad_span_reserved, &mut result)
        });
        assert_poison_result(&result);

        let bad_data = [0xff_u8];
        let mut null_span = valid;
        null_span.payload = SakuraOutputProviderSpanV1 {
            data: ptr::null(),
            length: 1,
            ..span(&[])
        };
        // SAFETY: The non-empty null payload span must be rejected before
        // dereference and the output must remain poisoned.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &null_span, &mut result)
        });
        assert_poison_result(&result);

        let mut overflow_span = valid;
        overflow_span.payload = SakuraOutputProviderSpanV1 {
            data: usize::MAX as *const u8,
            length: 1,
            ..span(&[])
        };
        // SAFETY: The deliberately overflowing byte range is rejected before
        // it is converted into a slice.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &overflow_span, &mut result)
        });
        assert_poison_result(&result);

        let misaligned_request = (&valid as *const SakuraOutputProviderRequestV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderRequestV1>();
        // SAFETY: The deliberately misaligned top-level request is rejected
        // before it is read.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, misaligned_request, &mut result)
        });
        assert_poison_result(&result);

        let misaligned_result = (&mut result as *mut SakuraOutputProviderApplyResultV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderApplyResultV1>();
        // SAFETY: The deliberately misaligned output pointer is rejected
        // before it is written.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &valid, misaligned_result)
        });

        let mut overlap_request = valid;
        let overlap_request_ptr = &mut overlap_request as *mut SakuraOutputProviderRequestV1;
        let overlap_result_ptr = overlap_request_ptr.cast::<SakuraOutputProviderApplyResultV1>();
        // SAFETY: The two descriptors intentionally overlap. The provider
        // poisons the result first, then rejects the overlapping request range
        // without reading the now-overwritten request.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, overlap_request_ptr, overlap_result_ptr)
        });
        // SAFETY: The result pointer refers to the same initialized storage
        // just written by the provider's poison step.
        assert_poison_result(unsafe { &*overlap_result_ptr });

        let mut bad_log_pointer = empty_request(b"bad-log-pointer", 4);
        let entry = valid_log_entry(b"message");
        bad_log_pointer.log_entries = (&entry as *const SakuraOutputProviderLogEntryV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderLogEntryV1>();
        bad_log_pointer.log_entry_count = 1;
        // SAFETY: The deliberately misaligned log array is rejected before a
        // slice is constructed.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &bad_log_pointer, &mut result)
        });
        assert_poison_result(&result);

        let mut overflowing_log_count = empty_request(b"bad-log-count", 4);
        overflowing_log_count.log_entries = ptr::null();
        overflowing_log_count.log_entry_count = u64::MAX;
        // SAFETY: The count overflow is rejected before any log array read.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_apply_v1(token, &overflowing_log_count, &mut result)
        });
        assert_poison_result(&result);

        let mut invalid_utf8_operation = valid;
        invalid_utf8_operation.operation_id = span(&bad_data);
        // SAFETY: The invalid UTF-8 bytes are in caller-owned local storage;
        // the provider copies them and returns a typed operation rejection.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_utf8_operation, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidOperationId as u32,
            result.reason
        );

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after all malformed calls have completed.
        let mut token = token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn provider_rejects_invalid_utf8_in_required_and_optional_spans() {
        let token = create_provider_token();
        let invalid = [0xff_u8];
        let mut result = poison_result();

        let invalid_owner = create_request(b"invalid-owner", &invalid, b"channel", b"Label");
        // SAFETY: The request and result are local storage; invalid UTF-8 is
        // copied and rejected as an operation-level validation failure.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_owner, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidOwner as u32,
            result.reason
        );

        let invalid_channel = create_request(b"invalid-channel", b"owner", &invalid, b"Label");
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_channel, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidChannelId as u32,
            result.reason
        );

        let invalid_label = create_request(b"invalid-label", b"owner", b"channel", &invalid);
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_label, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidLabel as u32,
            result.reason
        );

        let mut invalid_language =
            create_request(b"invalid-language", b"owner", b"channel", b"Label");
        invalid_language.flags = 1 << 2;
        invalid_language.metadata_language_id = span(&invalid);
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_language, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidMetadata as u32,
            result.reason
        );

        let mut invalid_source = create_request(b"invalid-source", b"owner", b"channel", b"Label");
        invalid_source.flags = 1 << 3;
        invalid_source.metadata_source = span(&invalid);
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_source, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidMetadata as u32,
            result.reason
        );

        let mut invalid_payload = empty_request(b"invalid-payload", 2);
        invalid_payload.payload = span(&invalid);
        // SAFETY: The request and result are local storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_payload, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidPayload as u32,
            result.reason
        );

        let log_channel = create_request(b"create-log", b"owner", b"log", b"Log");
        let mut log_channel = log_channel;
        log_channel.channel_kind = 1;
        // SAFETY: The request and result are valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &log_channel, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            result.status
        );

        let invalid_message_entry = SakuraOutputProviderLogEntryV1 {
            message: span(&invalid),
            ..valid_log_entry(b"message")
        };
        let invalid_message_request = append_log_request(
            b"invalid-log-message",
            b"log",
            std::slice::from_ref(&invalid_message_entry),
        );
        // SAFETY: The log entry and request are caller-owned for this call.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_message_request, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidPayload as u32,
            result.reason
        );

        let invalid_source_entry = SakuraOutputProviderLogEntryV1 {
            flags: 1,
            source: span(&invalid),
            ..valid_log_entry(b"message")
        };
        let invalid_source_request = append_log_request(
            b"invalid-log-source",
            b"log",
            std::slice::from_ref(&invalid_source_entry),
        );
        // SAFETY: The log entry and request are caller-owned for this call.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &invalid_source_request, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Rejected as u32,
            result.status
        );
        assert_eq!(
            SakuraOutputProviderReason::InvalidPayload as u32,
            result.reason
        );

        // An absent optional source has a valid empty descriptor and is
        // accepted, proving that optional presence is not inferred from data.
        let valid_entry = valid_log_entry(b"message");
        let valid_log_request = append_log_request(
            b"valid-log-source-absent",
            b"log",
            std::slice::from_ref(&valid_entry),
        );
        // SAFETY: The log entry and request are caller-owned for this call.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &valid_log_request, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            result.status
        );

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after all UTF-8 cases have completed.
        let mut token = token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn provider_copies_borrowed_input_and_output_spans_without_retaining_pointers() {
        let token = create_provider_token();
        let mut operation_id = b"copy-create".to_vec();
        let mut owner_id = b"owner".to_vec();
        let mut channel_id = b"channel".to_vec();
        let mut label = b"Label".to_vec();
        let request = SakuraOutputProviderRequestV1 {
            operation_id: span(&operation_id),
            owner_id: span(&owner_id),
            channel_id: span(&channel_id),
            label: span(&label),
            ..create_request(b"unused", b"unused", b"unused", b"unused")
        };
        let mut result = poison_result();
        // SAFETY: All request spans point to caller-owned vectors that remain
        // alive and immutable for this call only.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &request, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            result.status
        );

        operation_id.fill(b'x');
        owner_id.fill(b'y');
        channel_id.fill(b'z');
        label.fill(b'w');

        let mut active_storage = vec![0_u8; 32];
        let mut active = active_buffer(&mut active_storage);
        // SAFETY: The active destination is caller-owned for this call; the
        // earlier input vectors have been mutated after the provider returned.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut active)
        });
        assert_eq!(b"channel", &active_storage[..active.length as usize]);

        let mut payload = b"original-payload".to_vec();
        let mut append = empty_request(b"copy-append", 2);
        append.payload = span(&payload);
        // SAFETY: The payload span is caller-owned for this call only.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &append, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            result.status
        );
        payload.fill(b'q');

        let mut info = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut info)
        });
        let mut first_storage = vec![0_u8; info.encoded_size as usize];
        let mut first_buffer = snapshot_buffer(&mut first_storage);
        // SAFETY: The snapshot destination is caller-owned for this call only.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut first_buffer)
        });
        assert!(first_storage
            .windows(b"channel".len())
            .any(|window| window == b"channel"));
        assert!(first_storage
            .windows(b"original-payload".len())
            .any(|window| window == b"original-payload"));
        drop(first_storage);

        let mut second_storage = vec![0_u8; info.encoded_size as usize];
        let mut second_buffer = snapshot_buffer(&mut second_storage);
        // SAFETY: A new caller-owned destination proves the previous output
        // span was not retained by the provider.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut second_buffer)
        });
        assert_eq!(first_buffer.length, second_buffer.length);

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after the borrowed-span lifetime cases complete.
        let mut token = token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn provider_snapshot_measure_write_rejects_malformed_destinations() {
        let token = create_provider_token();
        let create = create_request(b"snapshot-create", b"owner", b"channel", b"Label");
        let mut result = poison_result();
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &create, &mut result)
        });

        let mut info = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut info)
        });
        let first_size = info.encoded_size;
        assert!(first_size > 0);

        let mut invalid_info = poison_info();
        // SAFETY: Zero is a stale provider token; the info slot is writable.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_snapshot_measure_v1(0, &mut invalid_info)
        });
        assert_poison_info(&invalid_info);

        let misaligned_info = (&mut info as *mut SakuraOutputProviderSnapshotInfoV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderSnapshotInfoV1>();
        // SAFETY: The deliberately misaligned info pointer is rejected before
        // any output write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, misaligned_info)
        });

        let mut storage = vec![0_u8; first_size as usize];
        let mut wrong_size = snapshot_buffer(&mut storage);
        wrong_size.struct_size -= 1;
        // SAFETY: The destination descriptor has an invalid struct size.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut wrong_size)
        });
        assert_poison_snapshot_length(&wrong_size);

        let mut wrong_version = snapshot_buffer(&mut storage);
        wrong_version.abi_version = 2;
        // SAFETY: The destination descriptor has an invalid ABI version.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut wrong_version)
        });
        assert_poison_snapshot_length(&wrong_version);

        let mut nonzero_reserved = snapshot_buffer(&mut storage);
        nonzero_reserved.reserved[0] = 1;
        // SAFETY: The destination descriptor has nonzero reserved bytes.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut nonzero_reserved)
        });
        assert_poison_snapshot_length(&nonzero_reserved);

        let mut null_data = [];
        let mut null_destination = snapshot_buffer(&mut null_data);
        null_destination.capacity = 1;
        // SAFETY: A nonzero destination capacity with a null data pointer is
        // rejected before any copy.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut null_destination)
        });
        assert_poison_snapshot_length(&null_destination);

        let mut overflow_destination = snapshot_buffer(&mut storage);
        overflow_destination.data = usize::MAX as *mut u8;
        overflow_destination.capacity = 1;
        // SAFETY: The destination address range overflows and is rejected
        // before it is dereferenced.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut overflow_destination)
        });
        assert_poison_snapshot_length(&overflow_destination);

        let mut overlap_destination = snapshot_buffer(&mut storage);
        let overlap_pointer = &mut overlap_destination as *mut SakuraOutputProviderSnapshotBufferV1;
        overlap_destination.data = overlap_pointer.cast::<u8>();
        overlap_destination.capacity = 1;
        // SAFETY: The output descriptor and its destination intentionally
        // overlap and must be rejected before copying.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut overlap_destination)
        });
        assert_poison_snapshot_length(&overlap_destination);

        let misaligned_buffer = (&mut overlap_destination
            as *mut SakuraOutputProviderSnapshotBufferV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderSnapshotBufferV1>();
        // SAFETY: The deliberately misaligned buffer descriptor is rejected
        // before the provider writes its length poison.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, misaligned_buffer)
        });

        let second_create = create_request(b"snapshot-create-2", b"owner", b"second", b"Label");
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &second_create, &mut result)
        });
        let mut new_info = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut new_info)
        });
        assert!(new_info.encoded_size > first_size);

        // The model changed between measure and write. The old bounded buffer
        // must fail closed instead of truncating or overflowing the snapshot.
        let mut old_storage = vec![0_u8; first_size as usize];
        let mut old_buffer = snapshot_buffer(&mut old_storage);
        // SAFETY: The destination is valid but intentionally smaller than the
        // newly measured canonical stream.
        assert_eq!(SakuraOutputProviderStatus::InsufficientCapacity, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut old_buffer)
        });
        assert_poison_snapshot_length(&old_buffer);

        let mut final_storage = vec![0_u8; new_info.encoded_size as usize];
        let mut final_buffer = snapshot_buffer(&mut final_storage);
        // SAFETY: The destination exactly matches the second measure result.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut final_buffer)
        });
        assert_eq!(new_info.encoded_size, final_buffer.length);
        assert!(final_storage.starts_with(b"SAKURA_OUTPUT_MODEL_V1\0"));

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after the malformed destination cases complete.
        let mut token = token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn provider_active_channel_rejects_malformed_descriptors_and_poison_outputs() {
        let token = create_provider_token();
        let create = create_request(b"active-create", b"owner", b"channel", b"Label");
        let mut result = poison_result();
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &create, &mut result)
        });

        let mut empty = [];
        let mut measured = active_buffer(&mut empty);
        // SAFETY: A zero-capacity active query asks for the bounded required
        // length and does not provide a destination pointer.
        assert_eq!(SakuraOutputProviderStatus::InsufficientCapacity, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut measured)
        });
        assert_eq!(1, measured.present);
        assert_eq!(b"channel".len() as u64, measured.length);
        assert!(measured.data.is_null());
        assert_eq!(0, measured.capacity);

        let mut active_storage = vec![0_u8; measured.length as usize];
        let mut active = active_buffer(&mut active_storage);
        // SAFETY: The destination exactly matches the required active ID.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut active)
        });
        assert_eq!(b"channel", active_storage.as_slice());

        let mut wrong_size = active_buffer(&mut active_storage);
        wrong_size.struct_size -= 1;
        // SAFETY: The active descriptor has an invalid struct size.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut wrong_size)
        });
        assert_poison_active(&wrong_size);

        let mut wrong_version = active_buffer(&mut active_storage);
        wrong_version.abi_version = 2;
        // SAFETY: The active descriptor has an invalid ABI version.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut wrong_version)
        });
        assert_poison_active(&wrong_version);

        let mut nonzero_reserved0 = active_buffer(&mut active_storage);
        nonzero_reserved0.reserved0[0] = 1;
        // SAFETY: The active descriptor has nonzero reserved0 bytes.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut nonzero_reserved0)
        });
        assert_poison_active(&nonzero_reserved0);

        let mut nonzero_reserved = active_buffer(&mut active_storage);
        nonzero_reserved.reserved[0] = 1;
        // SAFETY: The active descriptor has nonzero reserved bytes.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut nonzero_reserved)
        });
        assert_poison_active(&nonzero_reserved);

        let mut invalid_handle = active_buffer(&mut active_storage);
        // SAFETY: Zero is a stale provider token; the active output is valid
        // local storage and must be poisoned on failure.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_active_channel_v1(0, &mut invalid_handle)
        });
        assert_poison_active(&invalid_handle);

        let mut null_data = [];
        let mut null_destination = active_buffer(&mut null_data);
        null_destination.capacity = 1;
        // SAFETY: A nonzero capacity with a null destination is invalid.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut null_destination)
        });
        assert_poison_active(&null_destination);

        let mut overflow_destination = active_buffer(&mut active_storage);
        overflow_destination.data = usize::MAX as *mut u8;
        overflow_destination.capacity = 1;
        // SAFETY: The destination address range overflows and is rejected.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut overflow_destination)
        });
        assert_poison_active(&overflow_destination);

        let mut overlap_destination = active_buffer(&mut active_storage);
        let overlap_pointer = &mut overlap_destination as *mut SakuraOutputProviderActiveChannelV1;
        overlap_destination.data = overlap_pointer.cast::<u8>();
        overlap_destination.capacity = 1;
        // SAFETY: The active descriptor and destination intentionally overlap.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut overlap_destination)
        });
        assert_poison_active(&overlap_destination);

        let misaligned_active = (&mut overlap_destination
            as *mut SakuraOutputProviderActiveChannelV1)
            .cast::<u8>()
            .wrapping_add(1)
            .cast::<SakuraOutputProviderActiveChannelV1>();
        // SAFETY: The deliberately misaligned active descriptor is rejected
        // before any read or write.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_active_channel_v1(token, misaligned_active)
        });

        // SAFETY: The token is valid and the result is caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_stop_v1(token, &mut result)
        });
        let mut stopped_empty = [];
        let mut stopped_active = active_buffer(&mut stopped_empty);
        // SAFETY: A stopped provider remains queryable and has no active ID.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_active_channel_v1(token, &mut stopped_active)
        });
        assert_eq!(0, stopped_active.present);
        assert_eq!(0, stopped_active.length);

        // SAFETY: The token was created by this test and is destroyed exactly
        // once after all active descriptor cases complete.
        let mut token = token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn provider_destroyed_tokens_fail_closed_for_every_export() {
        let mut token = create_provider_token();
        let stale_token = token;
        let request = create_request(b"terminal-create", b"owner", b"channel", b"Label");

        let mut stop_result = poison_result();
        // SAFETY: The result is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_stop_v1(token, &mut stop_result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            stop_result.status
        );
        let stopped_revision = stop_result.revision;

        let mut repeated_stop = poison_result();
        // SAFETY: Stop is idempotent while the token remains valid.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_stop_v1(token, &mut repeated_stop)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            repeated_stop.status
        );
        assert_eq!(stopped_revision, repeated_stop.revision);

        // SAFETY: The token is valid and consumed only after successful
        // destruction.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);

        let mut result = poison_result();
        // SAFETY: The token was destroyed; the result remains writable local
        // storage and must be poisoned on the stale-handle failure.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_apply_v1(stale_token, &request, &mut result)
        });
        assert_poison_result(&result);

        let mut info = poison_info();
        // SAFETY: The stale token must fail closed and poison snapshot info.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_snapshot_measure_v1(stale_token, &mut info)
        });
        assert_poison_info(&info);

        let mut storage = vec![0_u8; 128];
        let mut buffer = snapshot_buffer(&mut storage);
        // SAFETY: The stale token must fail closed and poison the output
        // length before any destination write.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_snapshot_write_v1(stale_token, &mut buffer)
        });
        assert_poison_snapshot_length(&buffer);

        let mut active_storage = vec![0_u8; 64];
        let mut active = active_buffer(&mut active_storage);
        // SAFETY: The stale token must fail closed and poison active metadata.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_active_channel_v1(stale_token, &mut active)
        });
        assert_poison_active(&active);

        let mut stale_stop = poison_result();
        // SAFETY: The stale token must fail closed and poison Stop output.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_stop_v1(stale_token, &mut stale_stop)
        });
        assert_poison_result(&stale_stop);

        // A repeated destroy is a stale-handle failure and must not consume or
        // rewrite the caller's already-zero token.
        // SAFETY: The token slot is valid local storage and contains zero.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);

        let mut stale_slot = stale_token;
        // SAFETY: The slot is valid but contains a destroyed provider token.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_destroy_v1(&mut stale_slot)
        });
        assert_eq!(stale_token, stale_slot);
    }

    #[test]
    fn provider_cross_family_tokens_fail_closed_for_all_operations() {
        let provider_token = create_provider_token();
        let raw_limits = limits();
        let mut shadow_token = 0_u64;
        // SAFETY: The limits and token are initialized caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            output_shadow::model_create_v1(&raw_limits, &mut shadow_token)
        });
        assert_ne!(0, shadow_token);
        assert_eq!(0, shadow_token & PROVIDER_TOKEN_TAG);

        let request = create_request(b"cross-family", b"owner", b"channel", b"Label");
        let mut result = poison_result();
        // SAFETY: A replay-model token must not be accepted by the provider.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_apply_v1(shadow_token, &request, &mut result)
        });
        assert_poison_result(&result);

        let mut info = poison_info();
        // SAFETY: A replay-model token must not be accepted by provider
        // snapshot measurement.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_snapshot_measure_v1(shadow_token, &mut info)
        });
        assert_poison_info(&info);

        let mut storage = vec![0_u8; 128];
        let mut buffer = snapshot_buffer(&mut storage);
        // SAFETY: A replay-model token must not be accepted by provider
        // snapshot writing.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_snapshot_write_v1(shadow_token, &mut buffer)
        });
        assert_poison_snapshot_length(&buffer);

        let mut active_storage = vec![0_u8; 64];
        let mut active = active_buffer(&mut active_storage);
        // SAFETY: A replay-model token must not be accepted by provider
        // active-channel queries.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_active_channel_v1(shadow_token, &mut active)
        });
        assert_poison_active(&active);

        let mut shadow_stop = poison_result();
        // SAFETY: A replay-model token must not be accepted by provider Stop.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_stop_v1(shadow_token, &mut shadow_stop)
        });
        assert_poison_result(&shadow_stop);

        let mut shadow_slot = shadow_token;
        // SAFETY: A replay-model token must not be consumed by provider
        // destruction.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_destroy_v1(&mut shadow_slot)
        });
        assert_eq!(shadow_token, shadow_slot);

        let mut model_result = poison_result();
        // SAFETY: A provider token must not be accepted by the replay model.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_apply_v1(provider_token, &request, &mut model_result)
        });
        assert_poison_result(&model_result);

        let mut model_info = poison_info();
        // SAFETY: A provider token must not be accepted by replay snapshot
        // measurement.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_snapshot_measure_v1(provider_token, &mut model_info)
        });
        assert_poison_info(&model_info);

        let mut model_storage = vec![0_u8; 128];
        let mut model_buffer = snapshot_buffer(&mut model_storage);
        // SAFETY: A provider token must not be accepted by replay snapshot
        // writing.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_snapshot_write_v1(provider_token, &mut model_buffer)
        });
        assert_poison_snapshot_length(&model_buffer);

        let mut model_active_storage = vec![0_u8; 64];
        let mut model_active = active_buffer(&mut model_active_storage);
        // SAFETY: A provider token must not be accepted by replay active
        // channel queries.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::read_active_channel_internal(provider_token, &mut model_active)
        });
        assert_poison_active(&model_active);

        let mut model_stop = poison_result();
        // SAFETY: A provider token must not be accepted by replay Stop.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_stop_v1(provider_token, &mut model_stop)
        });
        assert_poison_result(&model_stop);

        let mut provider_slot_for_model = provider_token;
        // SAFETY: A provider token must not be consumed by replay destruction.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_destroy_v1(&mut provider_slot_for_model)
        });
        assert_eq!(provider_token, provider_slot_for_model);

        // SAFETY: The provider token is destroyed through its own family.
        let mut provider_slot = provider_token;
        // SAFETY: The token slot is valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut provider_slot)
        });
        assert_eq!(0, provider_slot);
        // SAFETY: The shadow token is destroyed through its own model family.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            output_shadow::model_destroy_v1(&mut shadow_token)
        });
        assert_eq!(0, shadow_token);
    }

    #[test]
    fn provider_exports_are_safe_under_concurrent_apply_snapshot_stop_destroy() {
        let token = create_provider_token();
        let barrier = Arc::new(Barrier::new(6));
        let mut handles = Vec::new();

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let request = create_request(b"concurrent-create", b"owner", b"channel", b"Label");
                let mut result = poison_result();
                // SAFETY: All request/result storage is local to this worker;
                // the token is an opaque copied value.
                let status =
                    unsafe { sakura_output_provider_apply_v1(token, &request, &mut result) };
                assert!(matches!(
                    status,
                    SakuraOutputProviderStatus::Ok
                        | SakuraOutputProviderStatus::Stopped
                        | SakuraOutputProviderStatus::InvalidHandle
                ));
            }));
        }

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let mut info = poison_info();
                // SAFETY: The info slot is local to this worker and the token
                // is an opaque copied value.
                let status =
                    unsafe { sakura_output_provider_snapshot_measure_v1(token, &mut info) };
                assert!(matches!(
                    status,
                    SakuraOutputProviderStatus::Ok | SakuraOutputProviderStatus::InvalidHandle
                ));
            }));
        }

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let mut storage = vec![0_u8; 4096];
                let mut buffer = snapshot_buffer(&mut storage);
                // SAFETY: The destination is caller-owned and bounded for the
                // duration of this worker call.
                let status =
                    unsafe { sakura_output_provider_snapshot_write_v1(token, &mut buffer) };
                assert!(matches!(
                    status,
                    SakuraOutputProviderStatus::Ok
                        | SakuraOutputProviderStatus::InsufficientCapacity
                        | SakuraOutputProviderStatus::InvalidHandle
                ));
            }));
        }

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let mut storage = vec![0_u8; 256];
                let mut active = active_buffer(&mut storage);
                // SAFETY: The active destination is caller-owned and bounded
                // for the duration of this worker call.
                let status =
                    unsafe { sakura_output_provider_active_channel_v1(token, &mut active) };
                assert!(matches!(
                    status,
                    SakuraOutputProviderStatus::Ok
                        | SakuraOutputProviderStatus::InsufficientCapacity
                        | SakuraOutputProviderStatus::InvalidHandle
                ));
            }));
        }

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let mut result = poison_result();
                // SAFETY: The result slot is local to this worker and the
                // token is an opaque copied value.
                let status = unsafe { sakura_output_provider_stop_v1(token, &mut result) };
                assert!(matches!(
                    status,
                    SakuraOutputProviderStatus::Ok | SakuraOutputProviderStatus::InvalidHandle
                ));
            }));
        }

        {
            let barrier = Arc::clone(&barrier);
            handles.push(thread::spawn(move || {
                barrier.wait();
                let mut token_slot = token;
                // SAFETY: The token slot is local to this worker. Destruction
                // races are serialized by the provider registry.
                let status = unsafe { sakura_output_provider_destroy_v1(&mut token_slot) };
                assert_eq!(SakuraOutputProviderStatus::Ok, status);
                assert_eq!(0, token_slot);
            }));
        }

        for handle in handles {
            handle.join().expect("provider concurrent worker panicked");
        }

        let mut stale_slot = token;
        // SAFETY: The destroy worker consumed the provider token; this final
        // stale call confirms deterministic invalid-handle behavior.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_destroy_v1(&mut stale_slot)
        });
        assert_eq!(token, stale_slot);
    }

    #[test]
    fn provider_export_source_contract_contains_panic_containment_for_each_export() {
        let source = include_str!("output_provider.rs");
        assert_eq!(
            7,
            source
                .lines()
                .filter(|line| line
                    .trim_start()
                    .starts_with("catch_unwind(AssertUnwindSafe(||"))
                .count()
        );
        assert_eq!(
            7,
            source
                .matches("pub unsafe extern \"C\" fn sakura_output_provider_")
                .count()
        );
    }

    #[test]
    fn provider_token_family_and_terminal_snapshot_are_isolated() {
        let raw_limits = limits();
        let mut provider_token = 0_u64;
        // SAFETY: The limits and token are initialized caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_create_v1(&raw_limits, &mut provider_token)
        });
        assert_ne!(0, provider_token);
        assert_ne!(0, provider_token & (1_u64 << 63));

        let mut shadow_token = 0_u64;
        // SAFETY: The limits and token are initialized caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            output_shadow::model_create_v1(&raw_limits, &mut shadow_token)
        });
        assert_eq!(0, shadow_token & (1_u64 << 63));

        let operation_id = b"create";
        let owner_id = b"owner";
        let channel_id = b"channel";
        let label = b"Label";
        let request = create_request(operation_id, owner_id, channel_id, label);
        let mut result = SakuraOutputProviderApplyResultV1 {
            struct_size: size_of::<SakuraOutputProviderApplyResultV1>() as u32,
            abi_version: 1,
            status: 0,
            reason: 0,
            revision: 0,
            callback_drain_deferred: 0,
            reserved: [0; 7],
        };
        // The provider token must not be accepted by the replay-model family.
        // SAFETY: The request and result are valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_apply_v1(provider_token, &request, &mut result)
        });
        // SAFETY: The request and result are valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            sakura_output_provider_apply_v1(shadow_token, &request, &mut result)
        });
        // SAFETY: The request and result are valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(provider_token, &request, &mut result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            result.status
        );
        let revision = result.revision;

        let mut active = SakuraOutputProviderActiveChannelV1 {
            struct_size: size_of::<SakuraOutputProviderActiveChannelV1>() as u32,
            abi_version: 1,
            revision: 0,
            present: 0,
            reserved0: [0; 7],
            data: ptr::null_mut(),
            capacity: 0,
            length: 0,
            reserved: [0; 2],
        };
        // SAFETY: The descriptor is valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::InsufficientCapacity, unsafe {
            sakura_output_provider_active_channel_v1(provider_token, &mut active)
        });
        assert_eq!(revision, active.revision);
        assert_eq!(1, active.present);
        assert_eq!(channel_id.len() as u64, active.length);
        let mut active_bytes = vec![0_u8; active.length as usize];
        active.data = active_bytes.as_mut_ptr();
        active.capacity = active_bytes.len() as u64;
        // SAFETY: The descriptor and destination are valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_active_channel_v1(provider_token, &mut active)
        });
        assert_eq!(channel_id, active_bytes.as_slice());

        let mut stop_result = SakuraOutputProviderApplyResultV1 {
            struct_size: size_of::<SakuraOutputProviderApplyResultV1>() as u32,
            abi_version: 1,
            status: 0,
            reason: 0,
            revision: 0,
            callback_drain_deferred: 0,
            reserved: [0; 7],
        };
        // SAFETY: The result is valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_stop_v1(provider_token, &mut stop_result)
        });
        assert_eq!(
            SakuraOutputProviderOperationStatus::Succeeded as u32,
            stop_result.status
        );
        assert!(stop_result.revision > revision);

        let mut info = SakuraOutputProviderSnapshotInfoV1 {
            struct_size: size_of::<SakuraOutputProviderSnapshotInfoV1>() as u32,
            abi_version: 1,
            revision: 0,
            stopped: 0,
            active_channel_present: 0,
            reserved0: [0; 6],
            dropped_notification_count: 0,
            channel_count: 0,
            encoded_size: 0,
            reserved: [0; 2],
        };
        // SAFETY: The info is valid caller-owned V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(provider_token, &mut info)
        });
        assert_eq!(1, info.stopped);

        // SAFETY: Both token slots are valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut provider_token)
        });
        // SAFETY: Both token slots are valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            output_shadow::model_destroy_v1(&mut shadow_token)
        });
        assert_eq!(0, provider_token);
        assert_eq!(0, shadow_token);
    }

    #[test]
    fn provider_export_source_contract_is_provider_only() {
        let source = include_str!("output_provider.rs");
        let mut exports = Vec::new();
        for line in source.lines() {
            if let Some(rest) = line.trim().strip_prefix("pub unsafe extern \"C\" fn ") {
                exports.push(rest.split('(').next().unwrap_or_default());
            }
        }
        assert_eq!(7, exports.len());
        assert!(exports
            .iter()
            .all(|name| name.starts_with("sakura_output_provider_")));
        let forbidden = ["sakura_output_", "shadow_"].concat();
        assert!(!source.contains(&forbidden));
    }
}
