//! Sole-authority OutputService provider exports.
//!
//! Requests and snapshots cross the ABI as bounded copied spans.  The Rust
//! model retains only owned values.  Provider tokens use a separate tagged
//! registry from the migration shadow so the two opaque-handle families can
//! never be confused.

#![allow(dead_code)]

use std::collections::BTreeMap;
use std::mem::{align_of, offset_of, size_of};
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
pub type SakuraOutputProviderActiveChannelV1 = output_shadow::SakuraOutputShadowActiveChannelV1;

const PROVIDER_TOKEN_TAG: u64 = 1_u64 << 63;
const MAX_SNAPSHOT_MEASUREMENTS: usize = 64;

// These points are used only by the in-module adversarial tests below.  The
// non-test implementation of `maybe_test_panic` is an inlinable no-op, so the
// production ABI has no externally reachable panic-injection hook.
const EXPORT_PANIC_CREATE: u8 = 1;
const EXPORT_PANIC_APPLY: u8 = 2;
const EXPORT_PANIC_SNAPSHOT_MEASURE: u8 = 3;
const EXPORT_PANIC_SNAPSHOT_WRITE: u8 = 4;
const EXPORT_PANIC_ACTIVE_CHANNEL: u8 = 5;
const EXPORT_PANIC_STOP: u8 = 6;
const EXPORT_PANIC_DESTROY: u8 = 7;

#[cfg(test)]
use std::cell::Cell;

#[cfg(test)]
thread_local! {
    static FORCED_EXPORT_PANIC: Cell<u8> = const { Cell::new(0) };
}

#[cfg(test)]
#[inline(never)]
fn maybe_test_panic(point: u8) {
    if FORCED_EXPORT_PANIC.with(Cell::get) == point {
        panic!("forced provider export panic");
    }
}

#[cfg(not(test))]
#[inline(always)]
fn maybe_test_panic(_point: u8) {}

/// Fixed-width identity captured by a successful snapshot measure.
///
/// The receipt is copied by the caller from the measure result into the write
/// descriptor.  It contains no pointer and is valid only for the provider
/// token that issued it.  The provider's accepted-mutation invariant advances
/// revision for every state change; the fixed-width metadata also fences the
/// encoded framing and advisory drop counter without retaining snapshot bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SakuraOutputProviderSnapshotReceiptV1 {
    pub measurement_id: u64,
    pub revision: u64,
    pub dropped_notification_count: u64,
    pub channel_count: u64,
    pub encoded_size: u64,
    pub stopped: u8,
    pub active_channel_present: u8,
    pub reserved: [u8; 6],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputProviderSnapshotInfoV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub revision: u64,
    pub stopped: u8,
    pub active_channel_present: u8,
    pub reserved0: [u8; 6],
    pub dropped_notification_count: u64,
    pub channel_count: u64,
    pub encoded_size: u64,
    pub reserved: [u64; 2],
    pub receipt: SakuraOutputProviderSnapshotReceiptV1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputProviderSnapshotBufferV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *mut u8,
    pub capacity: u64,
    pub length: u64,
    pub reserved: [u64; 2],
    pub receipt: SakuraOutputProviderSnapshotReceiptV1,
}

type SnapshotMeasureReceiptV1 = SakuraOutputProviderSnapshotReceiptV1;

#[derive(Clone, Copy)]
struct SnapshotMeasurement {
    provider_token: u64,
    receipt: SnapshotMeasureReceiptV1,
}

// ABI V1 is frozen at every field boundary. An incompatible change must use
// a new ABI version and export family rather than updating these assertions.
macro_rules! assert_abi_layout {
    ($type:ty, $size:expr, $alignment:expr, { $($field:ident: $offset:expr),+ $(,)? }) => {
        const _: () = {
            assert!(size_of::<$type>() == $size);
            assert!(align_of::<$type>() == $alignment);
            $(assert!(offset_of!($type, $field) == $offset);)+
        };
    };
}

const _: () = {
    assert!(size_of::<SakuraOutputProviderStatus>() == 4);
    assert!(size_of::<SakuraOutputProviderOperationStatus>() == 4);
    assert!(size_of::<SakuraOutputProviderReason>() == 4);
};

