//! Replay-only OutputService state shadow.
//!
//! This module deliberately has no callback, UI, filesystem, transport, or
//! thread-affinity boundary.  The C++ OutputService remains the authority; a
//! caller submits a copy of an already accepted operation to this model and
//! may copy a deterministic snapshot back out.  The ABI is kept here, beside
//! the model, so the Rust side can validate every pointer and retain no caller
//! memory.

#![allow(dead_code)]

#[cfg(test)]
use std::cell::Cell;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::str;
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

const ABI_VERSION_V1: u32 = 1;

const MAX_STABLE_ID_BYTES: usize = 160;
const MAX_LABEL_BYTES: usize = 512;
const MAX_METADATA_BYTES: usize = 512;

const REQUEST_HAS_EXPECTED_REVISION: u32 = 1 << 0;
const REQUEST_PRESERVE_FOCUS: u32 = 1 << 1;
const REQUEST_LANGUAGE_PRESENT: u32 = 1 << 2;
const REQUEST_SOURCE_PRESENT: u32 = 1 << 3;
const REQUEST_KNOWN_FLAGS: u32 = REQUEST_HAS_EXPECTED_REVISION
    | REQUEST_PRESERVE_FOCUS
    | REQUEST_LANGUAGE_PRESENT
    | REQUEST_SOURCE_PRESENT;

const LOG_SOURCE_PRESENT: u32 = 1;

const OP_CREATE_CHANNEL: u32 = 1;
const OP_APPEND_OUTPUT: u32 = 2;
const OP_REPLACE_OUTPUT: u32 = 3;
const OP_APPEND_LOG: u32 = 4;
const OP_CLEAR: u32 = 5;
const OP_SHOW: u32 = 6;
const OP_HIDE: u32 = 7;
const OP_DISPOSE: u32 = 8;
const OP_DISPOSE_OWNER: u32 = 9;

const CHANNEL_KIND_OUTPUT: u8 = 0;
const CHANNEL_KIND_LOG: u8 = 1;

const LOG_TRACE: u32 = 0;
const LOG_DEBUG: u32 = 1;
const LOG_INFO: u32 = 2;
const LOG_WARNING: u32 = 3;
const LOG_ERROR: u32 = 4;

const SNAPSHOT_MAGIC: &[u8] = b"SAKURA_OUTPUT_MODEL_V1\0";

#[cfg(test)]
thread_local! {
    static FINGERPRINT_CONSTRUCTION_COUNT: Cell<usize> = const { Cell::new(0) };
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReserveFailurePoint {
    Map,
    Queue,
}

#[cfg(test)]
thread_local! {
    static NEXT_RESERVE_FAILURE: Cell<Option<ReserveFailurePoint>> = const { Cell::new(None) };
}

#[cfg(test)]
fn take_reserve_failure(point: ReserveFailurePoint) -> bool {
    NEXT_RESERVE_FAILURE.with(|failure| {
        if failure.get() == Some(point) {
            failure.set(None);
            true
        } else {
            false
        }
    })
}

#[cfg(test)]
fn fail_next_reserve(point: ReserveFailurePoint) {
    NEXT_RESERVE_FAILURE.with(|failure| failure.set(Some(point)));
}

#[cfg(test)]
fn discard_reserve_failure() {
    NEXT_RESERVE_FAILURE.with(|failure| failure.set(None));
}

/// Errors in the ABI call itself. Operation-level failures are returned in
/// `SakuraOutputShadowApplyResultV1` so rejected requests remain observable.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraOutputShadowStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidHandle = 2,
    Stopped = 3,
    InsufficientCapacity = 4,
    InternalError = 5,
}

/// Result status corresponding to `EOutputOperationStatus`.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraOutputShadowOperationStatus {
    Succeeded = 0,
    Replayed = 1,
    NotApplicable = 2,
    Rejected = 3,
    Conflict = 4,
    StaleRevision = 5,
    RevisionExhausted = 6,
    Stopped = 7,
}

/// Reason corresponding to `EOutputOperationReason`.
#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SakuraOutputShadowReason {
    None = 0,
    InvalidOperationId = 1,
    InvalidOwner = 2,
    InvalidChannelId = 3,
    InvalidLabel = 4,
    InvalidMetadata = 5,
    InvalidPayload = 6,
    PayloadLimitExceeded = 7,
    OwnerLimitExceeded = 8,
    ChannelLimitExceeded = 9,
    TextLimitExceeded = 10,
    LogEntryLimitExceeded = 11,
    ChannelNotFound = 12,
    OwnerGenerationConflict = 13,
    ChannelKindMismatch = 14,
    OperationIdConflict = 15,
    ExpectedRevisionMismatch = 16,
}

/// A caller-owned byte span.  A non-empty span must be non-null, addressable,
/// and remain immutable for the duration of one ABI call.  No pointer is
/// retained after the call returns.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowSpanV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *const u8,
    pub length: u64,
    pub reserved: [u64; 2],
}

/// Resource limits copied into a shadow at creation. Zero values are
/// normalized to one, matching OutputService's fail-closed constructor.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowLimitsV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub maximum_owners: u64,
    pub maximum_channels: u64,
    pub maximum_text_bytes_per_channel: u64,
    pub maximum_payload_bytes: u64,
    pub maximum_log_entries_per_channel: u64,
    pub maximum_remembered_operations: u64,
    pub reserved: [u64; 3],
}

/// One log entry in an append-log request.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowLogEntryV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub level: u32,
    pub flags: u32,
    pub message: SakuraOutputShadowSpanV1,
    pub source: SakuraOutputShadowSpanV1,
    pub reserved: [u64; 2],
}

/// One flattened operation request. Fields not used by `operation_kind` must
/// be empty and their corresponding flags must be clear. This makes the
/// operation fingerprint unambiguous and prevents accidental stale fields
/// from becoming a second representation of the same operation.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowRequestV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub operation_kind: u32,
    pub channel_kind: u32,
    pub flags: u32,
    pub operation_id: SakuraOutputShadowSpanV1,
    pub expected_revision: u64,
    pub owner_id: SakuraOutputShadowSpanV1,
    pub owner_generation: u64,
    pub channel_id: SakuraOutputShadowSpanV1,
    pub label: SakuraOutputShadowSpanV1,
    pub metadata_language_id: SakuraOutputShadowSpanV1,
    pub metadata_source: SakuraOutputShadowSpanV1,
    pub payload: SakuraOutputShadowSpanV1,
    pub log_entries: *const SakuraOutputShadowLogEntryV1,
    pub log_entry_count: u64,
    pub reserved: [u64; 4],
}

/// Operation result. The structure is initialized to a poison value before
/// any request/handle validation; callers must never consume an untrusted
/// success-looking result after a non-OK ABI status.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowApplyResultV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub status: u32,
    pub reason: u32,
    pub revision: u64,
    pub callback_drain_deferred: u8,
    pub reserved: [u8; 7],
}

/// Snapshot metadata returned by the measure call. The actual snapshot is a
/// canonical byte stream returned by the write call (see `snapshot_bytes`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowSnapshotInfoV1 {
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
}

/// Caller-provided destination for the canonical snapshot bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowSnapshotBufferV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub data: *mut u8,
    pub capacity: u64,
    pub length: u64,
    pub reserved: [u64; 2],
}

/// Caller-owned destination for the current active-channel identifier.  This
/// deliberately carries only post-commit metadata; advisory notification
/// dispatch must not copy the full retained channel snapshot per mutation.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SakuraOutputShadowActiveChannelV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub revision: u64,
    pub present: u8,
    pub reserved0: [u8; 7],
    pub data: *mut u8,
    pub capacity: u64,
    pub length: u64,
    pub reserved: [u64; 2],
}

#[derive(Clone, Debug)]
struct Limits {
    maximum_owners: usize,
    maximum_channels: usize,
    maximum_text_bytes_per_channel: usize,
    maximum_payload_bytes: usize,
    maximum_log_entries_per_channel: usize,
    maximum_remembered_operations: usize,
}

#[derive(Clone, Debug)]
struct Owner {
    id: Vec<u8>,
    generation: u64,
}

#[derive(Clone, Debug)]
struct Metadata {
    language_id: Option<Vec<u8>>,
    source: Option<Vec<u8>>,
}

#[derive(Clone, Debug)]
struct LogEntry {
    level: u32,
    message: Vec<u8>,
    source: Option<Vec<u8>>,
}

#[derive(Clone, Debug)]
struct Channel {
    id: Vec<u8>,
    label: Vec<u8>,
    owner: Owner,
    kind: u8,
    metadata: Metadata,
    visible: bool,
    last_show_preserved_focus: bool,
    dropped_character_count: u64,
    text: Vec<u8>,
    log_entries: VecDeque<LogEntry>,
    projected_text: Vec<u8>,
}

#[derive(Clone, Debug)]
struct OwnerGeneration {
    generation: u64,
    disposed: bool,
}

#[derive(Clone, Debug)]
struct Operation {
    id: Arc<[u8]>,
    expected_revision: Option<u64>,
}

