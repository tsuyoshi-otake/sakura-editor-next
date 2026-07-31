/*! @file
 * @brief Presentation-neutral Hot Exit lifecycle contracts.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreTypes.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace workbench::editor::persistence {

enum class EEditorWorkingCopyLifecycleStatus : std::uint8_t {
	Succeeded, Deferred, NotApplicable, Suppressed, Cancelled, Failed, Conflict, Unsupported, Stopped,
};

//! Value-free terminal reason; implementations must never put document text or a path in this value.
enum class EEditorWorkingCopyLifecycleReason : std::uint8_t {
	None, InvalidScope, ExplicitCommandLine, MultipleFiles, DebugOrGrep, LayoutOrGroupNotReady,
	SessionNotFound, BackupNotFound, InvalidStoredRecord, InvalidSnapshot, StaleSnapshot,
	TooManyInputs, UnsupportedInput, BackupMismatch, ChecksumOrEncodingInvalid, ApplierCancelled,
	ApplierFailed, CoreAdoptionFailed, CoreRollbackFailed, CoreActivationFailed, NativeProjectionFailed,
	StoreConflict, StoreFailed, OperationInFlight, ShutdownInProgress, Stopped,
};

enum class EEditorWorkingCopyShutdownState : std::uint8_t { Running, BeforeShutdown, WillShutdown, Stopped };

struct EditorWorkingCopyLifecycleResult final {
	EEditorWorkingCopyLifecycleStatus status = EEditorWorkingCopyLifecycleStatus::Failed;
	EEditorWorkingCopyLifecycleReason reason = EEditorWorkingCopyLifecycleReason::None;
	bool mutatedDurableState = false;
	//! Populated only after a native recovery Commit. They let the composition root
	//! project the exact recovered input without rereading or inferring session state.
	std::optional<std::string> restoredInputId;
	std::optional<std::string> effectiveActiveInputId;
};

//! Snapshot source output; it deliberately contains no HWND, title, file-system path, or view state.
struct EditorWorkingCopyPersistenceSnapshot final {
	WorkingCopyPersistenceIdentity identity;
	std::string inputId;
	std::string inputTypeId;
	std::uint64_t contentVersion = 0;
	bool dirty = false;
	EWorkingCopyTextEncoding encoding = EWorkingCopyTextEncoding::Unknown;
	EWorkingCopyEol eol = EWorkingCopyEol::Unknown;
	std::string content;

	[[nodiscard]] bool IsValid() const noexcept;
};

//! Lightweight completion boundary used after save/close. It deliberately omits document text.
struct EditorWorkingCopyCleanEvent final {
	WorkingCopyPersistenceIdentity identity;
	std::uint64_t contentVersion = 0;

	[[nodiscard]] bool IsValid() const noexcept;
};

enum class EEditorWorkingCopySnapshotStatus : std::uint8_t { Captured, NoInput, Unsupported, Failed };
struct EditorWorkingCopySnapshotResult final {
	EEditorWorkingCopySnapshotStatus status = EEditorWorkingCopySnapshotStatus::Failed;
	std::optional<EditorWorkingCopyPersistenceSnapshot> snapshot;
};

class IEditorWorkingCopySnapshotSource {
public:
	virtual ~IEditorWorkingCopySnapshotSource() = default;
	[[nodiscard]] virtual EditorWorkingCopySnapshotResult Capture() = 0;
};

struct EditorWorkingCopyRecoveryRequest final {
	EditorSessionInputDescriptor input;
	WorkingCopyBackup backup;
};

enum class EEditorWorkingCopyRecoveryStatus : std::uint8_t { Prepared, Cancelled, Failed, Unsupported };
enum class EEditorWorkingCopyRecoveryCommitStatus : std::uint8_t { Committed, TargetChanged, NotPrepared };
struct EditorWorkingCopyRecoveryPrepareResult final {
	EEditorWorkingCopyRecoveryStatus status = EEditorWorkingCopyRecoveryStatus::Failed;
	//! Canonical core identity produced after validating the persisted identity and decoded text metadata.
	std::optional<EditorDocumentIdentity> coreIdentity;
};

/*! @brief Non-destructive recovery boundary.
 * Prepare validates and stages text/encoding/EOL. Commit makes the staged native document available.
 * Both stages must be independent of tab, window, and focus presentation.
 */
class IEditorWorkingCopyRecoveryApplier {
public:
	virtual ~IEditorWorkingCopyRecoveryApplier() = default;
	[[nodiscard]] virtual EditorWorkingCopyRecoveryPrepareResult Prepare(const EditorWorkingCopyRecoveryRequest& request) = 0;
	//! Discards the currently staged native recovery document without mutating its target.
	//! This is idempotent and is required after every successful Prepare that does not reach Commit.
	virtual void AbortPrepared() noexcept {}
	[[nodiscard]] virtual EEditorWorkingCopyRecoveryCommitStatus Commit(
		const EditorWorkingCopyRecoveryRequest& request) noexcept = 0;
};

/*! @brief Core-facing inactive adoption seam.
 * The adapter must adopt the recovered input without selecting it. It keeps the lifecycle independent
 * of the one-group implementation. Adoption precedes the native no-throw commit so adoption failure
 * cannot leave the native document mutated without its corresponding core input.
 */
class IEditorWorkingCopyRecoveredInputAdopter {
public:
	virtual ~IEditorWorkingCopyRecoveredInputAdopter() = default;
	//! On success the recovered input exists in the core but remains inactive. The
	//! native applier's no-throw Commit immediately finalizes the prepared document.
	[[nodiscard]] virtual bool AdoptInactive(const EditorSessionInputDescriptor& input,
		const EditorDocumentIdentity& identity, std::uint64_t contentVersion) = 0;
	//! Removes only the exact still-inactive recovery adopted by AdoptInactive.
	//! A failed native commit invokes this compensating terminal operation.
	[[nodiscard]] virtual bool RollbackInactive(const EditorSessionInputDescriptor& input,
		const EditorDocumentIdentity& identity, std::uint64_t contentVersion) noexcept = 0;
};

struct EditorWorkingCopyRestoreRequest final {
	WorkingCopyPersistenceScope scope;
	bool layoutAndGroupReady = false;
	bool explicitCommandLine = false;
	bool multipleFiles = false;
	bool debugOrGrep = false;
};

struct EditorWorkingCopyLifecycleOptions final {
	std::uint64_t debounceTicks = 20;
	std::uint64_t maximumAgeTicks = 200;
};

} // namespace workbench::editor::persistence