assert_abi_layout!(SakuraOutputProviderSpanV1, 40, 8, {
    struct_size: 0,
    abi_version: 4,
    data: 8,
    length: 16,
    reserved: 24,
});
assert_abi_layout!(SakuraOutputProviderLimitsV1, 80, 8, {
    struct_size: 0,
    abi_version: 4,
    maximum_owners: 8,
    maximum_channels: 16,
    maximum_text_bytes_per_channel: 24,
    maximum_payload_bytes: 32,
    maximum_log_entries_per_channel: 40,
    maximum_remembered_operations: 48,
    reserved: 56,
});
assert_abi_layout!(SakuraOutputProviderLogEntryV1, 112, 8, {
    struct_size: 0,
    abi_version: 4,
    level: 8,
    flags: 12,
    message: 16,
    source: 56,
    reserved: 96,
});
assert_abi_layout!(SakuraOutputProviderRequestV1, 368, 8, {
    struct_size: 0,
    abi_version: 4,
    operation_kind: 8,
    channel_kind: 12,
    flags: 16,
    operation_id: 24,
    expected_revision: 64,
    owner_id: 72,
    owner_generation: 112,
    channel_id: 120,
    label: 160,
    metadata_language_id: 200,
    metadata_source: 240,
    payload: 280,
    log_entries: 320,
    log_entry_count: 328,
    reserved: 336,
});
assert_abi_layout!(SakuraOutputProviderApplyResultV1, 32, 8, {
    struct_size: 0,
    abi_version: 4,
    status: 8,
    reason: 12,
    revision: 16,
    callback_drain_deferred: 24,
    reserved: 25,
});
assert_abi_layout!(SakuraOutputProviderSnapshotReceiptV1, 48, 8, {
    measurement_id: 0,
    revision: 8,
    dropped_notification_count: 16,
    channel_count: 24,
    encoded_size: 32,
    stopped: 40,
    active_channel_present: 41,
    reserved: 42,
});
assert_abi_layout!(SakuraOutputProviderSnapshotInfoV1, 112, 8, {
    struct_size: 0,
    abi_version: 4,
    revision: 8,
    stopped: 16,
    active_channel_present: 17,
    reserved0: 18,
    dropped_notification_count: 24,
    channel_count: 32,
    encoded_size: 40,
    reserved: 48,
    receipt: 64,
});
assert_abi_layout!(SakuraOutputProviderSnapshotBufferV1, 96, 8, {
    struct_size: 0,
    abi_version: 4,
    data: 8,
    capacity: 16,
    length: 24,
    reserved: 32,
    receipt: 48,
});
assert_abi_layout!(SakuraOutputProviderActiveChannelV1, 64, 8, {
    struct_size: 0,
    abi_version: 4,
    revision: 8,
    present: 16,
    reserved0: 17,
    data: 24,
    capacity: 32,
    length: 40,
    reserved: 48,
});

struct ProviderRegistry {
    next_token: u64,
    providers: BTreeMap<u64, u64>,
    next_measurement_id: u64,
    snapshot_measurements: BTreeMap<u64, SnapshotMeasurement>,
}

static PROVIDERS: OnceLock<Mutex<ProviderRegistry>> = OnceLock::new();