#[derive(Clone, Debug)]
enum Request {
    CreateChannel {
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        label: Vec<u8>,
        kind: u8,
        metadata: Metadata,
    },
    Text {
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        text: Vec<u8>,
        replace: bool,
    },
    AppendLog {
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        entries: Vec<LogEntry>,
    },
    Channel {
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        kind: u32,
        preserve_focus: bool,
    },
    DisposeOwner {
        operation: Operation,
        owner: Owner,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct OperationResult {
    status: SakuraOutputShadowOperationStatus,
    reason: SakuraOutputShadowReason,
    revision: u64,
}

impl OperationResult {
    fn to_ffi(self) -> SakuraOutputShadowApplyResultV1 {
        SakuraOutputShadowApplyResultV1 {
            struct_size: size_of::<SakuraOutputShadowApplyResultV1>() as u32,
            abi_version: ABI_VERSION_V1,
            status: self.status as u32,
            reason: self.reason as u32,
            revision: self.revision,
            callback_drain_deferred: 0,
            reserved: [0; 7],
        }
    }
}

struct PreparedRemember {
    operation_id: Arc<[u8]>,
    fingerprint: Vec<u8>,
}

enum ApplyOutcome {
    NotAccepted(OperationResult),
    Accepted {
        prepared: PreparedRemember,
        result: OperationResult,
    },
    InternalError,
}

impl From<OperationResult> for ApplyOutcome {
    fn from(result: OperationResult) -> Self {
        Self::NotAccepted(result)
    }
}

#[derive(Clone, Debug)]
struct CompletedOperation {
    fingerprint: Vec<u8>,
    result: OperationResult,
}

#[derive(Clone, Debug)]
struct SnapshotCache {
    revision: u64,
    bytes: Vec<u8>,
}

#[derive(Clone, Debug)]
struct Service {
    limits: Limits,
    channels: BTreeMap<Vec<u8>, Channel>,
    active_owner_generations: BTreeMap<Vec<u8>, OwnerGeneration>,
    active_channel_id: Option<Vec<u8>>,
    completed_operations: HashMap<Arc<[u8]>, CompletedOperation>,
    completed_operation_order: VecDeque<Arc<[u8]>>,
    revision: u64,
    dropped_notification_count: u64,
    stopped: bool,
    snapshot_cache: Option<SnapshotCache>,
}

impl Service {
    fn new(limits: Limits) -> Self {
        Self {
            limits,
            channels: BTreeMap::new(),
            active_owner_generations: BTreeMap::new(),
            active_channel_id: None,
            completed_operations: HashMap::new(),
            completed_operation_order: VecDeque::new(),
            revision: 1,
            dropped_notification_count: 0,
            stopped: false,
            snapshot_cache: None,
        }
    }

    fn current<T>(
        &self,
        status: SakuraOutputShadowOperationStatus,
        reason: SakuraOutputShadowReason,
    ) -> T
    where
        T: From<OperationResult>,
    {
        T::from(OperationResult {
            status,
            reason,
            revision: self.revision,
        })
    }

    fn stopped_result(&self) -> OperationResult {
        self.current(
            SakuraOutputShadowOperationStatus::Stopped,
            SakuraOutputShadowReason::None,
        )
    }

    fn prepare_remember(
        &mut self,
        operation_id: Arc<[u8]>,
        fingerprint: Vec<u8>,
    ) -> Result<PreparedRemember, SakuraOutputShadowStatus> {
        debug_assert_eq!(
            self.completed_operations.len(),
            self.completed_operation_order.len()
        );
        debug_assert!(self.completed_operations.len() <= self.limits.maximum_remembered_operations);
        let needs_reserve =
            self.completed_operations.len() < self.limits.maximum_remembered_operations;
        if !needs_reserve {
            #[cfg(test)]
            // A full cache evicts before reusing both existing allocations.
            // Consume an inapplicable test injection so it cannot leak into a
            // later non-full operation.
            discard_reserve_failure();
        }
        if needs_reserve {
            #[cfg(test)]
            if take_reserve_failure(ReserveFailurePoint::Map) {
                return Err(SakuraOutputShadowStatus::InternalError);
            }
            self.completed_operations
                .try_reserve(1)
                .map_err(|_| SakuraOutputShadowStatus::InternalError)?;
            #[cfg(test)]
            if take_reserve_failure(ReserveFailurePoint::Queue) {
                return Err(SakuraOutputShadowStatus::InternalError);
            }
            self.completed_operation_order
                .try_reserve(1)
                .map_err(|_| SakuraOutputShadowStatus::InternalError)?;
        }
        Ok(PreparedRemember {
            operation_id,
            fingerprint,
        })
    }

    fn commit_remember(&mut self, prepared: PreparedRemember, result: OperationResult) {
        let PreparedRemember {
            operation_id,
            fingerprint,
        } = prepared;
        if self.completed_operations.len() == self.limits.maximum_remembered_operations {
            if let Some(oldest) = self.completed_operation_order.pop_front() {
                self.completed_operations.remove(&oldest);
            }
        }
        self.completed_operation_order
            .push_back(operation_id.clone());
        self.completed_operations.insert(
            operation_id,
            CompletedOperation {
                fingerprint,
                result,
            },
        );
    }

    #[cfg(test)]
    fn apply(&mut self, request: Request) -> OperationResult {
        self.try_apply(request)
            .expect("test operation should not fail to reserve replay storage")
    }

    fn try_apply(&mut self, request: Request) -> Result<OperationResult, SakuraOutputShadowStatus> {
        if self.stopped {
            return Ok(self.stopped_result());
        }

        let operation = request.operation();
        if !is_valid_operation_id(&operation.id) {
            return Ok(self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOperationId,
            ));
        }
        if let Some(found) = self.completed_operations.get(&operation.id) {
            // A remembered ID must retain replay/conflict precedence over every
            // later validation, including stale expected revisions. The
            // canonical bytes are therefore still built on this path.
            let fingerprint = fingerprint(&request);
            if found.fingerprint != fingerprint {
                return Ok(self.current(
                    SakuraOutputShadowOperationStatus::Conflict,
                    SakuraOutputShadowReason::OperationIdConflict,
                ));
            }
            let mut result = found.result;
            result.status = SakuraOutputShadowOperationStatus::Replayed;
            return Ok(result);
        }
        if operation.expected_revision != Some(self.revision)
            && operation.expected_revision.is_some()
        {
            return Ok(self.current(
                SakuraOutputShadowOperationStatus::StaleRevision,
                SakuraOutputShadowReason::ExpectedRevisionMismatch,
            ));
        }

        let outcome = match request {
            Request::CreateChannel {
                operation,
                owner,
                channel_id,
                label,
                kind,
                metadata,
            } => self.create_channel(operation, owner, channel_id, label, kind, metadata),
            Request::Text {
                operation,
                owner,
                channel_id,
                text,
                replace,
            } => self.apply_text(operation, owner, channel_id, text, replace),
            Request::AppendLog {
                operation,
                owner,
                channel_id,
                entries,
            } => self.append_log(operation, owner, channel_id, entries),
            Request::Channel {
                operation,
                owner,
                channel_id,
                kind,
                preserve_focus,
            } => self.apply_channel(operation, owner, channel_id, kind, preserve_focus),
            Request::DisposeOwner { operation, owner } => self.dispose_owner(operation, owner),
        };
        match outcome {
            ApplyOutcome::NotAccepted(result) => Ok(result),
            ApplyOutcome::Accepted { prepared, result } => {
                self.commit_remember(prepared, result);
                Ok(result)
            }
            ApplyOutcome::InternalError => Err(SakuraOutputShadowStatus::InternalError),
        }
    }

    fn create_channel(
        &mut self,
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        label: Vec<u8>,
        kind: u8,
        metadata: Metadata,
    ) -> ApplyOutcome {
        if !is_valid_owner(&owner) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOwner,
            );
        }
        if !is_valid_stable_id(&channel_id) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidChannelId,
            );
        }
        if label.is_empty() || label.len() > MAX_LABEL_BYTES || !is_valid_utf8(&label, false) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidLabel,
            );
        }
        if kind != CHANNEL_KIND_OUTPUT && kind != CHANNEL_KIND_LOG {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidPayload,
            );
        }
        if !is_valid_metadata(&metadata) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidMetadata,
            );
        }

        let existing_generation = self.active_owner_generations.get(&owner.id).cloned();
        if existing_generation.is_none()
            && self.active_owner_generations.len() >= self.limits.maximum_owners
        {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::OwnerLimitExceeded,
            );
        }

        let mut adopts_new_generation = false;
        let mut replaced_owner_channels = 0_usize;
        if let Some(existing) = existing_generation {
            if owner.generation < existing.generation
                || (owner.generation == existing.generation && existing.disposed)
            {
                return self.current(
                    SakuraOutputShadowOperationStatus::Conflict,
                    SakuraOutputShadowReason::OwnerGenerationConflict,
                );
            }
            adopts_new_generation = owner.generation > existing.generation;
            if adopts_new_generation {
                replaced_owner_channels = self
                    .channels
                    .values()
                    .filter(|channel| channel.owner.id == owner.id)
                    .count();
            }
        }

        if let Some(existing) = self.channels.get(&channel_id) {
            if !(adopts_new_generation && existing.owner.id == owner.id) {
                return self.current(
                    SakuraOutputShadowOperationStatus::Conflict,
                    SakuraOutputShadowReason::InvalidChannelId,
                );
            }
        }
        if self.channels.len().saturating_sub(replaced_owner_channels)
            >= self.limits.maximum_channels
        {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::ChannelLimitExceeded,
            );
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }

        let fingerprint =
            fingerprint_create_channel(&operation, &owner, &channel_id, &label, kind, &metadata);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        if adopts_new_generation {
            self.channels
                .retain(|_, channel| channel.owner.id != owner.id);
            self.active_channel_id = None;
            self.select_fallback();
        }
        self.active_owner_generations.insert(
            owner.id.clone(),
            OwnerGeneration {
                generation: owner.generation,
                disposed: false,
            },
        );
        self.channels.insert(
            channel_id.clone(),
            Channel {
                id: channel_id,
                label,
                owner,
                kind,
                metadata,
                visible: false,
                last_show_preserved_focus: false,
                dropped_character_count: 0,
                text: Vec::new(),
                log_entries: VecDeque::new(),
                projected_text: Vec::new(),
            },
        );
        self.select_fallback();
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn apply_text(
        &mut self,
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        text: Vec<u8>,
        replace: bool,
    ) -> ApplyOutcome {
        if !is_valid_bounded_text(&text, self.limits.maximum_payload_bytes) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                if text.len() > self.limits.maximum_payload_bytes {
                    SakuraOutputShadowReason::PayloadLimitExceeded
                } else {
                    SakuraOutputShadowReason::InvalidPayload
                },
            );
        }
        if !self.channel_matches(&owner, &channel_id, Some(CHANNEL_KIND_OUTPUT)) {
            return self.validation_result(&owner, &channel_id, CHANNEL_KIND_OUTPUT);
        }
        if !replace && text.is_empty() {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::None,
            );
        }
        let unchanged = replace
            && self.channels.get(&channel_id).is_some_and(|channel| {
                channel.text == text && channel.dropped_character_count == 0
            });
        if unchanged {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::None,
            );
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        let fingerprint = fingerprint_text(&operation, &owner, &channel_id, &text, replace);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        let maximum_text_bytes_per_channel = self.limits.maximum_text_bytes_per_channel;
        let channel = self
            .channels
            .get_mut(&channel_id)
            .expect("validated channel must remain present");
        if replace {
            channel.text = text;
            channel.dropped_character_count = 0;
        } else {
            channel.text.extend_from_slice(&text);
        }
        keep_suffix(
            &mut channel.text,
            maximum_text_bytes_per_channel,
            &mut channel.dropped_character_count,
        );
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn append_log(
        &mut self,
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        entries: Vec<LogEntry>,
    ) -> ApplyOutcome {
        if entries.is_empty() {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::None,
            );
        }
        if entries.len() > self.limits.maximum_log_entries_per_channel {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::LogEntryLimitExceeded,
            );
        }
        let mut payload_bytes = 0_usize;
        for entry in &entries {
            payload_bytes = payload_bytes
                .saturating_add(entry.message.len())
                .saturating_add(entry.source.as_ref().map_or(0, Vec::len));
            if !is_valid_bounded_text(&entry.message, self.limits.maximum_payload_bytes)
                || entry.message.is_empty()
                || !is_valid_metadata_value(&entry.source)
            {
                return self.current(
                    SakuraOutputShadowOperationStatus::Rejected,
                    SakuraOutputShadowReason::InvalidPayload,
                );
            }
            if payload_bytes > self.limits.maximum_payload_bytes {
                return self.current(
                    SakuraOutputShadowOperationStatus::Rejected,
                    SakuraOutputShadowReason::PayloadLimitExceeded,
                );
            }
        }
        if !self.channel_matches(&owner, &channel_id, Some(CHANNEL_KIND_LOG)) {
            return self.validation_result(&owner, &channel_id, CHANNEL_KIND_LOG);
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        let fingerprint = fingerprint_append_log(&operation, &owner, &channel_id, &entries);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        let maximum_log_entries_per_channel = self.limits.maximum_log_entries_per_channel;
        let maximum_text_bytes_per_channel = self.limits.maximum_text_bytes_per_channel;
        let channel = self
            .channels
            .get_mut(&channel_id)
            .expect("validated channel must remain present");
        channel.log_entries.extend(entries);
        while channel.log_entries.len() > maximum_log_entries_per_channel {
            if let Some(entry) = channel.log_entries.pop_front() {
                saturating_add(
                    &mut channel.dropped_character_count,
                    character_count(&render_log_entry(&entry)),
                );
            }
        }
        rebuild_log_projection(channel, maximum_text_bytes_per_channel);
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn apply_channel(
        &mut self,
        operation: Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        kind: u32,
        preserve_focus: bool,
    ) -> ApplyOutcome {
        if kind == OP_CLEAR {
            return self.clear_channel(operation, &owner, &channel_id);
        }
        if kind == OP_SHOW {
            if !self.channel_matches(&owner, &channel_id, None) {
                return self.validation_result_any(&owner, &channel_id);
            }
            if self.revision == u64::MAX {
                return self.current(
                    SakuraOutputShadowOperationStatus::RevisionExhausted,
                    SakuraOutputShadowReason::None,
                );
            }
            let fingerprint =
                fingerprint_channel(&operation, &owner, &channel_id, kind, preserve_focus);
            let prepared = match self.prepare_remember(operation.id, fingerprint) {
                Ok(prepared) => prepared,
                Err(_) => return ApplyOutcome::InternalError,
            };
            let channel = self
                .channels
                .get_mut(&channel_id)
                .expect("validated channel must remain present");
            channel.visible = true;
            channel.last_show_preserved_focus = preserve_focus;
            self.active_channel_id = Some(channel.id.clone());
            self.revision += 1;
            let result = self.current(
                SakuraOutputShadowOperationStatus::Succeeded,
                SakuraOutputShadowReason::None,
            );
            return ApplyOutcome::Accepted { prepared, result };
        }
        if !self.channel_matches(&owner, &channel_id, None) {
            return self.validation_result_any(&owner, &channel_id);
        }
        if kind == OP_HIDE {
            let visible = self
                .channels
                .get(&channel_id)
                .expect("validated channel must remain present")
                .visible;
            if !visible {
                return self.current(
                    SakuraOutputShadowOperationStatus::NotApplicable,
                    SakuraOutputShadowReason::None,
                );
            }
            if self.revision == u64::MAX {
                return self.current(
                    SakuraOutputShadowOperationStatus::RevisionExhausted,
                    SakuraOutputShadowReason::None,
                );
            }
            let fingerprint =
                fingerprint_channel(&operation, &owner, &channel_id, kind, preserve_focus);
            let prepared = match self.prepare_remember(operation.id, fingerprint) {
                Ok(prepared) => prepared,
                Err(_) => return ApplyOutcome::InternalError,
            };
            let channel = self
                .channels
                .get_mut(&channel_id)
                .expect("validated channel must remain present");
            channel.visible = false;
            if self.active_channel_id.as_ref() == Some(&channel_id) {
                self.active_channel_id = None;
                self.select_fallback();
            }
            self.revision += 1;
            let result = self.current(
                SakuraOutputShadowOperationStatus::Succeeded,
                SakuraOutputShadowReason::None,
            );
            return ApplyOutcome::Accepted { prepared, result };
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        let fingerprint =
            fingerprint_channel(&operation, &owner, &channel_id, kind, preserve_focus);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        self.channels.remove(&channel_id);
        if self.active_channel_id.as_ref() == Some(&channel_id) {
            self.active_channel_id = None;
            self.select_fallback();
        }
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn clear_channel(
        &mut self,
        operation: Operation,
        owner: &Owner,
        channel_id: &[u8],
    ) -> ApplyOutcome {
        if !self.channel_matches(owner, channel_id, None) {
            return self.validation_result_any(owner, channel_id);
        }
        let empty = {
            let channel = self
                .channels
                .get(channel_id)
                .expect("validated channel must remain present");
            channel.text.is_empty()
                && channel.log_entries.is_empty()
                && channel.dropped_character_count == 0
        };
        if empty {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::None,
            );
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        let fingerprint = fingerprint_channel(&operation, owner, channel_id, OP_CLEAR, false);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        let channel = self
            .channels
            .get_mut(channel_id)
            .expect("validated channel must remain present");
        channel.text.clear();
        channel.log_entries.clear();
        channel.projected_text.clear();
        channel.dropped_character_count = 0;
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn dispose_owner(&mut self, operation: Operation, owner: Owner) -> ApplyOutcome {
        if !is_valid_owner(&owner) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOwner,
            );
        }
        let Some(active) = self.active_owner_generations.get(&owner.id).cloned() else {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::ChannelNotFound,
            );
        };
        if active.disposed {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::ChannelNotFound,
            );
        }
        if active.generation != owner.generation {
            return self.current(
                SakuraOutputShadowOperationStatus::Conflict,
                SakuraOutputShadowReason::OwnerGenerationConflict,
            );
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        let fingerprint = fingerprint_dispose_owner(&operation, &owner);
        let prepared = match self.prepare_remember(operation.id, fingerprint) {
            Ok(prepared) => prepared,
            Err(_) => return ApplyOutcome::InternalError,
        };
        self.channels.retain(|_, channel| {
            channel.owner.id != owner.id || channel.owner.generation != owner.generation
        });
        if let Some(active) = self.active_owner_generations.get_mut(&owner.id) {
            active.disposed = true;
        }
        self.active_channel_id = None;
        self.select_fallback();
        self.revision += 1;
        let result = self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        );
        ApplyOutcome::Accepted { prepared, result }
    }

    fn channel_matches(&self, owner: &Owner, channel_id: &[u8], expected_kind: Option<u8>) -> bool {
        if !is_valid_owner(owner) || !is_valid_stable_id(channel_id) {
            return false;
        }
        let Some(channel) = self.channels.get(channel_id) else {
            return false;
        };
        if channel.owner.id != owner.id
            || channel.owner.generation != owner.generation
            || expected_kind.is_some_and(|kind| channel.kind != kind)
        {
            return false;
        }
        true
    }

    fn validation_result(
        &self,
        owner: &Owner,
        channel_id: &[u8],
        expected_kind: u8,
    ) -> ApplyOutcome {
        if !is_valid_owner(owner) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOwner,
            );
        }
        if !is_valid_stable_id(channel_id) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidChannelId,
            );
        }
        let Some(channel) = self.channels.get(channel_id) else {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::ChannelNotFound,
            );
        };
        if channel.owner.id != owner.id || channel.owner.generation != owner.generation {
            return self.current(
                SakuraOutputShadowOperationStatus::Conflict,
                SakuraOutputShadowReason::OwnerGenerationConflict,
            );
        }
        if channel.kind != expected_kind {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::ChannelKindMismatch,
            );
        }
        self.current(
            SakuraOutputShadowOperationStatus::Rejected,
            SakuraOutputShadowReason::InvalidPayload,
        )
    }

    fn validation_result_any(&self, owner: &Owner, channel_id: &[u8]) -> ApplyOutcome {
        if !is_valid_owner(owner) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOwner,
            );
        }
        if !is_valid_stable_id(channel_id) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidChannelId,
            );
        }
        let Some(channel) = self.channels.get(channel_id) else {
            return self.current(
                SakuraOutputShadowOperationStatus::NotApplicable,
                SakuraOutputShadowReason::ChannelNotFound,
            );
        };
        if channel.owner.id != owner.id || channel.owner.generation != owner.generation {
            return self.current(
                SakuraOutputShadowOperationStatus::Conflict,
                SakuraOutputShadowReason::OwnerGenerationConflict,
            );
        }
        self.current(
            SakuraOutputShadowOperationStatus::Rejected,
            SakuraOutputShadowReason::InvalidPayload,
        )
    }

    fn select_fallback(&mut self) {
        if self
            .active_channel_id
            .as_ref()
            .is_some_and(|id| self.channels.contains_key(id))
        {
            return;
        }
        if let Some((id, _)) = self.channels.iter().find(|(_, channel)| channel.visible) {
            self.active_channel_id = Some(id.clone());
        } else {
            self.active_channel_id = self.channels.keys().next().cloned();
        }
    }

    fn stop(&mut self) -> OperationResult {
        if !self.stopped {
            // Stop is snapshot-visible even if a saturated revision cannot
            // advance, so do not let a revision-only cache key reuse a live
            // snapshot for the terminal state.
            self.snapshot_cache = None;
            self.channels.clear();
            self.active_owner_generations.clear();
            self.active_channel_id = None;
            self.completed_operations.clear();
            self.completed_operation_order.clear();
            self.stopped = true;
            self.revision = self.revision.saturating_add(1);
        }
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn snapshot_bytes(&mut self) -> &[u8] {
        // Every snapshot-visible mutation advances `revision`; the only other
        // snapshot-visible field, `dropped_notification_count`, is immutable in
        // this shadow. Keep that invariant when adding new mutable state.
        if self
            .snapshot_cache
            .as_ref()
            .is_some_and(|cache| cache.revision == self.revision)
        {
            return &self
                .snapshot_cache
                .as_ref()
                .expect("matching snapshot cache exists")
                .bytes;
        }

        let mut output = Vec::new();
        output.extend_from_slice(SNAPSHOT_MAGIC);
        put_u64(&mut output, self.revision);
        output.push(u8::from(self.stopped));
        put_u64(&mut output, self.dropped_notification_count);
        match &self.active_channel_id {
            Some(id) => {
                output.push(1);
                put_bytes(&mut output, id);
            }
            None => output.push(0),
        }
        put_u64(&mut output, self.channels.len() as u64);
        for channel in self.channels.values() {
            put_bytes(&mut output, &channel.id);
            put_bytes(&mut output, &channel.label);
            put_bytes(&mut output, &channel.owner.id);
            put_u64(&mut output, channel.owner.generation);
            output.push(channel.kind);
            put_optional_bytes(&mut output, channel.metadata.language_id.as_deref());
            put_optional_bytes(&mut output, channel.metadata.source.as_deref());
            output.push(u8::from(channel.visible));
            output.push(u8::from(channel.last_show_preserved_focus));
            put_u64(&mut output, channel.dropped_character_count);
            put_bytes(&mut output, &channel.text);
            put_u64(&mut output, channel.log_entries.len() as u64);
            for entry in &channel.log_entries {
                put_u32(&mut output, entry.level);
                put_bytes(&mut output, &entry.message);
                put_optional_bytes(&mut output, entry.source.as_deref());
            }
            // Plain Output projection is exactly its retained text. Do not
            // duplicate that buffer on every append/replace; structured Log
            // channels still retain their separately rendered projection.
            let projected_text = if channel.kind == CHANNEL_KIND_OUTPUT {
                &channel.text
            } else {
                &channel.projected_text
            };
            put_bytes(&mut output, projected_text);
        }
        self.snapshot_cache = Some(SnapshotCache {
            revision: self.revision,
            bytes: output,
        });
        &self
            .snapshot_cache
            .as_ref()
            .expect("snapshot cache was inserted")
            .bytes
    }
}

