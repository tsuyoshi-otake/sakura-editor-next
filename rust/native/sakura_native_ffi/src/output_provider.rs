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