fn providers() -> &'static Mutex<ProviderRegistry> {
    PROVIDERS.get_or_init(|| {
        Mutex::new(ProviderRegistry {
            next_token: PROVIDER_TOKEN_TAG | 1,
            providers: BTreeMap::new(),
            next_measurement_id: 1,
            snapshot_measurements: BTreeMap::new(),
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
        receipt: poison_receipt(),
    }
}

fn poison_receipt() -> SnapshotMeasureReceiptV1 {
    SnapshotMeasureReceiptV1 {
        measurement_id: u64::MAX,
        revision: u64::MAX,
        dropped_notification_count: u64::MAX,
        channel_count: u64::MAX,
        encoded_size: u64::MAX,
        stopped: 0xff,
        active_channel_present: 0xff,
        reserved: [0; 6],
    }
}

fn snapshot_receipt_is_valid(receipt: &SnapshotMeasureReceiptV1) -> bool {
    receipt.measurement_id != 0
        && receipt.stopped <= 1
        && receipt.active_channel_present <= 1
        && receipt.reserved == [0; 6]
}

fn shadow_snapshot_info_is_valid(info: &output_shadow::SakuraOutputShadowSnapshotInfoV1) -> bool {
    info.struct_size == size_of::<output_shadow::SakuraOutputShadowSnapshotInfoV1>() as u32
        && info.abi_version == 1
        && info.stopped <= 1
        && info.active_channel_present <= 1
        && info.reserved0 == [0; 6]
        && info.reserved == [0; 2]
}

fn provider_snapshot_info_is_valid(info: &SakuraOutputProviderSnapshotInfoV1) -> bool {
    info.struct_size == size_of::<SakuraOutputProviderSnapshotInfoV1>() as u32
        && info.abi_version == 1
        && info.stopped <= 1
        && info.active_channel_present <= 1
        && info.reserved0 == [0; 6]
        && info.reserved == [0; 2]
        && snapshot_receipt_is_valid(&info.receipt)
        && info.receipt.revision == info.revision
        && info.receipt.stopped == info.stopped
        && info.receipt.active_channel_present == info.active_channel_present
        && info.receipt.dropped_notification_count == info.dropped_notification_count
        && info.receipt.channel_count == info.channel_count
        && info.receipt.encoded_size == info.encoded_size
}

fn snapshot_receipt_from_model_info(
    measurement_id: u64,
    info: &output_shadow::SakuraOutputShadowSnapshotInfoV1,
) -> Option<SnapshotMeasureReceiptV1> {
    if measurement_id == 0 || !shadow_snapshot_info_is_valid(info) {
        return None;
    }
    Some(SnapshotMeasureReceiptV1 {
        measurement_id,
        revision: info.revision,
        dropped_notification_count: info.dropped_notification_count,
        channel_count: info.channel_count,
        encoded_size: info.encoded_size,
        stopped: info.stopped,
        active_channel_present: info.active_channel_present,
        reserved: [0; 6],
    })
}

fn shadow_poison_info() -> output_shadow::SakuraOutputShadowSnapshotInfoV1 {
    output_shadow::SakuraOutputShadowSnapshotInfoV1 {
        struct_size: size_of::<output_shadow::SakuraOutputShadowSnapshotInfoV1>() as u32,
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

fn capture_model_snapshot_measure(
    shadow_token: u64,
) -> Result<output_shadow::SakuraOutputShadowSnapshotInfoV1, SakuraOutputProviderStatus> {
    let mut info = shadow_poison_info();
    // SAFETY: `info` is local, initialized V1 storage and remains alive for
    // the duration of the model call.
    let measured = unsafe { output_shadow::model_snapshot_measure_v1(shadow_token, &mut info) };
    if measured != SakuraOutputProviderStatus::Ok {
        return Err(measured);
    }
    if !shadow_snapshot_info_is_valid(&info) {
        return Err(SakuraOutputProviderStatus::InternalError);
    }
    Ok(info)
}

fn next_measurement_id(registry: &mut ProviderRegistry) -> Option<u64> {
    let measurement_id = registry.next_measurement_id;
    if measurement_id == 0 {
        return None;
    }
    registry.next_measurement_id = measurement_id.checked_add(1).unwrap_or(0);
    Some(measurement_id)
}

fn retain_measurement(
    registry: &mut ProviderRegistry,
    measurement_id: u64,
    measurement: SnapshotMeasurement,
) {
    while registry.snapshot_measurements.len() >= MAX_SNAPSHOT_MEASUREMENTS {
        let Some(oldest) = registry.snapshot_measurements.keys().next().copied() else {
            break;
        };
        registry.snapshot_measurements.remove(&oldest);
    }
    registry
        .snapshot_measurements
        .insert(measurement_id, measurement);
}

fn remove_measurements_for_provider(registry: &mut ProviderRegistry, provider_token: u64) {
    registry
        .snapshot_measurements
        .retain(|_, measurement| measurement.provider_token != provider_token);
}

fn ranges_overlap(
    first: *const u8,
    first_length: usize,
    second: *const u8,
    second_length: usize,
) -> bool {
    if first_length == 0 || second_length == 0 {
        return false;
    }
    let Some(first_end) = (first as usize).checked_add(first_length) else {
        return true;
    };
    let Some(second_end) = (second as usize).checked_add(second_length) else {
        return true;
    };
    (first as usize) < second_end && (second as usize) < first_end
}

fn validate_snapshot_buffer_descriptor(
    buffer: *mut SakuraOutputProviderSnapshotBufferV1,
    descriptor: &SakuraOutputProviderSnapshotBufferV1,
) -> Result<usize, SakuraOutputProviderStatus> {
    if descriptor.struct_size != size_of::<SakuraOutputProviderSnapshotBufferV1>() as u32
        || descriptor.abi_version != 1
        || descriptor.reserved != [0; 2]
        || !snapshot_receipt_is_valid(&descriptor.receipt)
    {
        return Err(SakuraOutputProviderStatus::InvalidArgument);
    }
    let capacity = usize::try_from(descriptor.capacity)
        .map_err(|_| SakuraOutputProviderStatus::InvalidArgument)?;
    if capacity != 0
        && (descriptor.data.is_null()
            || !is_aligned(descriptor.data)
            || capacity > isize::MAX as usize
            || (descriptor.data as usize)
                .checked_add(capacity)
                .is_none_or(|end| end > isize::MAX as usize))
    {
        return Err(SakuraOutputProviderStatus::InvalidArgument);
    }
    if ranges_overlap(
        buffer.cast_const().cast(),
        size_of::<SakuraOutputProviderSnapshotBufferV1>(),
        descriptor.data.cast_const(),
        capacity,
    ) {
        return Err(SakuraOutputProviderStatus::InvalidArgument);
    }
    Ok(capacity)
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

fn poison_result_if_valid(result: *mut SakuraOutputProviderApplyResultV1) {
    if is_valid_pointer(result.cast_const()) {
        // SAFETY: The address/range check above proves that the caller-owned
        // result slot can hold one complete V1 result for this call.
        unsafe { result.write(poison_result()) };
    }
}

fn poison_info_if_valid(info: *mut SakuraOutputProviderSnapshotInfoV1) {
    if is_valid_pointer(info.cast_const()) {
        // SAFETY: The address/range check above proves that the caller-owned
        // info slot can hold one complete V1 descriptor for this call.
        unsafe { info.write(poison_info()) };
    }
}

fn poison_snapshot_length_if_valid(buffer: *mut SakuraOutputProviderSnapshotBufferV1) {
    if is_valid_pointer(buffer.cast_const()) {
        // SAFETY: The address/range check above proves that the caller-owned
        // descriptor has a writable length field for this call.
        unsafe { (*buffer).length = u64::MAX };
    }
}

fn poison_active_if_valid(active: *mut SakuraOutputProviderActiveChannelV1) {
    if is_valid_pointer(active.cast_const()) {
        // SAFETY: The address/range check above proves that the caller-owned
        // active-channel descriptor can hold one complete V1 result.
        unsafe { active.write(poison_active()) };
    }
}

fn zero_token_if_valid(token: *mut u64) {
    if is_valid_pointer(token.cast_const()) {
        // SAFETY: The address/range check above proves that the caller-owned
        // token slot is writable for this call.
        unsafe { token.write(0) };
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
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The token slot is validated and caller-owned for this call.
        unsafe { token.write(0) };
        maybe_test_panic(EXPORT_PANIC_CREATE);
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
    })) {
        Ok(status) => status,
        Err(_) => {
            // A provider token is published only after the complete create
            // transaction succeeds.  Keep the caller's token unambiguously
            // empty if any fallible work panics before that publication.
            zero_token_if_valid(token);
            SakuraOutputProviderStatus::InternalError
        }
    }
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
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(result.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The result slot is validated and caller-owned for this call.
        unsafe { result.write(poison_result()) };
        maybe_test_panic(EXPORT_PANIC_APPLY);
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export validates all request/span/range fields
        // and copies every input before changing the Rust-owned model.
        unsafe { output_shadow::model_apply_v1(shadow_token, request.cast(), result.cast()) }
    })) {
        Ok(status) => status,
        Err(_) => {
            // The model normally poisons rejected results itself.  Restore
            // that boundary sentinel if a provider-side allocation, lock, or
            // future model change panics after a partial result write.
            poison_result_if_valid(result);
            SakuraOutputProviderStatus::InternalError
        }
    }
}

/// Measures a copied canonical provider snapshot and records its fixed-width
/// identity receipt in the provider registry.  Every successful measure gets
/// a distinct measurement id, so concurrent callers can retain independent
/// receipts without a last-measure race.  The receipt contains no pointer.
///
/// # Safety
///
/// `info` must point to writable V1 storage for this call only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_snapshot_measure_v1(
    token: u64,
    info: *mut SakuraOutputProviderSnapshotInfoV1,
) -> SakuraOutputProviderStatus {
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(info.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The info slot is validated and caller-owned for this call.
        unsafe { info.write(poison_info()) };
        maybe_test_panic(EXPORT_PANIC_SNAPSHOT_MEASURE);
        let mut registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        let model_info = match capture_model_snapshot_measure(shadow_token) {
            Ok(value) => value,
            Err(status) => {
                // Keep the provider boundary fail-closed even if a future
                // model implementation returns an error without poisoning
                // its output.
                poison_info_if_valid(info);
                return status;
            }
        };
        let Some(measurement_id) = next_measurement_id(&mut registry) else {
            poison_info_if_valid(info);
            return SakuraOutputProviderStatus::InternalError;
        };
        let Some(receipt) = snapshot_receipt_from_model_info(measurement_id, &model_info) else {
            poison_info_if_valid(info);
            return SakuraOutputProviderStatus::InternalError;
        };
        let provider_info = SakuraOutputProviderSnapshotInfoV1 {
            struct_size: size_of::<SakuraOutputProviderSnapshotInfoV1>() as u32,
            abi_version: 1,
            revision: model_info.revision,
            stopped: model_info.stopped,
            active_channel_present: model_info.active_channel_present,
            reserved0: [0; 6],
            dropped_notification_count: model_info.dropped_notification_count,
            channel_count: model_info.channel_count,
            encoded_size: model_info.encoded_size,
            reserved: [0; 2],
            receipt,
        };
        if !provider_snapshot_info_is_valid(&provider_info) {
            poison_info_if_valid(info);
            return SakuraOutputProviderStatus::InternalError;
        }
        // SAFETY: `info` is validated caller-owned storage and the value is a
        // complete copied descriptor with no retained foreign pointer.
        unsafe { info.write(provider_info) };
        retain_measurement(
            &mut registry,
            measurement_id,
            SnapshotMeasurement {
                provider_token: token,
                receipt,
            },
        );
        SakuraOutputProviderStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => {
            // SAFETY: `is_valid_pointer` only inspects the address range.  If
            // the caller supplied writable V1 storage, restore its poison
            // value after any allocation/model panic.
            poison_info_if_valid(info);
            SakuraOutputProviderStatus::InternalError
        }
    }
}