impl Request {
    fn operation(&self) -> &Operation {
        match self {
            Self::CreateChannel { operation, .. }
            | Self::Text { operation, .. }
            | Self::AppendLog { operation, .. }
            | Self::Channel { operation, .. }
            | Self::DisposeOwner { operation, .. } => operation,
        }
    }
}

fn poison_result() -> SakuraOutputShadowApplyResultV1 {
    SakuraOutputShadowApplyResultV1 {
        struct_size: size_of::<SakuraOutputShadowApplyResultV1>() as u32,
        abi_version: ABI_VERSION_V1,
        status: SakuraOutputShadowOperationStatus::RevisionExhausted as u32,
        reason: SakuraOutputShadowReason::InvalidPayload as u32,
        revision: u64::MAX,
        callback_drain_deferred: 0,
        reserved: [0; 7],
    }
}

fn poison_info() -> SakuraOutputShadowSnapshotInfoV1 {
    SakuraOutputShadowSnapshotInfoV1 {
        struct_size: size_of::<SakuraOutputShadowSnapshotInfoV1>() as u32,
        abi_version: ABI_VERSION_V1,
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

fn poison_active_channel() -> SakuraOutputShadowActiveChannelV1 {
    SakuraOutputShadowActiveChannelV1 {
        struct_size: size_of::<SakuraOutputShadowActiveChannelV1>() as u32,
        abi_version: ABI_VERSION_V1,
        revision: u64::MAX,
        present: 0xff,
        reserved0: [0; 7],
        data: ptr::null_mut(),
        capacity: u64::MAX,
        length: u64::MAX,
        reserved: [0; 2],
    }
}

fn validate_struct_header(struct_size: u32, abi_version: u32, expected_size: usize) -> bool {
    struct_size as usize == expected_size && abi_version == ABI_VERSION_V1
}

fn is_aligned<T>(pointer: *const T) -> bool {
    (pointer as usize).is_multiple_of(align_of::<T>())
}

fn is_valid_pointer<T>(pointer: *const T) -> bool {
    if pointer.is_null() || !is_aligned(pointer) {
        return false;
    }
    (pointer as usize)
        .checked_add(size_of::<T>())
        .is_some_and(|end| end <= isize::MAX as usize)
}

fn checked_len(length: u64) -> Option<usize> {
    usize::try_from(length).ok()
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

#[derive(Clone, Copy, Debug)]
struct ValidatedSpan {
    data: *const u8,
    length: usize,
}

/// Validates only the fixed span descriptor fields and converts the bounded
/// length.  Address validation is deliberately a second stage so callers can
/// reject operation-inapplicable non-empty spans without ever touching their
/// pointed-to bytes.
fn validate_span_shape(span: SakuraOutputShadowSpanV1) -> Result<usize, SakuraOutputShadowStatus> {
    if !validate_struct_header(
        span.struct_size,
        span.abi_version,
        size_of::<SakuraOutputShadowSpanV1>(),
    ) || span.reserved != [0; 2]
    {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    checked_len(span.length).ok_or(SakuraOutputShadowStatus::InvalidArgument)
}

fn validate_span_range(
    span: SakuraOutputShadowSpanV1,
    length: usize,
) -> Result<ValidatedSpan, SakuraOutputShadowStatus> {
    if length == 0 {
        return Ok(ValidatedSpan {
            data: span.data,
            length,
        });
    }
    if span.data.is_null() || !is_aligned(span.data) || length > isize::MAX as usize {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    (span.data as usize)
        .checked_add(length)
        .filter(|end| *end <= isize::MAX as usize)
        .ok_or(SakuraOutputShadowStatus::InvalidArgument)?;
    Ok(ValidatedSpan {
        data: span.data,
        length,
    })
}

fn copy_validated_span(span: ValidatedSpan) -> Vec<u8> {
    if span.length == 0 {
        return Vec::new();
    }
    // SAFETY: The ABI contract requires an immutable initialized byte span for
    // this call. Null, alignment, length, and address overflow were checked.
    unsafe { slice::from_raw_parts(span.data, span.length) }.to_vec()
}

fn copy_span(span: SakuraOutputShadowSpanV1) -> Result<Vec<u8>, SakuraOutputShadowStatus> {
    let length = validate_span_shape(span)?;
    let span = validate_span_range(span, length)?;
    Ok(copy_validated_span(span))
}

fn copy_shaped_span(
    span: SakuraOutputShadowSpanV1,
    length: usize,
) -> Result<Vec<u8>, SakuraOutputShadowStatus> {
    let span = validate_span_range(span, length)?;
    Ok(copy_validated_span(span))
}

fn copy_shaped_arc_span(
    span: SakuraOutputShadowSpanV1,
    length: usize,
) -> Result<Arc<[u8]>, SakuraOutputShadowStatus> {
    let span = validate_span_range(span, length)?;
    if span.length == 0 {
        return Ok(Arc::from(&[][..]));
    }
    // SAFETY: The ABI contract requires an immutable initialized byte span for
    // this call. Null, alignment, length, and address overflow were checked.
    // `Arc::from` copies the bytes into owned Arc storage and retains no raw
    // pointer after this function returns.
    Ok(Arc::from(unsafe {
        slice::from_raw_parts(span.data, span.length)
    }))
}

fn copy_operation_and_owner(
    raw: SakuraOutputShadowRequestV1,
    operation_id_length: usize,
    owner_id_length: usize,
    expected_revision: Option<u64>,
) -> Result<(Operation, Owner), SakuraOutputShadowStatus> {
    Ok((
        Operation {
            id: copy_shaped_arc_span(raw.operation_id, operation_id_length)?,
            expected_revision,
        },
        Owner {
            id: copy_shaped_span(raw.owner_id, owner_id_length)?,
            generation: raw.owner_generation,
        },
    ))
}

fn read_log_entries(
    pointer: *const SakuraOutputShadowLogEntryV1,
    count: u64,
) -> Result<Vec<LogEntry>, SakuraOutputShadowStatus> {
    let count = checked_len(count).ok_or(SakuraOutputShadowStatus::InvalidArgument)?;
    if count == 0 {
        return if pointer.is_null() {
            Ok(Vec::new())
        } else {
            Err(SakuraOutputShadowStatus::InvalidArgument)
        };
    }
    if pointer.is_null() || !is_aligned(pointer) || count > isize::MAX as usize {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    let bytes = count
        .checked_mul(size_of::<SakuraOutputShadowLogEntryV1>())
        .filter(|value| *value <= isize::MAX as usize)
        .ok_or(SakuraOutputShadowStatus::InvalidArgument)?;
    (pointer as usize)
        .checked_add(bytes)
        .filter(|end| *end <= isize::MAX as usize)
        .ok_or(SakuraOutputShadowStatus::InvalidArgument)?;
    // SAFETY: The ABI contract requires an immutable initialized array for
    // this call; the bounded count, alignment, and address range were checked.
    let entries = unsafe { slice::from_raw_parts(pointer, count) };
    let mut copied = Vec::with_capacity(count);
    for entry in entries {
        if !validate_struct_header(
            entry.struct_size,
            entry.abi_version,
            size_of::<SakuraOutputShadowLogEntryV1>(),
        ) || entry.flags & !LOG_SOURCE_PRESENT != 0
            || entry.level > LOG_ERROR
            || entry.reserved != [0; 2]
        {
            return Err(SakuraOutputShadowStatus::InvalidArgument);
        }
        let message = copy_span(entry.message)?;
        let source = if entry.flags & LOG_SOURCE_PRESENT != 0 {
            Some(copy_span(entry.source)?)
        } else {
            if !validate_struct_header(
                entry.source.struct_size,
                entry.source.abi_version,
                size_of::<SakuraOutputShadowSpanV1>(),
            ) || entry.source.reserved != [0; 2]
                || entry.source.length != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            None
        };
        copied.push(LogEntry {
            level: entry.level,
            message,
            source,
        });
    }
    Ok(copied)
}

fn read_request(
    pointer: *const SakuraOutputShadowRequestV1,
) -> Result<Request, SakuraOutputShadowStatus> {
    if !is_valid_pointer(pointer) {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    // SAFETY: Nullability and alignment were checked. The caller owns the
    // initialized immutable request for the duration of this call.
    let raw = unsafe { pointer.read() };
    if !validate_struct_header(
        raw.struct_size,
        raw.abi_version,
        size_of::<SakuraOutputShadowRequestV1>(),
    ) || raw.flags & !REQUEST_KNOWN_FLAGS != 0
        || raw.reserved != [0; 4]
    {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    // Validate every fixed span descriptor before selecting an operation. This
    // keeps the pointer-free stage deterministic and lets the operation arms
    // use lengths for their empty/inapplicable-field checks without allocating
    // copies of those fields.
    let operation_id_length = validate_span_shape(raw.operation_id)?;
    let owner_id_length = validate_span_shape(raw.owner_id)?;
    let channel_id_length = validate_span_shape(raw.channel_id)?;
    let label_length = validate_span_shape(raw.label)?;
    let language_id_length = validate_span_shape(raw.metadata_language_id)?;
    let source_length = validate_span_shape(raw.metadata_source)?;
    let payload_length = validate_span_shape(raw.payload)?;
    let expected_revision = if raw.flags & REQUEST_HAS_EXPECTED_REVISION != 0 {
        Some(raw.expected_revision)
    } else {
        if raw.expected_revision != 0 {
            return Err(SakuraOutputShadowStatus::InvalidArgument);
        }
        None
    };
    let kind = raw.operation_kind;
    match kind {
        OP_CREATE_CHANNEL => {
            if payload_length != 0
                || raw.log_entry_count != 0
                || !raw.log_entries.is_null()
                || raw.flags & REQUEST_PRESERVE_FOCUS != 0
                || raw.channel_kind > u32::from(CHANNEL_KIND_LOG)
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            if (raw.flags & REQUEST_LANGUAGE_PRESENT == 0 && language_id_length != 0)
                || (raw.flags & REQUEST_SOURCE_PRESENT == 0 && source_length != 0)
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            let metadata = Metadata {
                language_id: if raw.flags & REQUEST_LANGUAGE_PRESENT != 0 {
                    Some(copy_shaped_span(
                        raw.metadata_language_id,
                        language_id_length,
                    )?)
                } else {
                    None
                },
                source: if raw.flags & REQUEST_SOURCE_PRESENT != 0 {
                    Some(copy_shaped_span(raw.metadata_source, source_length)?)
                } else {
                    None
                },
            };
            Ok(Request::CreateChannel {
                operation,
                owner,
                channel_id: copy_shaped_span(raw.channel_id, channel_id_length)?,
                label: copy_shaped_span(raw.label, label_length)?,
                kind: raw.channel_kind as u8,
                metadata,
            })
        }
        OP_APPEND_OUTPUT | OP_REPLACE_OUTPUT => {
            if language_id_length != 0
                || source_length != 0
                || raw.log_entry_count != 0
                || !raw.log_entries.is_null()
                || raw.flags
                    & (REQUEST_PRESERVE_FOCUS | REQUEST_LANGUAGE_PRESENT | REQUEST_SOURCE_PRESENT)
                    != 0
                || raw.channel_kind != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            // Text operations historically ignore `label` contents, but the
            // ABI still requires every non-empty span to be addressable. Keep
            // that validation without copying the ignored bytes.
            validate_span_range(raw.label, label_length)?;
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            Ok(Request::Text {
                operation,
                owner,
                channel_id: copy_shaped_span(raw.channel_id, channel_id_length)?,
                text: copy_shaped_span(raw.payload, payload_length)?,
                replace: kind == OP_REPLACE_OUTPUT,
            })
        }
        OP_APPEND_LOG => {
            if label_length != 0
                || language_id_length != 0
                || source_length != 0
                || payload_length != 0
                || raw.flags
                    & (REQUEST_PRESERVE_FOCUS | REQUEST_LANGUAGE_PRESENT | REQUEST_SOURCE_PRESENT)
                    != 0
                || raw.channel_kind != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            Ok(Request::AppendLog {
                operation,
                owner,
                channel_id: copy_shaped_span(raw.channel_id, channel_id_length)?,
                entries: read_log_entries(raw.log_entries, raw.log_entry_count)?,
            })
        }
        OP_CLEAR | OP_HIDE | OP_DISPOSE => {
            if label_length != 0
                || language_id_length != 0
                || source_length != 0
                || payload_length != 0
                || raw.log_entry_count != 0
                || !raw.log_entries.is_null()
                || raw.flags
                    & (REQUEST_PRESERVE_FOCUS | REQUEST_LANGUAGE_PRESENT | REQUEST_SOURCE_PRESENT)
                    != 0
                || raw.channel_kind != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            Ok(Request::Channel {
                operation,
                owner,
                channel_id: copy_shaped_span(raw.channel_id, channel_id_length)?,
                kind,
                preserve_focus: false,
            })
        }
        OP_SHOW => {
            if label_length != 0
                || language_id_length != 0
                || source_length != 0
                || payload_length != 0
                || raw.log_entry_count != 0
                || !raw.log_entries.is_null()
                || raw.flags & (REQUEST_LANGUAGE_PRESENT | REQUEST_SOURCE_PRESENT) != 0
                || raw.channel_kind != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            Ok(Request::Channel {
                operation,
                owner,
                channel_id: copy_shaped_span(raw.channel_id, channel_id_length)?,
                kind,
                preserve_focus: raw.flags & REQUEST_PRESERVE_FOCUS != 0,
            })
        }
        OP_DISPOSE_OWNER => {
            if channel_id_length != 0
                || label_length != 0
                || language_id_length != 0
                || source_length != 0
                || payload_length != 0
                || raw.log_entry_count != 0
                || !raw.log_entries.is_null()
                || raw.flags
                    & (REQUEST_PRESERVE_FOCUS | REQUEST_LANGUAGE_PRESENT | REQUEST_SOURCE_PRESENT)
                    != 0
                || raw.channel_kind != 0
            {
                return Err(SakuraOutputShadowStatus::InvalidArgument);
            }
            let (operation, owner) = copy_operation_and_owner(
                raw,
                operation_id_length,
                owner_id_length,
                expected_revision,
            )?;
            Ok(Request::DisposeOwner { operation, owner })
        }
        _ => Err(SakuraOutputShadowStatus::InvalidArgument),
    }
}

fn read_limits(
    pointer: *const SakuraOutputShadowLimitsV1,
) -> Result<Limits, SakuraOutputShadowStatus> {
    if !is_valid_pointer(pointer) {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    // SAFETY: Nullability and alignment were checked; the caller owns the
    // initialized immutable limits for the duration of this call.
    let raw = unsafe { pointer.read() };
    if !validate_struct_header(
        raw.struct_size,
        raw.abi_version,
        size_of::<SakuraOutputShadowLimitsV1>(),
    ) || raw.reserved != [0; 3]
    {
        return Err(SakuraOutputShadowStatus::InvalidArgument);
    }
    Ok(Limits {
        maximum_owners: normalize_limit(raw.maximum_owners)?,
        maximum_channels: normalize_limit(raw.maximum_channels)?,
        maximum_text_bytes_per_channel: normalize_limit(raw.maximum_text_bytes_per_channel)?,
        maximum_payload_bytes: normalize_limit(raw.maximum_payload_bytes)?,
        maximum_log_entries_per_channel: normalize_limit(raw.maximum_log_entries_per_channel)?,
        maximum_remembered_operations: normalize_limit(raw.maximum_remembered_operations)?,
    })
}

fn normalize_limit(value: u64) -> Result<usize, SakuraOutputShadowStatus> {
    let value = if value == 0 { 1 } else { value };
    usize::try_from(value).map_err(|_| SakuraOutputShadowStatus::InvalidArgument)
}

fn lock_registry() -> MutexGuard<'static, Registry> {
    registry()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

struct Registry {
    next_token: u64,
    services: BTreeMap<u64, Service>,
}

static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();

fn registry() -> &'static Mutex<Registry> {
    REGISTRY.get_or_init(|| {
        Mutex::new(Registry {
            next_token: 1,
            services: BTreeMap::new(),
        })
    })
}

fn catch_status(operation: impl FnOnce() -> SakuraOutputShadowStatus) -> SakuraOutputShadowStatus {
    catch_unwind(AssertUnwindSafe(operation)).unwrap_or(SakuraOutputShadowStatus::InternalError)
}

/// Creates one replay-only shadow and returns a numeric opaque token.
///
/// # Safety
///
/// `limits` must point to one initialized immutable V1 structure and `token`
/// must point to writable `u64` storage. No pointer is retained.
pub(crate) unsafe fn model_create_v1(
    limits: *const SakuraOutputShadowLimitsV1,
    token: *mut u64,
) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(token.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null and aligned. Poisoning it
    // before validation makes every failure fail closed.
    unsafe { token.write(0) };
    let limits = match read_limits(limits) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let mut registry = lock_registry();
    let token_value = registry.next_token;
    if token_value == 0 {
        return SakuraOutputShadowStatus::InternalError;
    }
    registry.next_token = token_value.checked_add(1).unwrap_or(0);
    registry.services.insert(token_value, Service::new(limits));
    // SAFETY: The output pointer was validated above and remains caller
    // owned for this call.
    unsafe { token.write(token_value) };
    SakuraOutputShadowStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_create_v1(
    limits: *const SakuraOutputShadowLimitsV1,
    token: *mut u64,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_create_v1(limits, token) }
    })
}

/// Applies one copied operation to the replay model.
///
/// # Safety
///
/// `request` and `result` must point to initialized/caller-owned V1 storage
/// for the duration of this call. Nested spans and log-entry arrays follow
/// the same immutable, non-retained contract.
pub(crate) unsafe fn model_apply_v1(
    token: u64,
    request: *const SakuraOutputShadowRequestV1,
    result: *mut SakuraOutputShadowApplyResultV1,
) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(result.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null and aligned. Publish a poison
    // result before reading any caller-controlled input.
    unsafe { result.write(poison_result()) };
    if !request.is_null()
        && is_aligned(request)
        && ranges_overlap(
            request.cast(),
            size_of::<SakuraOutputShadowRequestV1>(),
            result.cast(),
            size_of::<SakuraOutputShadowApplyResultV1>(),
        )
    {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    let request = match read_request(request) {
        Ok(value) => value,
        Err(status) => return status,
    };
    let mut registry = lock_registry();
    let Some(service) = registry.services.get_mut(&token) else {
        return SakuraOutputShadowStatus::InvalidHandle;
    };
    // A future fallible mutation must never leave a partially updated model
    // paired with bytes from the prior revision. Invalidating at the copied
    // request boundary is conservative for rejected/replayed operations and
    // keeps panic recovery fail-closed without changing their result.
    service.snapshot_cache = None;
    let operation_result = match service.try_apply(request) {
        Ok(result) => result,
        Err(status) => return status,
    };
    let status = if operation_result.status == SakuraOutputShadowOperationStatus::Stopped {
        SakuraOutputShadowStatus::Stopped
    } else {
        SakuraOutputShadowStatus::Ok
    };
    // SAFETY: The output pointer was validated above and remains caller
    // owned for this call.
    unsafe { result.write(operation_result.to_ffi()) };
    status
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_apply_v1(
    token: u64,
    request: *const SakuraOutputShadowRequestV1,
    result: *mut SakuraOutputShadowApplyResultV1,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_apply_v1(token, request, result) }
    })
}

/// Measures the deterministic canonical snapshot stream.
///
/// # Safety
///
/// `info` must point to writable V1 storage for the duration of this call.
pub(crate) unsafe fn model_snapshot_measure_v1(
    token: u64,
    info: *mut SakuraOutputShadowSnapshotInfoV1,
) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(info.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null and aligned.
    unsafe { info.write(poison_info()) };
    let mut registry = lock_registry();
    let Some(service) = registry.services.get_mut(&token) else {
        return SakuraOutputShadowStatus::InvalidHandle;
    };
    let bytes = service.snapshot_bytes();
    let encoded_size = match u64::try_from(bytes.len()) {
        Ok(value) => value,
        Err(_) => return SakuraOutputShadowStatus::InternalError,
    };
    let value = SakuraOutputShadowSnapshotInfoV1 {
        struct_size: size_of::<SakuraOutputShadowSnapshotInfoV1>() as u32,
        abi_version: ABI_VERSION_V1,
        revision: service.revision,
        stopped: u8::from(service.stopped),
        active_channel_present: u8::from(service.active_channel_id.is_some()),
        reserved0: [0; 6],
        dropped_notification_count: service.dropped_notification_count,
        channel_count: service.channels.len() as u64,
        encoded_size,
        reserved: [0; 2],
    };
    // SAFETY: The output pointer was validated above.
    unsafe { info.write(value) };
    SakuraOutputShadowStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_snapshot_measure_v1(
    token: u64,
    info: *mut SakuraOutputShadowSnapshotInfoV1,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_snapshot_measure_v1(token, info) }
    })
}

/// Writes a measured canonical snapshot without retaining the destination.
/// The stream begins with `SAKURA_OUTPUT_MODEL_V1\0`, then uses little-endian
/// fixed-width integers and length-prefixed byte fields. BTreeMap iteration
/// makes channel order deterministic by channel ID.
///
/// # Safety
///
/// `buffer` must point to writable V1 storage. On success its `data` span must
/// reference writable storage of at least `capacity` bytes.
pub(crate) unsafe fn model_snapshot_write_v1(
    token: u64,
    buffer: *mut SakuraOutputShadowSnapshotBufferV1,
) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(buffer.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The output structure pointer is non-null and aligned.
    unsafe { (*buffer).length = u64::MAX };
    // SAFETY: The structure pointer was validated above; copy it before
    // taking the registry lock so no caller pointer is retained.
    let descriptor = unsafe { buffer.read() };
    if !validate_struct_header(
        descriptor.struct_size,
        descriptor.abi_version,
        size_of::<SakuraOutputShadowSnapshotBufferV1>(),
    ) || descriptor.reserved != [0; 2]
    {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    let Some(capacity) = checked_len(descriptor.capacity) else {
        return SakuraOutputShadowStatus::InvalidArgument;
    };
    if capacity != 0
        && (descriptor.data.is_null()
            || !is_aligned(descriptor.data)
            || capacity > isize::MAX as usize
            || (descriptor.data as usize)
                .checked_add(capacity)
                .is_none_or(|end| end > isize::MAX as usize))
    {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    if ranges_overlap(
        buffer.cast(),
        size_of::<SakuraOutputShadowSnapshotBufferV1>(),
        descriptor.data.cast_const(),
        capacity,
    ) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    let mut registry = lock_registry();
    let Some(service) = registry.services.get_mut(&token) else {
        return SakuraOutputShadowStatus::InvalidHandle;
    };
    let bytes = service.snapshot_bytes();
    if capacity < bytes.len() {
        return SakuraOutputShadowStatus::InsufficientCapacity;
    }
    if !bytes.is_empty() {
        // SAFETY: Capacity and address-range checks above prove that the
        // caller supplied a writable destination large enough for the
        // canonical stream. No alias is retained after the copy.
        unsafe { ptr::copy_nonoverlapping(bytes.as_ptr(), descriptor.data, bytes.len()) };
    }
    // SAFETY: The descriptor pointer was validated above.
    unsafe { (*buffer).length = bytes.len() as u64 };
    SakuraOutputShadowStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_snapshot_write_v1(
    token: u64,
    buffer: *mut SakuraOutputShadowSnapshotBufferV1,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_snapshot_write_v1(token, buffer) }
    })
}

/// Copies the current active-channel identifier into a caller-owned bounded
/// byte span.  This is an O(identifier length) post-commit metadata query and
/// never retains the destination pointer.
///
/// # Safety
///
/// `active` must point to writable V1 storage.  When `capacity` is nonzero,
/// `data` must point to writable storage for that many bytes.
pub(crate) unsafe fn read_active_channel_internal(
    token: u64,
    active: *mut SakuraOutputShadowActiveChannelV1,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        if !is_valid_pointer(active.cast_const()) {
            return SakuraOutputShadowStatus::InvalidArgument;
        }
        // SAFETY: The descriptor pointer was validated above and is copied
        // before any registry lock is acquired.
        let descriptor = unsafe { active.read() };
        // SAFETY: The output descriptor is caller-owned and was validated
        // above. Poisoning it makes every rejected call fail closed while the
        // copied descriptor preserves the caller-owned destination metadata.
        unsafe { active.write(poison_active_channel()) };
        if !validate_struct_header(
            descriptor.struct_size,
            descriptor.abi_version,
            size_of::<SakuraOutputShadowActiveChannelV1>(),
        ) || descriptor.reserved0 != [0; 7]
            || descriptor.reserved != [0; 2]
        {
            return SakuraOutputShadowStatus::InvalidArgument;
        }
        let capacity = match checked_len(descriptor.capacity) {
            Some(value) => value,
            None => return SakuraOutputShadowStatus::InvalidArgument,
        };
        if capacity != 0
            && (descriptor.data.is_null()
                || !is_aligned(descriptor.data)
                || capacity > isize::MAX as usize
                || (descriptor.data as usize)
                    .checked_add(capacity)
                    .is_none_or(|end| end > isize::MAX as usize))
        {
            return SakuraOutputShadowStatus::InvalidArgument;
        }
        if ranges_overlap(
            active.cast(),
            size_of::<SakuraOutputShadowActiveChannelV1>(),
            descriptor.data.cast_const(),
            capacity,
        ) {
            return SakuraOutputShadowStatus::InvalidArgument;
        }
        let registry = lock_registry();
        let Some(service) = registry.services.get(&token) else {
            return SakuraOutputShadowStatus::InvalidHandle;
        };
        let identifier = service.active_channel_id.as_deref().unwrap_or_default();
        if capacity < identifier.len() {
            let required = SakuraOutputShadowActiveChannelV1 {
                struct_size: size_of::<SakuraOutputShadowActiveChannelV1>() as u32,
                abi_version: ABI_VERSION_V1,
                revision: service.revision,
                present: u8::from(service.active_channel_id.is_some()),
                reserved0: [0; 7],
                data: descriptor.data,
                capacity: descriptor.capacity,
                length: identifier.len() as u64,
                reserved: [0; 2],
            };
            // SAFETY: The descriptor pointer remains caller-owned.  Returning
            // the required bounded length lets the caller allocate exactly the
            // small active-id buffer without exposing retained model memory.
            unsafe { active.write(required) };
            return SakuraOutputShadowStatus::InsufficientCapacity;
        }
        if !identifier.is_empty() {
            // SAFETY: The destination capacity and address range were checked
            // above and the source is Rust-owned for the call.
            unsafe {
                ptr::copy_nonoverlapping(identifier.as_ptr(), descriptor.data, identifier.len())
            };
        }
        let value = SakuraOutputShadowActiveChannelV1 {
            struct_size: size_of::<SakuraOutputShadowActiveChannelV1>() as u32,
            abi_version: ABI_VERSION_V1,
            revision: service.revision,
            present: u8::from(service.active_channel_id.is_some()),
            reserved0: [0; 7],
            data: descriptor.data,
            capacity: descriptor.capacity,
            length: identifier.len() as u64,
            reserved: [0; 2],
        };
        // SAFETY: The descriptor pointer remains caller-owned for this call.
        unsafe { active.write(value) };
        SakuraOutputShadowStatus::Ok
    })
}

/// Stops one shadow. Stop is idempotent, clears all channels/tombstones and
/// makes later apply calls return a typed `Stopped` result.
///
/// # Safety
///
/// `result` must point to writable V1 storage for the duration of this call.
pub(crate) unsafe fn model_stop_v1(
    token: u64,
    result: *mut SakuraOutputShadowApplyResultV1,
) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(result.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The output pointer is non-null and aligned.
    unsafe { result.write(poison_result()) };
    let mut registry = lock_registry();
    let Some(service) = registry.services.get_mut(&token) else {
        return SakuraOutputShadowStatus::InvalidHandle;
    };
    // SAFETY: The output pointer was validated above.
    unsafe { result.write(service.stop().to_ffi()) };
    SakuraOutputShadowStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_stop_v1(
    token: u64,
    result: *mut SakuraOutputShadowApplyResultV1,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_stop_v1(token, result) }
    })
}

