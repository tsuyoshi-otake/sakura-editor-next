//! Replay-only OutputService state shadow.
//!
//! This module deliberately has no callback, UI, filesystem, transport, or
//! thread-affinity boundary.  The C++ OutputService remains the authority; a
//! caller submits a copy of an already accepted operation to this model and
//! may copy a deterministic snapshot back out.  The ABI is kept here, beside
//! the model, so the Rust side can validate every pointer and retain no caller
//! memory.

#![allow(dead_code)]

use std::collections::{BTreeMap, VecDeque};
use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::str;
use std::sync::{Mutex, MutexGuard, OnceLock};

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
    id: Vec<u8>,
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
    completed_operations: BTreeMap<Vec<u8>, CompletedOperation>,
    completed_operation_order: VecDeque<Vec<u8>>,
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
            completed_operations: BTreeMap::new(),
            completed_operation_order: VecDeque::new(),
            revision: 1,
            dropped_notification_count: 0,
            stopped: false,
            snapshot_cache: None,
        }
    }

    fn current(
        &self,
        status: SakuraOutputShadowOperationStatus,
        reason: SakuraOutputShadowReason,
    ) -> OperationResult {
        OperationResult {
            status,
            reason,
            revision: self.revision,
        }
    }

    fn stopped_result(&self) -> OperationResult {
        self.current(
            SakuraOutputShadowOperationStatus::Stopped,
            SakuraOutputShadowReason::None,
        )
    }

    fn remember(&mut self, operation_id: Vec<u8>, fingerprint: Vec<u8>, result: OperationResult) {
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

    fn replay_or_conflict(
        &self,
        operation: &Operation,
        fingerprint: &[u8],
    ) -> Option<OperationResult> {
        let found = self.completed_operations.get(&operation.id)?;
        if found.fingerprint != fingerprint {
            return Some(self.current(
                SakuraOutputShadowOperationStatus::Conflict,
                SakuraOutputShadowReason::OperationIdConflict,
            ));
        }
        let mut result = found.result;
        result.status = SakuraOutputShadowOperationStatus::Replayed;
        Some(result)
    }

    fn apply(&mut self, request: Request) -> OperationResult {
        if self.stopped {
            return self.stopped_result();
        }

        let operation = request.operation();
        let operation_id = operation.id.clone();
        if !is_valid_operation_id(&operation.id) {
            return self.current(
                SakuraOutputShadowOperationStatus::Rejected,
                SakuraOutputShadowReason::InvalidOperationId,
            );
        }
        let fingerprint = fingerprint(&request);
        if let Some(result) = self.replay_or_conflict(operation, &fingerprint) {
            return result;
        }
        if operation.expected_revision != Some(self.revision)
            && operation.expected_revision.is_some()
        {
            return self.current(
                SakuraOutputShadowOperationStatus::StaleRevision,
                SakuraOutputShadowReason::ExpectedRevisionMismatch,
            );
        }

        let result = match request {
            Request::CreateChannel {
                operation,
                owner,
                channel_id,
                label,
                kind,
                metadata,
            } => self.create_channel(&operation, owner, channel_id, label, kind, metadata),
            Request::Text {
                operation,
                owner,
                channel_id,
                text,
                replace,
            } => self.apply_text(&operation, owner, channel_id, text, replace),
            Request::AppendLog {
                operation,
                owner,
                channel_id,
                entries,
            } => self.append_log(&operation, owner, channel_id, entries),
            Request::Channel {
                operation,
                owner,
                channel_id,
                kind,
                preserve_focus,
            } => self.apply_channel(&operation, owner, channel_id, kind, preserve_focus),
            Request::DisposeOwner { operation, owner } => self.dispose_owner(&operation, owner),
        };

        if result.status == SakuraOutputShadowOperationStatus::Succeeded {
            self.remember(operation_id, fingerprint, result);
        }
        result
    }

    fn create_channel(
        &mut self,
        _operation: &Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        label: Vec<u8>,
        kind: u8,
        metadata: Metadata,
    ) -> OperationResult {
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
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn apply_text(
        &mut self,
        _operation: &Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        text: Vec<u8>,
        replace: bool,
    ) -> OperationResult {
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
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn append_log(
        &mut self,
        _operation: &Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        entries: Vec<LogEntry>,
    ) -> OperationResult {
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
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn apply_channel(
        &mut self,
        _operation: &Operation,
        owner: Owner,
        channel_id: Vec<u8>,
        kind: u32,
        preserve_focus: bool,
    ) -> OperationResult {
        if kind == OP_CLEAR {
            return self.clear_channel(&owner, &channel_id);
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
            let channel = self
                .channels
                .get_mut(&channel_id)
                .expect("validated channel must remain present");
            channel.visible = true;
            channel.last_show_preserved_focus = preserve_focus;
            self.active_channel_id = Some(channel.id.clone());
            self.revision += 1;
            return self.current(
                SakuraOutputShadowOperationStatus::Succeeded,
                SakuraOutputShadowReason::None,
            );
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
            return self.current(
                SakuraOutputShadowOperationStatus::Succeeded,
                SakuraOutputShadowReason::None,
            );
        }
        if self.revision == u64::MAX {
            return self.current(
                SakuraOutputShadowOperationStatus::RevisionExhausted,
                SakuraOutputShadowReason::None,
            );
        }
        self.channels.remove(&channel_id);
        if self.active_channel_id.as_ref() == Some(&channel_id) {
            self.active_channel_id = None;
            self.select_fallback();
        }
        self.revision += 1;
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn clear_channel(&mut self, owner: &Owner, channel_id: &[u8]) -> OperationResult {
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
        let channel = self
            .channels
            .get_mut(channel_id)
            .expect("validated channel must remain present");
        channel.text.clear();
        channel.log_entries.clear();
        channel.projected_text.clear();
        channel.dropped_character_count = 0;
        self.revision += 1;
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
    }

    fn dispose_owner(&mut self, _operation: &Operation, owner: Owner) -> OperationResult {
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
        self.channels.retain(|_, channel| {
            channel.owner.id != owner.id || channel.owner.generation != owner.generation
        });
        if let Some(active) = self.active_owner_generations.get_mut(&owner.id) {
            active.disposed = true;
        }
        self.active_channel_id = None;
        self.select_fallback();
        self.revision += 1;
        self.current(
            SakuraOutputShadowOperationStatus::Succeeded,
            SakuraOutputShadowReason::None,
        )
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
    ) -> OperationResult {
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

    fn validation_result_any(&self, owner: &Owner, channel_id: &[u8]) -> OperationResult {
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

fn copy_operation_and_owner(
    raw: SakuraOutputShadowRequestV1,
    operation_id_length: usize,
    owner_id_length: usize,
    expected_revision: Option<u64>,
) -> Result<(Operation, Owner), SakuraOutputShadowStatus> {
    Ok((
        Operation {
            id: copy_shaped_span(raw.operation_id, operation_id_length)?,
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
    let operation_result = service.apply(request);
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

fn fingerprint(request: &Request) -> Vec<u8> {
    let mut output = Vec::new();
    match request {
        Request::CreateChannel {
            operation,
            owner,
            channel_id,
            label,
            kind,
            metadata,
        } => {
            output.extend_from_slice(b"create;");
            append_fingerprint_operation(&mut output, operation);
            append_fingerprint_owner(&mut output, owner);
            append_fingerprint_bytes(&mut output, channel_id);
            append_fingerprint_bytes(&mut output, label);
            output.push(*kind);
            put_optional_bytes(&mut output, metadata.language_id.as_deref());
            put_optional_bytes(&mut output, metadata.source.as_deref());
        }
        Request::Text {
            operation,
            owner,
            channel_id,
            text,
            replace,
        } => {
            output.extend_from_slice(if *replace {
                b"replace-output;"
            } else {
                b"append-output;"
            });
            append_fingerprint_operation(&mut output, operation);
            append_fingerprint_owner(&mut output, owner);
            append_fingerprint_bytes(&mut output, channel_id);
            append_fingerprint_bytes(&mut output, text);
        }
        Request::AppendLog {
            operation,
            owner,
            channel_id,
            entries,
        } => {
            output.extend_from_slice(b"append-log;");
            append_fingerprint_operation(&mut output, operation);
            append_fingerprint_owner(&mut output, owner);
            append_fingerprint_bytes(&mut output, channel_id);
            for entry in entries {
                put_u32(&mut output, entry.level);
                append_fingerprint_bytes(&mut output, &entry.message);
                put_optional_bytes(&mut output, entry.source.as_deref());
            }
        }
        Request::Channel {
            operation,
            owner,
            channel_id,
            kind,
            preserve_focus,
        } => {
            let tag = match *kind {
                OP_CLEAR => b"clear;".as_slice(),
                OP_SHOW => b"show;".as_slice(),
                OP_HIDE => b"hide;".as_slice(),
                OP_DISPOSE => b"dispose;".as_slice(),
                _ => b"channel;".as_slice(),
            };
            output.extend_from_slice(tag);
            append_fingerprint_operation(&mut output, operation);
            append_fingerprint_owner(&mut output, owner);
            append_fingerprint_bytes(&mut output, channel_id);
            if *kind == OP_SHOW {
                output.push(u8::from(*preserve_focus));
            }
        }
        Request::DisposeOwner { operation, owner } => {
            output.extend_from_slice(b"dispose-owner;");
            append_fingerprint_operation(&mut output, operation);
            append_fingerprint_owner(&mut output, owner);
        }
    }
    output
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
            id: id.as_bytes().to_vec(),
            expected_revision: None,
        }
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