/// Writes the canonical provider snapshot identified by the caller-copied
/// receipt in the buffer.  The current snapshot must match that receipt
/// before any caller-owned bytes are copied; an intervening mutation therefore
/// fails closed even when it happens to preserve encoded_size.
///
/// # Safety
///
/// `buffer` and its destination span follow the V1 copied output contract.
/// `length` is the only output field. On every rejected or panicking call it
/// is set to `u64::MAX`; the input descriptor fields (`data`, `capacity`, and
/// `receipt`) remain untouched so the caller can diagnose or retry the same
/// request. Destination bytes are never considered valid unless the returned
/// status is `Ok`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_provider_snapshot_write_v1(
    token: u64,
    buffer: *mut SakuraOutputProviderSnapshotBufferV1,
) -> SakuraOutputProviderStatus {
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(buffer.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The descriptor slot is validated and caller-owned.
        poison_snapshot_length_if_valid(buffer);
        maybe_test_panic(EXPORT_PANIC_SNAPSHOT_WRITE);
        let mut registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The descriptor pointer was validated above and remains
        // caller-owned for this call. Copy it before taking any destination
        // action; no caller pointer is retained.
        let descriptor = unsafe { buffer.read() };
        let capacity = match validate_snapshot_buffer_descriptor(buffer, &descriptor) {
            Ok(value) => value,
            Err(status) => return status,
        };
        let measurement_id = descriptor.receipt.measurement_id;
        let Some(measured) = registry.snapshot_measurements.get(&measurement_id).copied() else {
            // A write without a successful preceding measure has no identity
            // fence and must never copy an arbitrary current snapshot.
            return SakuraOutputProviderStatus::InvalidArgument;
        };
        if measured.provider_token != token || measured.receipt != descriptor.receipt {
            // Keep a valid stored receipt available if a caller supplied a
            // forged copy. A caller cannot use another provider's receipt.
            return SakuraOutputProviderStatus::InvalidArgument;
        }

        let model_info = match capture_model_snapshot_measure(shadow_token) {
            Ok(value) => value,
            Err(status) => return status,
        };
        let Some(current) = snapshot_receipt_from_model_info(measurement_id, &model_info) else {
            registry.snapshot_measurements.remove(&measurement_id);
            return SakuraOutputProviderStatus::InternalError;
        };
        if current != measured.receipt {
            // Consume a stale receipt. The caller must perform a fresh measure
            // before it can attempt another write. This comparison includes
            // the monotonic revision, drop counter, framing metadata, and
            // stopped/active state.
            registry.snapshot_measurements.remove(&measurement_id);
            return SakuraOutputProviderStatus::InternalError;
        }
        if u64::try_from(capacity).unwrap_or(u64::MAX) < model_info.encoded_size {
            // Retain a valid receipt so the caller can retry with a larger
            // destination without another measure transaction.
            return SakuraOutputProviderStatus::InsufficientCapacity;
        }
        let mut model_buffer = output_shadow::SakuraOutputShadowSnapshotBufferV1 {
            struct_size: size_of::<output_shadow::SakuraOutputShadowSnapshotBufferV1>() as u32,
            abi_version: 1,
            data: descriptor.data,
            capacity: descriptor.capacity,
            length: 0,
            reserved: [0; 2],
        };
        // SAFETY: The provider descriptor was validated above, and the model
        // buffer borrows its caller-owned destination only for this call.  No
        // pointer is retained by either layer.
        let written =
            unsafe { output_shadow::model_snapshot_write_v1(shadow_token, &mut model_buffer) };
        if written != SakuraOutputProviderStatus::Ok
            || model_buffer.struct_size
                != size_of::<output_shadow::SakuraOutputShadowSnapshotBufferV1>() as u32
            || model_buffer.abi_version != 1
            || model_buffer.data != descriptor.data
            || model_buffer.capacity != descriptor.capacity
            || model_buffer.reserved != [0; 2]
            || model_buffer.length != model_info.encoded_size
        {
            if written != SakuraOutputProviderStatus::Ok {
                return written;
            }
            registry.snapshot_measurements.remove(&measurement_id);
            return SakuraOutputProviderStatus::InternalError;
        }
        // SAFETY: The descriptor pointer was validated and remains caller-
        // owned. Set length only after the model copied the full snapshot.
        unsafe { (*buffer).length = model_buffer.length };
        // Keep the immutable receipt available for another destination or a
        // retry. It is bounded by MAX_SNAPSHOT_MEASUREMENTS and is cleared
        // when the provider is destroyed.
        SakuraOutputProviderStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => {
            // SAFETY: `is_valid_pointer` only inspects the address range.  If
            // the descriptor was valid, restore its poison length after any
            // allocation/model panic and never expose a partial success.
            poison_snapshot_length_if_valid(buffer);
            SakuraOutputProviderStatus::InternalError
        }
    }
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
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(active.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        maybe_test_panic(EXPORT_PANIC_ACTIVE_CHANNEL);
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
    })) {
        Ok(status) => status,
        Err(_) => {
            // The model wrapper owns the normal descriptor poisoning path;
            // this outer boundary also covers panics before it is entered.
            poison_active_if_valid(active);
            SakuraOutputProviderStatus::InternalError
        }
    }
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
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(result.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        // SAFETY: The result slot is validated and caller-owned.
        unsafe { result.write(poison_result()) };
        maybe_test_panic(EXPORT_PANIC_STOP);
        let registry = lock_providers();
        let Some(&shadow_token) = registry.providers.get(&token) else {
            return SakuraOutputProviderStatus::InvalidHandle;
        };
        // SAFETY: The wrapped export performs terminal-state handling.
        unsafe { output_shadow::model_stop_v1(shadow_token, result.cast()) }
    })) {
        Ok(status) => status,
        Err(_) => {
            // Stop publishes a complete operation result only on success.
            // Restore the poison result if a lock/model operation panics.
            poison_result_if_valid(result);
            SakuraOutputProviderStatus::InternalError
        }
    }
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
    match catch_unwind(AssertUnwindSafe(|| {
        if !is_valid_pointer(token.cast_const()) {
            return SakuraOutputProviderStatus::InvalidArgument;
        }
        maybe_test_panic(EXPORT_PANIC_DESTROY);
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
        remove_measurements_for_provider(&mut registry, value);
        // SAFETY: Consume the caller token only after successful destruction.
        unsafe { token.write(0) };
        SakuraOutputProviderStatus::Ok
    })) {
        Ok(status) => status,
        Err(_) => {
            // Destruction consumes the caller token only after success.  A
            // panic therefore leaves the original opaque value untouched.
            SakuraOutputProviderStatus::InternalError
        }
    }
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

    fn text_request(
        operation_id: &[u8],
        channel_id: &[u8],
        payload: &[u8],
        operation_kind: u32,
    ) -> SakuraOutputProviderRequestV1 {
        let mut request = empty_request(operation_id, operation_kind);
        request.channel_id = span(channel_id);
        request.payload = span(payload);
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

    fn empty_receipt() -> SakuraOutputProviderSnapshotReceiptV1 {
        SakuraOutputProviderSnapshotReceiptV1 {
            measurement_id: 0,
            revision: 0,
            dropped_notification_count: 0,
            channel_count: 0,
            encoded_size: 0,
            stopped: 0,
            active_channel_present: 0,
            reserved: [0; 6],
        }
    }

    fn snapshot_buffer_with_receipt(
        storage: &mut [u8],
        receipt: SakuraOutputProviderSnapshotReceiptV1,
    ) -> SakuraOutputProviderSnapshotBufferV1 {
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
            receipt,
        }
    }

    fn snapshot_buffer(storage: &mut [u8]) -> SakuraOutputProviderSnapshotBufferV1 {
        snapshot_buffer_with_receipt(storage, empty_receipt())
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

    fn with_export_panic<T>(point: u8, operation: impl FnOnce() -> T) -> T {
        FORCED_EXPORT_PANIC.with(|forced| {
            let previous = forced.replace(point);
            let result = operation();
            forced.set(previous);
            result
        })
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
        assert_eq!(poison_receipt(), info.receipt);
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
    fn provider_snapshot_write_rejects_unmeasured_and_same_size_mutations() {
        let token = create_provider_token();
        let create = create_request(b"receipt-create", b"owner", b"channel", b"Label");
        let mut result = poison_result();
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &create, &mut result)
        });

        let mut unmeasured_storage = vec![0xa5_u8; 256];
        let mut unmeasured = snapshot_buffer(&mut unmeasured_storage);
        // A write without a preceding measure has no receipt and must not
        // expose the current model state.
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut unmeasured)
        });
        assert_poison_snapshot_length(&unmeasured);
        assert!(unmeasured_storage.iter().all(|value| *value == 0xa5));

        let append = text_request(b"receipt-append", b"channel", b"AAAA", 2);
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &append, &mut result)
        });

        // Caller A measures the first four-byte payload.
        let mut info_a = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut info_a)
        });
        assert_ne!(0, info_a.receipt.measurement_id);

        let replace = text_request(b"receipt-replace", b"channel", b"BBBB", 3);
        // SAFETY: The request and result satisfy the V1 copied ABI contract.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_apply_v1(token, &replace, &mut result)
        });

        // Caller B measures after a same-size mutation. A and B therefore
        // have different receipts even though encoded_size is identical.
        let mut info_b = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut info_b)
        });
        assert_ne!(info_a.receipt.measurement_id, info_b.receipt.measurement_id);
        assert_eq!(info_a.encoded_size, info_b.encoded_size);
        assert_ne!(info_a.receipt.revision, info_b.receipt.revision);

        let mut a_storage = vec![0xa5_u8; info_a.encoded_size as usize];
        let mut a_buffer = snapshot_buffer_with_receipt(&mut a_storage, info_a.receipt);
        // A's caller-bound receipt must reject the current B snapshot; it may
        // not silently use B's last measurement or emit B's bytes.
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InternalError, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut a_buffer)
        });
        assert_poison_snapshot_length(&a_buffer);
        assert!(a_storage.iter().all(|value| *value == 0xa5));

        // A stale receipt is terminal for that measurement.  Reusing it
        // after the mismatch must not expose the now-current snapshot.
        let mut expired_storage = vec![0xa5_u8; info_a.encoded_size as usize];
        let mut expired_buffer = snapshot_buffer_with_receipt(&mut expired_storage, info_a.receipt);
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut expired_buffer)
        });
        assert_poison_snapshot_length(&expired_buffer);
        assert!(expired_storage.iter().all(|value| *value == 0xa5));

        // Altering any receipt field is rejected without consuming the valid
        // B entry, so a forged copy cannot turn into a partial write.
        let mut forged_receipt = info_b.receipt;
        forged_receipt.channel_count ^= 1;
        let mut forged_storage = vec![0xa5_u8; info_b.encoded_size as usize];
        let mut forged_buffer = snapshot_buffer_with_receipt(&mut forged_storage, forged_receipt);
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut forged_buffer)
        });
        assert_poison_snapshot_length(&forged_buffer);
        assert!(forged_storage.iter().all(|value| *value == 0xa5));

        // A receipt issued by one provider token cannot be replayed through a
        // different live provider, even though the global measurement id is
        // otherwise well formed.
        let other_token = create_provider_token();
        let mut cross_storage = vec![0xa5_u8; info_b.encoded_size as usize];
        let mut cross_buffer = snapshot_buffer_with_receipt(&mut cross_storage, info_b.receipt);
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(other_token, &mut cross_buffer)
        });
        assert_poison_snapshot_length(&cross_buffer);
        assert!(cross_storage.iter().all(|value| *value == 0xa5));
        let mut other_token = other_token;
        // SAFETY: The second token was created by this test and is destroyed
        // exactly once after the cross-provider receipt check.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut other_token)
        });
        assert_eq!(0, other_token);

        let mut b_storage = vec![0xa5_u8; info_b.encoded_size as usize];
        let mut b_buffer = snapshot_buffer_with_receipt(&mut b_storage, info_b.receipt);
        // B's receipt identifies the current snapshot and is accepted.
        // SAFETY: The descriptor and storage exactly match B's measure.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut b_buffer)
        });
        assert_eq!(info_b.encoded_size, b_buffer.length);
        assert!(b_storage
            .windows(b"BBBB".len())
            .any(|window| window == b"BBBB"));

        // The receipt registry is deliberately bounded.  Keep one receipt
        // outstanding, then add 64 newer measurements so the oldest entry is
        // evicted instead of being silently rebound to a newer snapshot.
        let mut oldest_info = poison_info();
        // SAFETY: The info slot is writable local V1 storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut oldest_info)
        });
        for _ in 0..64 {
            let mut newer_info = poison_info();
            // SAFETY: Each info slot is writable local V1 storage.
            assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
                sakura_output_provider_snapshot_measure_v1(token, &mut newer_info)
            });
        }
        let mut evicted_storage = vec![0xa5_u8; oldest_info.encoded_size as usize];
        let mut evicted_buffer =
            snapshot_buffer_with_receipt(&mut evicted_storage, oldest_info.receipt);
        // SAFETY: The descriptor and storage are caller-owned and bounded.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut evicted_buffer)
        });
        assert_poison_snapshot_length(&evicted_buffer);
        assert!(evicted_storage.iter().all(|value| *value == 0xa5));

        // SAFETY: The token slot is valid caller-owned storage.
        let mut token = token;
        // SAFETY: The token slot remains valid caller-owned storage.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
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
        let mut first_buffer = snapshot_buffer_with_receipt(&mut first_storage, info.receipt);
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
        let mut second_buffer = snapshot_buffer_with_receipt(&mut second_storage, info.receipt);
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

        let mut nonzero_receipt_reserved = snapshot_buffer(&mut storage);
        nonzero_receipt_reserved.receipt.reserved[0] = 1;
        // SAFETY: The destination receipt has nonzero reserved bytes.
        assert_eq!(SakuraOutputProviderStatus::InvalidArgument, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut nonzero_receipt_reserved)
        });
        assert_poison_snapshot_length(&nonzero_receipt_reserved);

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
        let mut old_buffer = snapshot_buffer_with_receipt(&mut old_storage, new_info.receipt);
        // SAFETY: The destination is valid but intentionally smaller than the
        // newly measured canonical stream.
        assert_eq!(SakuraOutputProviderStatus::InsufficientCapacity, unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut old_buffer)
        });
        assert_poison_snapshot_length(&old_buffer);

        let mut final_storage = vec![0_u8; new_info.encoded_size as usize];
        let mut final_buffer = snapshot_buffer_with_receipt(&mut final_storage, new_info.receipt);
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

        let mut model_info = shadow_poison_info();
        // SAFETY: A provider token must not be accepted by replay snapshot
        // measurement.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_snapshot_measure_v1(
                provider_token,
                &mut model_info as *mut output_shadow::SakuraOutputShadowSnapshotInfoV1,
            )
        });
        assert_eq!(u64::MAX, model_info.revision);

        let mut model_storage = vec![0_u8; 128];
        let mut model_buffer = output_shadow::SakuraOutputShadowSnapshotBufferV1 {
            struct_size: size_of::<output_shadow::SakuraOutputShadowSnapshotBufferV1>() as u32,
            abi_version: 1,
            data: model_storage.as_mut_ptr(),
            capacity: model_storage.len() as u64,
            length: 0,
            reserved: [0; 2],
        };
        // SAFETY: A provider token must not be accepted by replay snapshot
        // writing.
        assert_eq!(SakuraOutputProviderStatus::InvalidHandle, unsafe {
            output_shadow::model_snapshot_write_v1(
                provider_token,
                &mut model_buffer as *mut output_shadow::SakuraOutputShadowSnapshotBufferV1,
            )
        });
        assert_eq!(u64::MAX, model_buffer.length);

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
                        | SakuraOutputProviderStatus::InvalidArgument
                        | SakuraOutputProviderStatus::InternalError
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
    fn every_provider_export_recovers_from_its_own_panic_boundary() {
        let raw_limits = limits();
        let mut create_slot = u64::MAX;
        // SAFETY: The limits and token are valid caller-owned storage. The
        // test-only hook panics after the token is initialized but before the
        // create transaction can publish a provider token.
        let create_status = with_export_panic(EXPORT_PANIC_CREATE, || unsafe {
            sakura_output_provider_create_v1(&raw_limits, &mut create_slot)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, create_status);
        assert_eq!(0, create_slot);

        let token = create_provider_token();
        let request = create_request(b"panic-boundaries", b"owner", b"channel", b"Label");

        let mut apply_result = SakuraOutputProviderApplyResultV1 {
            struct_size: 0,
            abi_version: 0,
            status: 0,
            reason: 0,
            revision: 0,
            callback_drain_deferred: 0,
            reserved: [0; 7],
        };
        // SAFETY: The request and result are valid caller-owned storage. The
        // panic occurs after the export writes its initial poison result.
        let apply_status = with_export_panic(EXPORT_PANIC_APPLY, || unsafe {
            sakura_output_provider_apply_v1(token, &request, &mut apply_result)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, apply_status);
        assert_poison_result(&apply_result);

        let mut info = SakuraOutputProviderSnapshotInfoV1 {
            struct_size: 0,
            abi_version: 0,
            revision: 0,
            stopped: 0,
            active_channel_present: 0,
            reserved0: [0; 6],
            dropped_notification_count: 0,
            channel_count: 0,
            encoded_size: 0,
            reserved: [0; 2],
            receipt: empty_receipt(),
        };
        // SAFETY: The info descriptor is valid caller-owned storage. The
        // panic occurs after the export writes its initial poison descriptor.
        let measure_status = with_export_panic(EXPORT_PANIC_SNAPSHOT_MEASURE, || unsafe {
            sakura_output_provider_snapshot_measure_v1(token, &mut info)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, measure_status);
        assert_poison_info(&info);

        let mut snapshot_storage = [0xa5_u8; 8];
        let mut snapshot_buffer = snapshot_buffer(&mut snapshot_storage);
        snapshot_buffer.length = 17;
        let snapshot_data = snapshot_buffer.data;
        let snapshot_capacity = snapshot_buffer.capacity;
        let snapshot_receipt = snapshot_buffer.receipt;
        // SAFETY: The descriptor is valid caller-owned storage. The panic is
        // injected after the length-only poison is published and before the
        // descriptor is read for validation.
        let write_status = with_export_panic(EXPORT_PANIC_SNAPSHOT_WRITE, || unsafe {
            sakura_output_provider_snapshot_write_v1(token, &mut snapshot_buffer)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, write_status);
        assert_poison_snapshot_length(&snapshot_buffer);
        assert_eq!(snapshot_data, snapshot_buffer.data);
        assert_eq!(snapshot_capacity, snapshot_buffer.capacity);
        assert_eq!(snapshot_receipt, snapshot_buffer.receipt);
        assert_eq!([0xa5_u8; 8], snapshot_storage);

        let mut active_storage = [0_u8; 1];
        let mut active = active_buffer(&mut active_storage);
        // SAFETY: The descriptor is valid caller-owned storage. The panic is
        // injected before the wrapped model is entered, so the provider catch
        // arm must publish the complete active-channel poison descriptor.
        let active_status = with_export_panic(EXPORT_PANIC_ACTIVE_CHANNEL, || unsafe {
            sakura_output_provider_active_channel_v1(token, &mut active)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, active_status);
        assert_poison_active(&active);

        let mut stop_result = SakuraOutputProviderApplyResultV1 {
            struct_size: 0,
            abi_version: 0,
            status: 0,
            reason: 0,
            revision: 0,
            callback_drain_deferred: 0,
            reserved: [0; 7],
        };
        // SAFETY: The result descriptor is valid caller-owned storage. The
        // panic occurs after the export writes its initial poison result.
        let stop_status = with_export_panic(EXPORT_PANIC_STOP, || unsafe {
            sakura_output_provider_stop_v1(token, &mut stop_result)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, stop_status);
        assert_poison_result(&stop_result);

        let mut destroy_slot = token;
        // SAFETY: The token slot is valid caller-owned storage. Destruction
        // has not started when this test-only panic is raised, so the opaque
        // token must remain unchanged on failure.
        let destroy_status = with_export_panic(EXPORT_PANIC_DESTROY, || unsafe {
            sakura_output_provider_destroy_v1(&mut destroy_slot)
        });
        assert_eq!(SakuraOutputProviderStatus::InternalError, destroy_status);
        assert_eq!(token, destroy_slot);

        // SAFETY: The token slot is valid and the provider is destroyed once
        // after all seven panic-boundary checks.
        assert_eq!(SakuraOutputProviderStatus::Ok, unsafe {
            sakura_output_provider_destroy_v1(&mut destroy_slot)
        });
        assert_eq!(0, destroy_slot);
    }

    #[test]
    fn provider_export_source_contract_contains_panic_containment_for_each_export() {
        let source = include_str!("output_provider.rs");
        let export_source = source
            .split("#[cfg(test)]\nmod tests")
            .next()
            .unwrap_or(source);
        let expected = [
            "sakura_output_provider_create_v1",
            "sakura_output_provider_apply_v1",
            "sakura_output_provider_snapshot_measure_v1",
            "sakura_output_provider_snapshot_write_v1",
            "sakura_output_provider_active_channel_v1",
            "sakura_output_provider_stop_v1",
            "sakura_output_provider_destroy_v1",
        ];
        assert_eq!(7, export_source.matches("catch_unwind(").count());
        assert_eq!(
            7,
            export_source.matches("pub unsafe extern \"C\" fn ").count()
        );
        for (index, name) in expected.iter().enumerate() {
            let marker = format!("pub unsafe extern \"C\" fn {name}");
            let start = export_source
                .find(&marker)
                .unwrap_or_else(|| panic!("missing provider export {name}"));
            let end = expected
                .get(index + 1)
                .and_then(|next| export_source.find(&format!("pub unsafe extern \"C\" fn {next}")))
                .unwrap_or(export_source.len());
            let body = &export_source[start..end];
            assert_eq!(1, body.matches("catch_unwind(").count(), "{name}");
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
            receipt: empty_receipt(),
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
        assert_eq!(
            [
                "sakura_output_provider_create_v1",
                "sakura_output_provider_apply_v1",
                "sakura_output_provider_snapshot_measure_v1",
                "sakura_output_provider_snapshot_write_v1",
                "sakura_output_provider_active_channel_v1",
                "sakura_output_provider_stop_v1",
                "sakura_output_provider_destroy_v1",
            ],
            exports.as_slice()
        );
        let forbidden = ["sakura_output_", "shadow_"].concat();
        assert!(!source.contains(&forbidden));
    }
}