/// Destroys one numeric token. Successful destruction consumes and zeros the
/// caller's token; stale or repeated tokens return `InvalidHandle`.
///
/// # Safety
///
/// `token` must point to writable `u64` storage for the duration of this call.
pub(crate) unsafe fn model_destroy_v1(token: *mut u64) -> SakuraOutputShadowStatus {
    if !is_valid_pointer(token.cast_const()) {
        return SakuraOutputShadowStatus::InvalidArgument;
    }
    // SAFETY: The pointer is non-null and aligned; caller owns it for this
    // call and the value is copied before touching the registry.
    let value = unsafe { token.read() };
    if value == 0 {
        return SakuraOutputShadowStatus::InvalidHandle;
    }
    let mut registry = lock_registry();
    if registry.services.remove(&value).is_none() {
        return SakuraOutputShadowStatus::InvalidHandle;
    }
    // SAFETY: The pointer was validated above and remains caller-owned.
    unsafe { token.write(0) };
    SakuraOutputShadowStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sakura_output_shadow_destroy_v1(
    token: *mut u64,
) -> SakuraOutputShadowStatus {
    catch_status(|| {
        // SAFETY: This wrapper has the same caller contract as the model call.
        unsafe { model_destroy_v1(token) }
    })
}

fn is_valid_owner(owner: &Owner) -> bool {
    owner.generation != 0 && is_valid_stable_id(&owner.id)
}

fn is_valid_operation_id(value: &[u8]) -> bool {
    is_valid_stable_id(value)
}

fn is_valid_stable_id(value: &[u8]) -> bool {
    !value.is_empty()
        && value.len() <= MAX_STABLE_ID_BYTES
        && is_valid_utf8(value, false)
        && value.iter().all(|byte| *byte > 0x20 && *byte != 0x7f)
}

fn is_valid_utf8(value: &[u8], permit_controls: bool) -> bool {
    let Ok(value) = str::from_utf8(value) else {
        return false;
    };
    value.chars().all(|character| {
        if character == '\0' {
            return false;
        }
        if !permit_controls && (character < '\u{20}' || character == '\u{7f}') {
            return false;
        }
        permit_controls || !(('\u{80}'..='\u{9f}').contains(&character))
    })
}

fn is_valid_bounded_text(value: &[u8], maximum_bytes: usize) -> bool {
    value.len() <= maximum_bytes && is_valid_utf8(value, true)
}

fn is_valid_metadata_value(value: &Option<Vec<u8>>) -> bool {
    value.as_ref().is_none_or(|value| {
        !value.is_empty() && value.len() <= MAX_METADATA_BYTES && is_valid_utf8(value, false)
    })
}

fn is_valid_metadata(metadata: &Metadata) -> bool {
    is_valid_metadata_value(&metadata.language_id) && is_valid_metadata_value(&metadata.source)
}

fn character_count(value: &[u8]) -> u64 {
    value.iter().filter(|byte| **byte & 0xc0 != 0x80).count() as u64
}

fn saturating_add(value: &mut u64, amount: u64) {
    *value = value.saturating_add(amount);
}

fn leading_boundary(value: &[u8], mut byte_count: usize) -> usize {
    byte_count = byte_count.min(value.len());
    while byte_count < value.len() && value[byte_count] & 0xc0 == 0x80 {
        byte_count += 1;
    }
    byte_count
}

fn keep_suffix(value: &mut Vec<u8>, maximum_bytes: usize, dropped_characters: &mut u64) {
    if value.len() <= maximum_bytes {
        return;
    }
    let removed = leading_boundary(value, value.len() - maximum_bytes);
    saturating_add(dropped_characters, character_count(&value[..removed]));
    value.drain(..removed);
}

fn log_level_text(level: u32) -> &'static [u8] {
    match level {
        LOG_TRACE => b"Trace",
        LOG_DEBUG => b"Debug",
        LOG_WARNING => b"Warning",
        LOG_ERROR => b"Error",
        _ => b"Info",
    }
}

fn render_log_entry(entry: &LogEntry) -> Vec<u8> {
    let mut output = Vec::new();
    output.extend_from_slice(b"[");
    output.extend_from_slice(log_level_text(entry.level));
    output.extend_from_slice(b"] ");
    if let Some(source) = &entry.source {
        output.extend_from_slice(source);
        output.extend_from_slice(b": ");
    }
    output.extend_from_slice(&entry.message);
    output.push(b'\n');
    output
}

fn rebuild_log_projection(channel: &mut Channel, maximum_bytes: usize) {
    let mut projected_bytes = 0_usize;
    for entry in &channel.log_entries {
        projected_bytes += render_log_entry(entry).len();
    }
    while projected_bytes > maximum_bytes && !channel.log_entries.is_empty() {
        if let Some(entry) = channel.log_entries.pop_front() {
            let rendered = render_log_entry(&entry);
            saturating_add(
                &mut channel.dropped_character_count,
                character_count(&rendered),
            );
            projected_bytes -= rendered.len();
        }
    }
    let mut projected = Vec::with_capacity(projected_bytes);
    for entry in &channel.log_entries {
        projected.extend_from_slice(&render_log_entry(entry));
    }
    channel.projected_text = projected;
}

fn put_u32(target: &mut Vec<u8>, value: u32) {
    target.extend_from_slice(&value.to_le_bytes());
}

fn put_u64(target: &mut Vec<u8>, value: u64) {
    target.extend_from_slice(&value.to_le_bytes());
}

fn put_bytes(target: &mut Vec<u8>, value: &[u8]) {
    put_u64(target, value.len() as u64);
    target.extend_from_slice(value);
}

fn put_optional_bytes(target: &mut Vec<u8>, value: Option<&[u8]>) {
    match value {
        Some(value) => {
            target.push(1);
            put_bytes(target, value);
        }
        None => target.push(0),
    }
}

fn append_fingerprint_bytes(target: &mut Vec<u8>, value: &[u8]) {
    put_bytes(target, value);
}

fn append_fingerprint_operation(target: &mut Vec<u8>, operation: &Operation) {
    append_fingerprint_bytes(target, &operation.id);
    match operation.expected_revision {
        Some(revision) => {
            target.push(1);
            put_u64(target, revision);
        }
        None => target.push(0),
    }
}

fn append_fingerprint_owner(target: &mut Vec<u8>, owner: &Owner) {
    append_fingerprint_bytes(target, &owner.id);
    put_u64(target, owner.generation);
}

fn fingerprint_bytes_capacity(value: &[u8]) -> Option<usize> {
    size_of::<u64>().checked_add(value.len())
}

fn fingerprint_optional_bytes_capacity(value: Option<&[u8]>) -> Option<usize> {
    1_usize.checked_add(match value {
        Some(value) => fingerprint_bytes_capacity(value)?,
        None => 0,
    })
}

fn fingerprint_operation_capacity(operation: &Operation) -> Option<usize> {
    fingerprint_bytes_capacity(&operation.id)?
        .checked_add(1)?
        .checked_add(if operation.expected_revision.is_some() {
            size_of::<u64>()
        } else {
            0
        })
}

fn fingerprint_owner_capacity(owner: &Owner) -> Option<usize> {
    fingerprint_bytes_capacity(&owner.id)?.checked_add(size_of::<u64>())
}

fn fingerprint_header_capacity(
    tag: &[u8],
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
) -> Option<usize> {
    tag.len()
        .checked_add(fingerprint_operation_capacity(operation)?)?
        .checked_add(fingerprint_owner_capacity(owner)?)?
        .checked_add(fingerprint_bytes_capacity(channel_id)?)
}

fn fingerprint_buffer(capacity: usize) -> Vec<u8> {
    #[cfg(test)]
    FINGERPRINT_CONSTRUCTION_COUNT.with(|count| count.set(count.get() + 1));
    Vec::with_capacity(capacity)
}

fn fingerprint_create_channel_capacity(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    label: &[u8],
    metadata: &Metadata,
) -> Option<usize> {
    let mut capacity = fingerprint_header_capacity(b"create;", operation, owner, channel_id)?;
    capacity = capacity.checked_add(fingerprint_bytes_capacity(label)?)?;
    capacity = capacity.checked_add(1)?;
    capacity = capacity.checked_add(fingerprint_optional_bytes_capacity(
        metadata.language_id.as_deref(),
    )?)?;
    capacity = capacity.checked_add(fingerprint_optional_bytes_capacity(
        metadata.source.as_deref(),
    )?)?;
    Some(capacity)
}

fn fingerprint_text_capacity(
    tag: &[u8],
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    text: &[u8],
) -> Option<usize> {
    fingerprint_header_capacity(tag, operation, owner, channel_id)?
        .checked_add(fingerprint_bytes_capacity(text)?)
}

fn fingerprint_append_log_capacity(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    entries: &[LogEntry],
) -> Option<usize> {
    let mut capacity = fingerprint_header_capacity(b"append-log;", operation, owner, channel_id)?;
    for entry in entries {
        capacity = capacity.checked_add(size_of::<u32>())?;
        capacity = capacity.checked_add(fingerprint_bytes_capacity(&entry.message)?)?;
        capacity = capacity.checked_add(fingerprint_optional_bytes_capacity(
            entry.source.as_deref(),
        )?)?;
    }
    Some(capacity)
}

fn fingerprint_channel_capacity(
    tag: &[u8],
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    include_preserve_focus: bool,
) -> Option<usize> {
    fingerprint_header_capacity(tag, operation, owner, channel_id)?
        .checked_add(usize::from(include_preserve_focus))
}

fn fingerprint_dispose_owner_capacity(operation: &Operation, owner: &Owner) -> Option<usize> {
    b"dispose-owner;"
        .len()
        .checked_add(fingerprint_operation_capacity(operation)?)?
        .checked_add(fingerprint_owner_capacity(owner)?)
}

fn append_fingerprint_header(
    target: &mut Vec<u8>,
    tag: &[u8],
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
) {
    target.extend_from_slice(tag);
    append_fingerprint_operation(target, operation);
    append_fingerprint_owner(target, owner);
    append_fingerprint_bytes(target, channel_id);
}

fn fingerprint_create_channel(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    label: &[u8],
    kind: u8,
    metadata: &Metadata,
) -> Vec<u8> {
    let capacity =
        fingerprint_create_channel_capacity(operation, owner, channel_id, label, metadata)
            .expect("fingerprint capacity must fit usize");
    let mut output = fingerprint_buffer(capacity);
    append_fingerprint_header(&mut output, b"create;", operation, owner, channel_id);
    append_fingerprint_bytes(&mut output, label);
    output.push(kind);
    put_optional_bytes(&mut output, metadata.language_id.as_deref());
    put_optional_bytes(&mut output, metadata.source.as_deref());
    output
}

fn fingerprint_text(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    text: &[u8],
    replace: bool,
) -> Vec<u8> {
    let tag = if replace {
        b"replace-output;".as_slice()
    } else {
        b"append-output;".as_slice()
    };
    let capacity = fingerprint_text_capacity(tag, operation, owner, channel_id, text)
        .expect("fingerprint capacity must fit usize");
    let mut output = fingerprint_buffer(capacity);
    append_fingerprint_header(&mut output, tag, operation, owner, channel_id);
    append_fingerprint_bytes(&mut output, text);
    output
}

fn fingerprint_append_log(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    entries: &[LogEntry],
) -> Vec<u8> {
    let capacity = fingerprint_append_log_capacity(operation, owner, channel_id, entries)
        .expect("fingerprint capacity must fit usize");
    let mut output = fingerprint_buffer(capacity);
    append_fingerprint_header(&mut output, b"append-log;", operation, owner, channel_id);
    for entry in entries {
        put_u32(&mut output, entry.level);
        append_fingerprint_bytes(&mut output, &entry.message);
        put_optional_bytes(&mut output, entry.source.as_deref());
    }
    output
}

fn fingerprint_channel(
    operation: &Operation,
    owner: &Owner,
    channel_id: &[u8],
    kind: u32,
    preserve_focus: bool,
) -> Vec<u8> {
    let tag = match kind {
        OP_CLEAR => b"clear;".as_slice(),
        OP_SHOW => b"show;".as_slice(),
        OP_HIDE => b"hide;".as_slice(),
        OP_DISPOSE => b"dispose;".as_slice(),
        _ => b"channel;".as_slice(),
    };
    let capacity = fingerprint_channel_capacity(tag, operation, owner, channel_id, kind == OP_SHOW)
        .expect("fingerprint capacity must fit usize");
    let mut output = fingerprint_buffer(capacity);
    append_fingerprint_header(&mut output, tag, operation, owner, channel_id);
    if kind == OP_SHOW {
        output.push(u8::from(preserve_focus));
    }
    output
}

fn fingerprint_dispose_owner(operation: &Operation, owner: &Owner) -> Vec<u8> {
    let capacity = fingerprint_dispose_owner_capacity(operation, owner)
        .expect("fingerprint capacity must fit usize");
    let mut output = fingerprint_buffer(capacity);
    output.extend_from_slice(b"dispose-owner;");
    append_fingerprint_operation(&mut output, operation);
    append_fingerprint_owner(&mut output, owner);
    output
}

fn fingerprint(request: &Request) -> Vec<u8> {
    match request {
        Request::CreateChannel {
            operation,
            owner,
            channel_id,
            label,
            kind,
            metadata,
        } => fingerprint_create_channel(operation, owner, channel_id, label, *kind, metadata),
        Request::Text {
            operation,
            owner,
            channel_id,
            text,
            replace,
        } => fingerprint_text(operation, owner, channel_id, text, *replace),
        Request::AppendLog {
            operation,
            owner,
            channel_id,
            entries,
        } => fingerprint_append_log(operation, owner, channel_id, entries),
        Request::Channel {
            operation,
            owner,
            channel_id,
            kind,
            preserve_focus,
        } => fingerprint_channel(operation, owner, channel_id, *kind, *preserve_focus),
        Request::DisposeOwner { operation, owner } => fingerprint_dispose_owner(operation, owner),
    }
}

#[cfg(test)]
fn fingerprint_capacity(request: &Request) -> Option<usize> {
    match request {
        Request::CreateChannel {
            operation,
            owner,
            channel_id,
            label,
            metadata,
            ..
        } => fingerprint_create_channel_capacity(operation, owner, channel_id, label, metadata),
        Request::Text {
            operation,
            owner,
            channel_id,
            text,
            replace,
        } => fingerprint_text_capacity(
            if *replace {
                b"replace-output;"
            } else {
                b"append-output;"
            },
            operation,
            owner,
            channel_id,
            text,
        ),
        Request::AppendLog {
            operation,
            owner,
            channel_id,
            entries,
        } => fingerprint_append_log_capacity(operation, owner, channel_id, entries),
        Request::Channel {
            operation,
            owner,
            channel_id,
            kind,
            ..
        } => {
            let tag = match *kind {
                OP_CLEAR => b"clear;".as_slice(),
                OP_SHOW => b"show;".as_slice(),
                OP_HIDE => b"hide;".as_slice(),
                OP_DISPOSE => b"dispose;".as_slice(),
                _ => b"channel;".as_slice(),
            };
            fingerprint_channel_capacity(tag, operation, owner, channel_id, *kind == OP_SHOW)
        }
        Request::DisposeOwner { operation, owner } => {
            fingerprint_dispose_owner_capacity(operation, owner)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits() -> Limits {
        Limits {
            maximum_owners: 4,
            maximum_channels: 4,
            maximum_text_bytes_per_channel: 4,
            maximum_payload_bytes: 16,
            maximum_log_entries_per_channel: 2,
            maximum_remembered_operations: 4,
        }
    }

    fn owner(generation: u64) -> Owner {
        Owner {
            id: b"owner".to_vec(),
            generation,
        }
    }

    fn operation(id: &str) -> Operation {
        Operation {
            id: Arc::from(id.as_bytes()),
            expected_revision: None,
        }
    }

    fn assert_remember_invariant(service: &Service) {
        assert_eq!(
            service.completed_operations.len(),
            service.completed_operation_order.len()
        );
        assert!(service
            .completed_operation_order
            .iter()
            .all(|operation_id| service.completed_operations.contains_key(operation_id)));
        assert!(service.completed_operations.len() <= service.limits.maximum_remembered_operations);
    }

    fn span(data: *const u8, length: usize) -> SakuraOutputShadowSpanV1 {
        SakuraOutputShadowSpanV1 {
            struct_size: size_of::<SakuraOutputShadowSpanV1>() as u32,
            abi_version: ABI_VERSION_V1,
            data,
            length: length as u64,
            reserved: [0; 2],
        }
    }

    fn abi_create_request(operation_id: &[u8], channel_id: &[u8]) -> SakuraOutputShadowRequestV1 {
        let owner_id = b"owner";
        let label = b"Label";
        SakuraOutputShadowRequestV1 {
            struct_size: size_of::<SakuraOutputShadowRequestV1>() as u32,
            abi_version: ABI_VERSION_V1,
            operation_kind: OP_CREATE_CHANNEL,
            channel_kind: u32::from(CHANNEL_KIND_OUTPUT),
            flags: 0,
            operation_id: span(operation_id.as_ptr(), operation_id.len()),
            expected_revision: 0,
            owner_id: span(owner_id.as_ptr(), owner_id.len()),
            owner_generation: 1,
            channel_id: span(channel_id.as_ptr(), channel_id.len()),
            label: span(label.as_ptr(), label.len()),
            metadata_language_id: span(ptr::null(), 0),
            metadata_source: span(ptr::null(), 0),
            payload: span(ptr::null(), 0),
            log_entries: ptr::null(),
            log_entry_count: 0,
            reserved: [0; 4],
        }
    }

    fn create(id: &str, generation: u64, channel: &str, kind: u8) -> Request {
        Request::CreateChannel {
            operation: operation(id),
            owner: owner(generation),
            channel_id: channel.as_bytes().to_vec(),
            label: b"Label".to_vec(),
            kind,
            metadata: Metadata {
                language_id: None,
                source: None,
            },
        }
    }

    fn channel_request(id: &str, channel: &str, kind: u32) -> Request {
        Request::Channel {
            operation: operation(id),
            owner: owner(1),
            channel_id: channel.as_bytes().to_vec(),
            kind,
            preserve_focus: true,
        }
    }

    fn channel_request_with_values(
        operation: Operation,
        owner: Owner,
        kind: u32,
        preserve_focus: bool,
    ) -> Request {
        Request::Channel {
            operation,
            owner,
            channel_id: b"chan".to_vec(),
            kind,
            preserve_focus,
        }
    }

    fn reset_fingerprint_count() {
        FINGERPRINT_CONSTRUCTION_COUNT.with(|count| count.set(0));
    }

    fn fingerprint_count() -> usize {
        FINGERPRINT_CONSTRUCTION_COUNT.with(Cell::get)
    }

    fn fingerprint_hex(request: &Request, expected: &str) {
        let expected = expected
            .split_whitespace()
            .map(|byte| u8::from_str_radix(byte, 16).expect("valid fingerprint hex"))
            .collect::<Vec<_>>();
        assert_eq!(Some(expected.len()), fingerprint_capacity(request));
        assert_eq!(expected, fingerprint(request));
    }

    fn assert_poisoned_result(result: &SakuraOutputShadowApplyResultV1) {
        let poison = poison_result();
        assert_eq!(poison.struct_size, result.struct_size);
        assert_eq!(poison.abi_version, result.abi_version);
        assert_eq!(poison.status, result.status);
        assert_eq!(poison.reason, result.reason);
        assert_eq!(poison.revision, result.revision);
        assert_eq!(
            poison.callback_drain_deferred,
            result.callback_drain_deferred
        );
        assert_eq!(poison.reserved, result.reserved);
    }

    #[test]
    fn unknown_ids_defer_fingerprint_until_semantically_accepted() {
        reset_fingerprint_count();
        let mut service = Service::new(limits());

        let rejected = Request::CreateChannel {
            operation: operation("rejected"),
            owner: owner(0),
            channel_id: b"rejected-channel".to_vec(),
            label: b"Label".to_vec(),
            kind: CHANNEL_KIND_OUTPUT,
            metadata: Metadata {
                language_id: None,
                source: None,
            },
        };
        assert_eq!(
            SakuraOutputShadowOperationStatus::Rejected,
            service.apply(rejected).status
        );
        assert_eq!(0, fingerprint_count());
        assert!(!service
            .completed_operations
            .contains_key(b"rejected".as_slice()));

        let mut stale = create("stale", 1, "stale-channel", CHANNEL_KIND_OUTPUT);
        if let Request::CreateChannel { operation, .. } = &mut stale {
            operation.expected_revision = Some(service.revision + 1);
        }
        assert_eq!(
            SakuraOutputShadowOperationStatus::StaleRevision,
            service.apply(stale).status
        );
        assert_eq!(0, fingerprint_count());
        assert!(!service
            .completed_operations
            .contains_key(b"stale".as_slice()));

        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("seed", 1, "seed-channel", CHANNEL_KIND_OUTPUT))
                .status
        );
        reset_fingerprint_count();
        let not_applicable = Request::Text {
            operation: operation("not-applicable"),
            owner: owner(1),
            channel_id: b"seed-channel".to_vec(),
            text: Vec::new(),
            replace: false,
        };
        assert_eq!(
            SakuraOutputShadowOperationStatus::NotApplicable,
            service.apply(not_applicable).status
        );
        assert_eq!(0, fingerprint_count());
        assert!(!service
            .completed_operations
            .contains_key(b"not-applicable".as_slice()));

        let accepted = create("accepted", 1, "accepted-channel", CHANNEL_KIND_OUTPUT);
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.apply(accepted).status
        );
        assert_eq!(1, fingerprint_count());
        assert!(service
            .completed_operations
            .contains_key(b"accepted".as_slice()));
    }

    #[test]
    fn known_ids_still_fingerprint_before_stale_and_semantic_checks() {
        reset_fingerprint_count();
        let mut service = Service::new(limits());
        let mut request = create("known", 1, "known-channel", CHANNEL_KIND_OUTPUT);
        if let Request::CreateChannel { operation, .. } = &mut request {
            operation.expected_revision = Some(1);
        }
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.apply(request.clone()).status
        );
        assert_eq!(1, fingerprint_count());

        // The expected revision is now stale, but a byte-identical known ID
        // still replays before that check.
        assert_eq!(
            SakuraOutputShadowOperationStatus::Replayed,
            service.apply(request.clone()).status
        );
        assert_eq!(2, fingerprint_count());

        // An altered request would fail semantic validation as well as stale
        // revision validation, but a known ID must report a conflict first.
        let conflict = match request {
            Request::CreateChannel {
                operation,
                owner,
                channel_id,
                kind,
                metadata,
                ..
            } => Request::CreateChannel {
                operation,
                owner,
                channel_id,
                label: Vec::new(),
                kind,
                metadata,
            },
            _ => unreachable!(),
        };
        let result = service.apply(conflict);
        assert_eq!(SakuraOutputShadowOperationStatus::Conflict, result.status);
        assert_eq!(SakuraOutputShadowReason::OperationIdConflict, result.reason);
        assert_eq!(3, fingerprint_count());
    }

    #[test]
    fn eviction_allows_a_fresh_operation_with_the_evicted_id() {
        reset_fingerprint_count();
        let mut constrained = limits();
        constrained.maximum_remembered_operations = 1;
        let mut service = Service::new(constrained);

        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("first", 1, "first-channel", CHANNEL_KIND_OUTPUT))
                .status
        );
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("second", 1, "second-channel", CHANNEL_KIND_OUTPUT))
                .status
        );
        assert!(!service
            .completed_operations
            .contains_key(b"first".as_slice()));
        assert!(service
            .completed_operations
            .contains_key(b"second".as_slice()));
        assert_remember_invariant(&service);

        fail_next_reserve(ReserveFailurePoint::Map);
        let fresh = create("first", 1, "fresh-channel", CHANNEL_KIND_OUTPUT);
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.apply(fresh).status
        );
        assert!(service
            .completed_operations
            .contains_key(b"first".as_slice()));
        assert!(!service
            .completed_operations
            .contains_key(b"second".as_slice()));
        assert_eq!(3, fingerprint_count());
        assert_remember_invariant(&service);
    }

    #[test]
    fn every_operation_kind_is_accepted_and_remembered_once() {
        reset_fingerprint_count();
        let mut service = Service::new(limits());

        let mut apply_success = |request| {
            let result = service.apply(request);
            assert_eq!(SakuraOutputShadowOperationStatus::Succeeded, result.status);
            result
        };

        apply_success(create("create-output", 1, "output", CHANNEL_KIND_OUTPUT));
        apply_success(Request::Text {
            operation: operation("append-output"),
            owner: owner(1),
            channel_id: b"output".to_vec(),
            text: b"app".to_vec(),
            replace: false,
        });
        apply_success(Request::Text {
            operation: operation("replace-output"),
            owner: owner(1),
            channel_id: b"output".to_vec(),
            text: b"rep".to_vec(),
            replace: true,
        });
        apply_success(create("create-log", 1, "log", CHANNEL_KIND_LOG));
        apply_success(Request::AppendLog {
            operation: operation("append-log"),
            owner: owner(1),
            channel_id: b"log".to_vec(),
            entries: vec![LogEntry {
                level: LOG_INFO,
                message: b"entry".to_vec(),
                source: None,
            }],
        });
        apply_success(channel_request("clear", "log", OP_CLEAR));
        apply_success(channel_request("show", "log", OP_SHOW));
        apply_success(channel_request("hide", "log", OP_HIDE));
        apply_success(channel_request("dispose", "log", OP_DISPOSE));
        apply_success(Request::DisposeOwner {
            operation: operation("dispose-owner"),
            owner: owner(1),
        });

        assert_eq!(10, fingerprint_count());
        assert_eq!(11, service.revision);
        assert!(service.channels.is_empty());
        assert!(service
            .completed_operations
            .contains_key(b"dispose-owner".as_slice()));
        assert_remember_invariant(&service);
    }

    #[test]
    fn canonical_fingerprint_bytes_are_stable_for_every_request_kind() {
        let operation_with_revision = Operation {
            id: Arc::from(&b"op"[..]),
            expected_revision: Some(9),
        };
        let owner_with_generation = Owner {
            id: b"owner".to_vec(),
            generation: 7,
        };
        let metadata = Metadata {
            language_id: Some(b"ja".to_vec()),
            source: Some(b"src".to_vec()),
        };
        fingerprint_hex(
            &Request::CreateChannel {
                operation: operation_with_revision.clone(),
                owner: owner_with_generation.clone(),
                channel_id: b"chan".to_vec(),
                label: b"Label".to_vec(),
                kind: CHANNEL_KIND_LOG,
                metadata: metadata.clone(),
            },
            "63 72 65 61 74 65 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e 05 00 00 00 00 00 00 00 4c 61 62 65 6c 01 01 02 00 00 00 00 00 00 00 6a 61 01 03 00 00 00 00 00 00 00 73 72 63",
        );

        let append = Request::Text {
            operation: operation_with_revision.clone(),
            owner: owner_with_generation.clone(),
            channel_id: b"chan".to_vec(),
            text: b"txt".to_vec(),
            replace: false,
        };
        fingerprint_hex(
            &append,
            "61 70 70 65 6e 64 2d 6f 75 74 70 75 74 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e 03 00 00 00 00 00 00 00 74 78 74",
        );
        fingerprint_hex(
            &Request::Text {
                operation: operation_with_revision.clone(),
                owner: owner_with_generation.clone(),
                channel_id: b"chan".to_vec(),
                text: b"txt".to_vec(),
                replace: true,
            },
            "72 65 70 6c 61 63 65 2d 6f 75 74 70 75 74 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e 03 00 00 00 00 00 00 00 74 78 74",
        );

        let logs = Request::AppendLog {
            operation: operation_with_revision.clone(),
            owner: owner_with_generation.clone(),
            channel_id: b"chan".to_vec(),
            entries: vec![
                LogEntry {
                    level: LOG_INFO,
                    message: b"m".to_vec(),
                    source: None,
                },
                LogEntry {
                    level: LOG_ERROR,
                    message: b"err".to_vec(),
                    source: Some(b"s".to_vec()),
                },
            ],
        };
        fingerprint_hex(
            &logs,
            "61 70 70 65 6e 64 2d 6c 6f 67 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e 02 00 00 00 01 00 00 00 00 00 00 00 6d 00 04 00 00 00 03 00 00 00 00 00 00 00 65 72 72 01 01 00 00 00 00 00 00 00 73",
        );

        for (kind, expected) in [
            (OP_CLEAR, "63 6c 65 61 72 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e"),
            (OP_SHOW, "73 68 6f 77 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e 01"),
            (OP_HIDE, "68 69 64 65 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e"),
            (OP_DISPOSE, "64 69 73 70 6f 73 65 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00 04 00 00 00 00 00 00 00 63 68 61 6e"),
        ] {
            fingerprint_hex(
                &channel_request_with_values(
                    operation_with_revision.clone(),
                    owner_with_generation.clone(),
                    kind,
                    true,
                ),
                expected,
            );
        }

        fingerprint_hex(
            &Request::DisposeOwner {
                operation: operation_with_revision,
                owner: owner_with_generation,
            },
            "64 69 73 70 6f 73 65 2d 6f 77 6e 65 72 3b 02 00 00 00 00 00 00 00 6f 70 01 09 00 00 00 00 00 00 00 05 00 00 00 00 00 00 00 6f 77 6e 65 72 07 00 00 00 00 00 00 00",
        );
    }

    #[test]
    fn fixed_abi_contract_is_stable() {
        assert_eq!(1, ABI_VERSION_V1);
        assert_eq!(1, REQUEST_HAS_EXPECTED_REVISION);
        assert_eq!(2, REQUEST_PRESERVE_FOCUS);
        assert_eq!(4, REQUEST_LANGUAGE_PRESENT);
        assert_eq!(8, REQUEST_SOURCE_PRESENT);
        assert_eq!(15, REQUEST_KNOWN_FLAGS);
        assert_eq!(1, LOG_SOURCE_PRESENT);
        assert_eq!(
            [1, 2, 3, 4, 5, 6, 7, 8, 9],
            [
                OP_CREATE_CHANNEL,
                OP_APPEND_OUTPUT,
                OP_REPLACE_OUTPUT,
                OP_APPEND_LOG,
                OP_CLEAR,
                OP_SHOW,
                OP_HIDE,
                OP_DISPOSE,
                OP_DISPOSE_OWNER,
            ]
        );
        assert_eq!([0, 1], [CHANNEL_KIND_OUTPUT, CHANNEL_KIND_LOG]);
        assert_eq!(
            [0, 1, 2, 3, 4],
            [LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR]
        );
        assert_eq!(b"SAKURA_OUTPUT_MODEL_V1\0", SNAPSHOT_MAGIC);
        assert_eq!(
            [0, 1, 2, 3, 4, 5],
            [
                SakuraOutputShadowStatus::Ok as u32,
                SakuraOutputShadowStatus::InvalidArgument as u32,
                SakuraOutputShadowStatus::InvalidHandle as u32,
                SakuraOutputShadowStatus::Stopped as u32,
                SakuraOutputShadowStatus::InsufficientCapacity as u32,
                SakuraOutputShadowStatus::InternalError as u32,
            ]
        );
        assert_eq!(
            [0, 1, 2, 3, 4, 5, 6, 7],
            [
                SakuraOutputShadowOperationStatus::Succeeded as u32,
                SakuraOutputShadowOperationStatus::Replayed as u32,
                SakuraOutputShadowOperationStatus::NotApplicable as u32,
                SakuraOutputShadowOperationStatus::Rejected as u32,
                SakuraOutputShadowOperationStatus::Conflict as u32,
                SakuraOutputShadowOperationStatus::StaleRevision as u32,
                SakuraOutputShadowOperationStatus::RevisionExhausted as u32,
                SakuraOutputShadowOperationStatus::Stopped as u32,
            ]
        );
        assert_eq!(
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16],
            [
                SakuraOutputShadowReason::None as u32,
                SakuraOutputShadowReason::InvalidOperationId as u32,
                SakuraOutputShadowReason::InvalidOwner as u32,
                SakuraOutputShadowReason::InvalidChannelId as u32,
                SakuraOutputShadowReason::InvalidLabel as u32,
                SakuraOutputShadowReason::InvalidMetadata as u32,
                SakuraOutputShadowReason::InvalidPayload as u32,
                SakuraOutputShadowReason::PayloadLimitExceeded as u32,
                SakuraOutputShadowReason::OwnerLimitExceeded as u32,
                SakuraOutputShadowReason::ChannelLimitExceeded as u32,
                SakuraOutputShadowReason::TextLimitExceeded as u32,
                SakuraOutputShadowReason::LogEntryLimitExceeded as u32,
                SakuraOutputShadowReason::ChannelNotFound as u32,
                SakuraOutputShadowReason::OwnerGenerationConflict as u32,
                SakuraOutputShadowReason::ChannelKindMismatch as u32,
                SakuraOutputShadowReason::OperationIdConflict as u32,
                SakuraOutputShadowReason::ExpectedRevisionMismatch as u32,
            ]
        );
        assert_eq!(40, size_of::<SakuraOutputShadowSpanV1>());
        assert_eq!(80, size_of::<SakuraOutputShadowLimitsV1>());
        assert_eq!(112, size_of::<SakuraOutputShadowLogEntryV1>());
        assert_eq!(368, size_of::<SakuraOutputShadowRequestV1>());
        assert_eq!(32, size_of::<SakuraOutputShadowApplyResultV1>());
        assert_eq!(64, size_of::<SakuraOutputShadowSnapshotInfoV1>());
        assert_eq!(48, size_of::<SakuraOutputShadowSnapshotBufferV1>());
        assert_eq!(64, size_of::<SakuraOutputShadowActiveChannelV1>());
    }

    #[test]
    fn create_replay_conflict_and_expected_revision_are_distinct() {
        let mut service = Service::new(limits());
        let request = create("create", 1, "channel", CHANNEL_KIND_OUTPUT);
        let result = service.apply(request.clone());
        assert_eq!(SakuraOutputShadowOperationStatus::Succeeded, result.status);
        assert_eq!(2, result.revision);
        let replay = service.apply(request.clone());
        assert_eq!(SakuraOutputShadowOperationStatus::Replayed, replay.status);
        assert_eq!(result.revision, replay.revision);
        let conflict_request = match request.clone() {
            Request::CreateChannel {
                operation,
                owner,
                channel_id,
                kind,
                metadata,
                ..
            } => Request::CreateChannel {
                operation,
                owner,
                channel_id,
                label: b"Different".to_vec(),
                kind,
                metadata,
            },
            _ => unreachable!(),
        };
        let conflict = service.apply(conflict_request);
        assert_eq!(SakuraOutputShadowOperationStatus::Conflict, conflict.status);
        let mut stale = create("stale", 1, "second", CHANNEL_KIND_OUTPUT);
        if let Request::CreateChannel { operation, .. } = &mut stale {
            operation.expected_revision = Some(1);
        }
        let stale_result = service.apply(stale);
        assert_eq!(
            SakuraOutputShadowOperationStatus::StaleRevision,
            stale_result.status
        );
    }

    #[test]
    fn generation_replaces_old_channels_and_tombstone_fences_late_work() {
        let mut service = Service::new(limits());
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("c1", 1, "old", CHANNEL_KIND_OUTPUT))
                .status
        );
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("c2", 2, "new", CHANNEL_KIND_OUTPUT))
                .status
        );
        assert!(!service.channels.contains_key(b"old".as_slice()));
        assert!(service.channels.contains_key(b"new".as_slice()));
        let late = service.apply(create("late", 1, "late", CHANNEL_KIND_OUTPUT));
        assert_eq!(SakuraOutputShadowOperationStatus::Conflict, late.status);
        let disposed = service.apply(Request::DisposeOwner {
            operation: operation("dispose-owner"),
            owner: owner(2),
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            disposed.status
        );
        let late_same_generation = service.apply(create("late2", 2, "late2", CHANNEL_KIND_OUTPUT));
        assert_eq!(
            SakuraOutputShadowOperationStatus::Conflict,
            late_same_generation.status
        );
    }

    #[test]
    fn output_and_log_limits_match_trim_and_projection_rules() {
        let mut service = Service::new(limits());
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("c", 1, "out", CHANNEL_KIND_OUTPUT))
                .status
        );
        let append = Request::Text {
            operation: operation("append"),
            owner: owner(1),
            channel_id: b"out".to_vec(),
            text: "\u{e9}abc".as_bytes().to_vec(),
            replace: false,
        };
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.apply(append).status
        );
        assert_eq!(b"abc", service.channels[b"out".as_slice()].text.as_slice());
        assert_eq!(
            1,
            service.channels[b"out".as_slice()].dropped_character_count
        );

        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("log-create", 1, "log", CHANNEL_KIND_LOG))
                .status
        );
        let logs = Request::AppendLog {
            operation: operation("logs"),
            owner: owner(1),
            channel_id: b"log".to_vec(),
            entries: vec![
                LogEntry {
                    level: LOG_INFO,
                    message: b"one".to_vec(),
                    source: None,
                },
                LogEntry {
                    level: LOG_ERROR,
                    message: b"two".to_vec(),
                    source: Some(b"src".to_vec()),
                },
            ],
        };
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.apply(logs).status
        );
        assert!(
            service.channels[b"log".as_slice()].projected_text.len()
                <= limits().maximum_text_bytes_per_channel
        );
    }

    #[test]
    fn stop_is_terminal_and_snapshot_is_deterministic() {
        let mut first = Service::new(limits());
        let mut second = Service::new(limits());
        for service in [&mut first, &mut second] {
            assert_eq!(
                SakuraOutputShadowOperationStatus::Succeeded,
                service
                    .apply(create("c", 1, "channel", CHANNEL_KIND_OUTPUT))
                    .status
            );
            assert_eq!(
                SakuraOutputShadowOperationStatus::Succeeded,
                service
                    .apply(Request::Text {
                        operation: operation("text"),
                        owner: owner(1),
                        channel_id: b"channel".to_vec(),
                        text: b"text".to_vec(),
                        replace: true,
                    })
                    .status
            );
        }
        assert_eq!(first.snapshot_bytes(), second.snapshot_bytes());
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            first.stop().status
        );
        assert_eq!(
            SakuraOutputShadowOperationStatus::Stopped,
            first.apply(create("x", 1, "x", CHANNEL_KIND_OUTPUT)).status
        );
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            first.stop().status
        );
        assert!(first.snapshot_bytes().starts_with(SNAPSHOT_MAGIC));
    }

    #[test]
    fn snapshot_cache_reuses_current_revision_and_refreshes_after_mutations() {
        let mut service = Service::new(limits());
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(create("create", 1, "channel", CHANNEL_KIND_OUTPUT))
                .status
        );

        let before_mutation = service.snapshot_bytes().to_vec();
        let before_mutation_revision = service.revision;
        let cached_pointer = service
            .snapshot_cache
            .as_ref()
            .expect("first snapshot is cached")
            .bytes
            .as_ptr();
        assert_eq!(
            before_mutation_revision,
            service
                .snapshot_cache
                .as_ref()
                .expect("first snapshot is cached")
                .revision
        );
        assert_eq!(cached_pointer, service.snapshot_bytes().as_ptr());
        assert_eq!(before_mutation.as_slice(), service.snapshot_bytes());

        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service
                .apply(Request::Text {
                    operation: operation("text"),
                    owner: owner(1),
                    channel_id: b"channel".to_vec(),
                    text: b"new text".to_vec(),
                    replace: true,
                })
                .status
        );
        let after_mutation = service.snapshot_bytes().to_vec();
        assert_ne!(before_mutation, after_mutation);
        assert_eq!(
            service.revision,
            service
                .snapshot_cache
                .as_ref()
                .expect("mutated snapshot is cached")
                .revision
        );
        assert_eq!(after_mutation.as_slice(), service.snapshot_bytes());

        service.revision = u64::MAX;
        let saturated_live = service.snapshot_bytes().to_vec();
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded,
            service.stop().status
        );
        let stopped = service.snapshot_bytes().to_vec();
        assert_ne!(saturated_live, stopped);
        assert_eq!(u64::MAX, service.revision);
        assert_eq!(stopped.as_slice(), service.snapshot_bytes());
        assert_eq!(
            service.revision,
            service
                .snapshot_cache
                .as_ref()
                .expect("stopped snapshot is cached")
                .revision
        );
    }

    #[test]
    fn abi_token_lifecycle_snapshot_and_poisoned_capacity_are_typed() {
        let limits = SakuraOutputShadowLimitsV1 {
            struct_size: size_of::<SakuraOutputShadowLimitsV1>() as u32,
            abi_version: ABI_VERSION_V1,
            maximum_owners: 4,
            maximum_channels: 4,
            maximum_text_bytes_per_channel: 32,
            maximum_payload_bytes: 32,
            maximum_log_entries_per_channel: 4,
            maximum_remembered_operations: 4,
            reserved: [0; 3],
        };
        let owner_id = b"owner";
        let operation_id = b"create";
        let channel_id = b"channel";
        let label = b"Label";
        let request = SakuraOutputShadowRequestV1 {
            struct_size: size_of::<SakuraOutputShadowRequestV1>() as u32,
            abi_version: ABI_VERSION_V1,
            operation_kind: OP_CREATE_CHANNEL,
            channel_kind: u32::from(CHANNEL_KIND_OUTPUT),
            flags: 0,
            operation_id: span(operation_id.as_ptr(), operation_id.len()),
            expected_revision: 0,
            owner_id: span(owner_id.as_ptr(), owner_id.len()),
            owner_generation: 1,
            channel_id: span(channel_id.as_ptr(), channel_id.len()),
            label: span(label.as_ptr(), label.len()),
            metadata_language_id: span(ptr::null(), 0),
            metadata_source: span(ptr::null(), 0),
            payload: span(ptr::null(), 0),
            log_entries: ptr::null(),
            log_entry_count: 0,
            reserved: [0; 4],
        };
        let mut token = 0_u64;
        // SAFETY: All pointers refer to initialized local storage for this
        // test call and no pointer is retained by the export.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_create_v1(&limits, &mut token)
        });
        assert_ne!(0, token);
        let stale_token = token;
        let mut result = poison_result();
        // SAFETY: The request and result satisfy the V1 ABI contract above.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_apply_v1(token, &request, &mut result)
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded as u32,
            result.status
        );

        let mut info = poison_info();
        // SAFETY: `info` is writable local V1 storage.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_snapshot_measure_v1(token, &mut info)
        });
        let mut destination = vec![0_u8; info.encoded_size as usize];
        let mut buffer = SakuraOutputShadowSnapshotBufferV1 {
            struct_size: size_of::<SakuraOutputShadowSnapshotBufferV1>() as u32,
            abi_version: ABI_VERSION_V1,
            data: destination.as_mut_ptr(),
            capacity: destination.len() as u64,
            length: 0,
            reserved: [0; 2],
        };
        // SAFETY: The destination is writable for its declared capacity.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_snapshot_write_v1(token, &mut buffer)
        });
        assert_eq!(info.encoded_size, buffer.length);
        assert!(destination.starts_with(SNAPSHOT_MAGIC));

        let mut too_small = SakuraOutputShadowSnapshotBufferV1 {
            struct_size: size_of::<SakuraOutputShadowSnapshotBufferV1>() as u32,
            abi_version: ABI_VERSION_V1,
            data: ptr::null_mut(),
            capacity: 0,
            length: 0,
            reserved: [0; 2],
        };
        // SAFETY: The descriptor is writable local storage; its zero-capacity
        // destination is intentionally rejected before any dereference.
        assert_eq!(SakuraOutputShadowStatus::InsufficientCapacity, unsafe {
            sakura_output_shadow_snapshot_write_v1(token, &mut too_small)
        });
        assert_eq!(u64::MAX, too_small.length);

        // SAFETY: The result is writable local V1 storage.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_stop_v1(token, &mut result)
        });
        // SAFETY: A stopped token remains valid but cannot apply another op.
        assert_eq!(SakuraOutputShadowStatus::Stopped, unsafe {
            sakura_output_shadow_apply_v1(token, &request, &mut result)
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Stopped as u32,
            result.status
        );
        // SAFETY: The token is writable local storage and is consumed exactly
        // once by the destroy call.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
        let mut stale_info = poison_info();
        // SAFETY: The output is valid local storage; the numeric token was
        // consumed above and must now fail closed as a stale handle.
        assert_eq!(SakuraOutputShadowStatus::InvalidHandle, unsafe {
            sakura_output_shadow_snapshot_measure_v1(stale_token, &mut stale_info)
        });
        assert_eq!(u64::MAX, stale_info.revision);
        let mut stale_destroy_token = stale_token;
        // SAFETY: The token storage is writable but refers to a destroyed
        // shadow, so failure must not consume or zero it.
        assert_eq!(SakuraOutputShadowStatus::InvalidHandle, unsafe {
            sakura_output_shadow_destroy_v1(&mut stale_destroy_token)
        });
        assert_eq!(stale_token, stale_destroy_token);
        // SAFETY: A zero token is intentionally stale/invalid.
        assert_eq!(SakuraOutputShadowStatus::InvalidHandle, unsafe {
            sakura_output_shadow_destroy_v1(&mut token)
        });
    }

    #[test]
    fn reserve_failure_is_typed_and_preserves_model_state() {
        let limits = SakuraOutputShadowLimitsV1 {
            struct_size: size_of::<SakuraOutputShadowLimitsV1>() as u32,
            abi_version: ABI_VERSION_V1,
            maximum_owners: 4,
            maximum_channels: 4,
            maximum_text_bytes_per_channel: 32,
            maximum_payload_bytes: 32,
            maximum_log_entries_per_channel: 4,
            maximum_remembered_operations: 4,
            reserved: [0; 3],
        };
        let mut token = 0_u64;
        // SAFETY: The limits and token point to initialized storage owned by
        // this test, and the export retains neither pointer.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_create_v1(&limits, &mut token)
        });

        let seed_id = b"seed";
        let seed_channel = b"seed-channel";
        let seed_request = abi_create_request(seed_id, seed_channel);
        let mut result = poison_result();
        // SAFETY: The request and result point to initialized test storage for
        // the duration of this call.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_apply_v1(token, &seed_request, &mut result)
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded as u32,
            result.status
        );

        let snapshot_state = |token| {
            let mut registry = lock_registry();
            let service = registry
                .services
                .get_mut(&token)
                .expect("test token remains registered");
            assert_remember_invariant(service);
            let snapshot = service.snapshot_bytes().to_vec();
            let seed_key: Arc<[u8]> = Arc::from(&seed_id[..]);
            let seed_record = service
                .completed_operations
                .get(&seed_key)
                .expect("seed operation is remembered");
            (
                service.revision,
                service.channels.len(),
                service.completed_operations.len(),
                service.completed_operation_order.clone(),
                seed_record.fingerprint.clone(),
                seed_record.result,
                snapshot,
            )
        };

        let before_map_failure = snapshot_state(token);
        let map_failure_id = b"fail-map";
        let map_failure_channel = b"map-channel";
        let map_failure_request = abi_create_request(map_failure_id, map_failure_channel);
        fail_next_reserve(ReserveFailurePoint::Map);
        result = poison_result();
        // SAFETY: The request and result point to initialized test storage for
        // the duration of this call.
        assert_eq!(SakuraOutputShadowStatus::InternalError, unsafe {
            sakura_output_shadow_apply_v1(token, &map_failure_request, &mut result)
        });
        assert_poisoned_result(&result);
        {
            let after = snapshot_state(token);
            assert_eq!(before_map_failure, after);
        }

        // The failed operation was not remembered and can be accepted on a
        // retry after the injected reserve failure has been consumed.
        // SAFETY: The request and result point to initialized test storage for
        // the duration of this call.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_apply_v1(token, &map_failure_request, &mut result)
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded as u32,
            result.status
        );

        let before_queue_failure = snapshot_state(token);
        let queue_failure_id = b"fail-queue";
        let queue_failure_channel = b"queue-channel";
        let queue_failure_request = abi_create_request(queue_failure_id, queue_failure_channel);
        fail_next_reserve(ReserveFailurePoint::Queue);
        result = poison_result();
        // SAFETY: The request and result point to initialized test storage for
        // the duration of this call.
        assert_eq!(SakuraOutputShadowStatus::InternalError, unsafe {
            sakura_output_shadow_apply_v1(token, &queue_failure_request, &mut result)
        });
        assert_poisoned_result(&result);
        {
            let after = snapshot_state(token);
            assert_eq!(before_queue_failure, after);
        }

        // Queue reservation failure has the same retry semantics as map
        // reservation failure.
        // SAFETY: The request and result point to initialized test storage for
        // the duration of this call.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_apply_v1(token, &queue_failure_request, &mut result)
        });
        assert_eq!(
            SakuraOutputShadowOperationStatus::Succeeded as u32,
            result.status
        );

        // SAFETY: The token storage is writable and this test owns its token.
        assert_eq!(SakuraOutputShadowStatus::Ok, unsafe {
            sakura_output_shadow_destroy_v1(&mut token)
        });
        assert_eq!(0, token);
    }

    #[test]
    fn span_shape_validation_defers_address_check_until_selected_copy() {
        let invalid_address = usize::MAX as *const u8;
        let empty = span(invalid_address, 0);
        assert_eq!(0, validate_span_shape(empty).expect("empty shape is valid"));
        assert_eq!(Ok(Vec::new()), copy_shaped_span(empty, 0));

        let payload = b"payload";
        let selected = span(payload.as_ptr(), payload.len());
        let selected_length = validate_span_shape(selected).expect("payload shape is valid");
        assert_eq!(
            Ok(payload.to_vec()),
            copy_shaped_span(selected, selected_length)
        );
        let copied_arc = copy_shaped_arc_span(selected, selected_length)
            .expect("operation IDs copy directly into owned Arc storage");
        assert_eq!(payload, copied_arc.as_ref());
        assert_ne!(payload.as_ptr(), copied_arc.as_ptr());

        let invalid = span(invalid_address, 1);
        let invalid_length = validate_span_shape(invalid).expect("length shape is valid");
        assert_eq!(
            Err(SakuraOutputShadowStatus::InvalidArgument),
            copy_shaped_span(invalid, invalid_length)
        );
    }

    #[test]
    fn text_request_validates_but_does_not_copy_ignored_label() {
        let operation_id = b"operation";
        let owner_id = b"owner";
        let channel_id = b"channel";
        let label = b"ignored label";
        let payload = b"payload";
        let request = SakuraOutputShadowRequestV1 {
            struct_size: size_of::<SakuraOutputShadowRequestV1>() as u32,
            abi_version: ABI_VERSION_V1,
            operation_kind: OP_APPEND_OUTPUT,
            channel_kind: u32::from(CHANNEL_KIND_OUTPUT),
            flags: 0,
            operation_id: span(operation_id.as_ptr(), operation_id.len()),
            expected_revision: 0,
            owner_id: span(owner_id.as_ptr(), owner_id.len()),
            owner_generation: 1,
            channel_id: span(channel_id.as_ptr(), channel_id.len()),
            label: span(label.as_ptr(), label.len()),
            metadata_language_id: span(ptr::null(), 0),
            metadata_source: span(ptr::null(), 0),
            payload: span(payload.as_ptr(), payload.len()),
            log_entries: ptr::null(),
            log_entry_count: 0,
            reserved: [0; 4],
        };

        match read_request(&request).expect("text request with an ignored label is valid") {
            Request::Text { text, replace, .. } => {
                assert_eq!(payload, text.as_slice());
                assert!(!replace);
            }
            _ => panic!("expected text request"),
        }

        let invalid_label = SakuraOutputShadowRequestV1 {
            label: span(usize::MAX as *const u8, 1),
            ..request
        };
        assert!(matches!(
            read_request(&invalid_label),
            Err(SakuraOutputShadowStatus::InvalidArgument)
        ));
    }

    #[test]
    fn invalid_spans_and_panics_are_contained() {
        assert_eq!(
            Err(SakuraOutputShadowStatus::InvalidArgument),
            copy_span(span(std::ptr::null(), 1))
        );
        let invalid_address = (isize::MAX as usize + 1) as *const SakuraOutputShadowRequestV1;
        let mut result = poison_result();
        // SAFETY: The deliberately non-addressable request is rejected by the
        // complete top-level range check before the function reads from it.
        assert_eq!(SakuraOutputShadowStatus::InvalidArgument, unsafe {
            sakura_output_shadow_apply_v1(1, invalid_address, &mut result)
        });
        assert_eq!(u64::MAX, result.revision);
        assert_eq!(
            SakuraOutputShadowStatus::InternalError,
            catch_status(|| panic!("contained"))
        );
    }
}
