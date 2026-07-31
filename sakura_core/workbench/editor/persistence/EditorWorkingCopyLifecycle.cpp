/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/persistence/EditorWorkingCopyLifecycle.h"

#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace workbench::editor::persistence {
namespace {

EditorWorkingCopyLifecycleResult Result(EEditorWorkingCopyLifecycleStatus status,
	EEditorWorkingCopyLifecycleReason reason = EEditorWorkingCopyLifecycleReason::None, bool mutated = false)
{
	return { .status = status, .reason = reason, .mutatedDurableState = mutated };
}

bool IsSaved(const WorkingCopyPersistenceWriteResult& value) noexcept
{
	return value.status == EWorkingCopyPersistenceWriteStatus::Persisted;
}

bool IsDeleted(const WorkingCopyPersistenceWriteResult& value) noexcept
{
	return value.status == EWorkingCopyPersistenceWriteStatus::Deleted;
}

EditorWorkingCopyLifecycleResult StoreFailure(
	const WorkingCopyPersistenceWriteResult& value, bool mutatedDurableState = false)
{
	return Result(value.status == EWorkingCopyPersistenceWriteStatus::Conflict
		? EEditorWorkingCopyLifecycleStatus::Conflict : EEditorWorkingCopyLifecycleStatus::Failed,
		value.status == EWorkingCopyPersistenceWriteStatus::Conflict
		? EEditorWorkingCopyLifecycleReason::StoreConflict : EEditorWorkingCopyLifecycleReason::StoreFailed,
		mutatedDurableState);
}

std::optional<std::uint64_t> NextGeneration(std::optional<std::uint64_t> current) noexcept
{
	if (current && *current >= kMaximumWorkingCopyPersistenceGeneration) return std::nullopt;
	return current.value_or(0) + 1;
}

class PreparedRecoveryAbortGuard final {
public:
	explicit PreparedRecoveryAbortGuard(IEditorWorkingCopyRecoveryApplier& applier) noexcept
		: m_applier(applier)
	{
	}

	PreparedRecoveryAbortGuard(const PreparedRecoveryAbortGuard&) = delete;
	PreparedRecoveryAbortGuard& operator=(const PreparedRecoveryAbortGuard&) = delete;

	~PreparedRecoveryAbortGuard()
	{
		if (m_prepared && !m_commitCalled) m_applier.AbortPrepared();
	}

	void MarkPrepared() noexcept { m_prepared = true; }
	void MarkCommitCalled() noexcept { m_commitCalled = true; }

private:
	IEditorWorkingCopyRecoveryApplier& m_applier;
	bool m_prepared = false;
	bool m_commitCalled = false;
};

} // namespace

bool EditorWorkingCopyPersistenceSnapshot::IsValid() const noexcept
{
	const bool validEncoding = encoding == EWorkingCopyTextEncoding::Utf8
		|| encoding == EWorkingCopyTextEncoding::Utf8WithBom
		|| encoding == EWorkingCopyTextEncoding::Utf16Le
		|| encoding == EWorkingCopyTextEncoding::Utf16Be
		|| encoding == EWorkingCopyTextEncoding::Windows1252
		|| encoding == EWorkingCopyTextEncoding::Unknown;
	const bool validEol = eol == EWorkingCopyEol::Lf || eol == EWorkingCopyEol::CrLf
		|| eol == EWorkingCopyEol::Cr || eol == EWorkingCopyEol::Unknown;
	return identity.IsValid() && IsValidWorkingCopyPersistenceId(inputId)
		&& inputTypeId == CWorkingCopyPersistenceCodec::kTextInputTypeId
		&& contentVersion != 0 && contentVersion <= kMaximumWorkingCopyPersistenceGeneration
		&& validEncoding && validEol
		&& IsValidWorkingCopyPersistenceUtf8(content, true, kMaximumWorkingCopyPersistenceContentBytes);
}

bool EditorWorkingCopyCleanEvent::IsValid() const noexcept
{
	return identity.IsValid() && contentVersion != 0
		&& contentVersion <= kMaximumWorkingCopyPersistenceGeneration;
}

EditorWorkingCopyLifecycle::EditorWorkingCopyLifecycle(IWorkingCopyBackupStore& backups, IEditorSessionStore& sessions,
	IEditorWorkingCopySnapshotSource& snapshots, IEditorWorkingCopyRecoveryApplier& applier,
	IEditorWorkingCopyRecoveredInputAdopter& adopter, EditorWorkingCopyLifecycleOptions options)
	: m_backups(backups), m_sessions(sessions), m_snapshots(snapshots), m_applier(applier), m_adopter(adopter), m_options(options)
{
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::Restore(const EditorWorkingCopyRestoreRequest& request)
{
	if (!request.scope.IsValid()) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidScope);
	if (request.explicitCommandLine) return Result(EEditorWorkingCopyLifecycleStatus::Suppressed, EEditorWorkingCopyLifecycleReason::ExplicitCommandLine);
	if (request.multipleFiles) return Result(EEditorWorkingCopyLifecycleStatus::Suppressed, EEditorWorkingCopyLifecycleReason::MultipleFiles);
	if (request.debugOrGrep) return Result(EEditorWorkingCopyLifecycleStatus::Suppressed, EEditorWorkingCopyLifecycleReason::DebugOrGrep);
	if (!request.layoutAndGroupReady) return Result(EEditorWorkingCopyLifecycleStatus::Deferred, EEditorWorkingCopyLifecycleReason::LayoutOrGroupNotReady);
	{
		std::scoped_lock lock(m_mutex);
		if (m_shutdown == EEditorWorkingCopyShutdownState::Stopped) return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
		if (m_shutdown != EEditorWorkingCopyShutdownState::Running) {
			return Result(EEditorWorkingCopyLifecycleStatus::Suppressed, EEditorWorkingCopyLifecycleReason::ShutdownInProgress);
		}
		if (m_completionFence == kMaximumWorkingCopyPersistenceGeneration) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
		if (m_inFlight) return Result(EEditorWorkingCopyLifecycleStatus::Deferred, EEditorWorkingCopyLifecycleReason::OperationInFlight);
		m_inFlight = true;
	}

	const auto finish = [this](EditorWorkingCopyLifecycleResult result) {
		std::scoped_lock lock(m_mutex);
		const bool stopped = m_shutdown == EEditorWorkingCopyShutdownState::Stopped;
		m_inFlight = false;
		return stopped ? Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped) : result;
	};
	const auto stopped = [this]() {
		std::scoped_lock lock(m_mutex);
		return m_shutdown == EEditorWorkingCopyShutdownState::Stopped;
	};
	PreparedRecoveryAbortGuard preparedGuard(m_applier);
	try {
		const auto sessionResult = m_sessions.Load(request.scope);
		if (stopped()) return finish(Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped));
		if (sessionResult.status == EWorkingCopyPersistenceLoadStatus::NotFound) return finish(Result(EEditorWorkingCopyLifecycleStatus::NotApplicable, EEditorWorkingCopyLifecycleReason::SessionNotFound));
		if (!sessionResult.Loaded() || !sessionResult.manifest || !sessionResult.manifest->IsValid()
			|| sessionResult.manifest->scope != request.scope) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord));
		}
		const auto& session = *sessionResult.manifest;
		if (session.inputs.empty()) return finish(Result(EEditorWorkingCopyLifecycleStatus::NotApplicable));
		if (session.inputs.size() != 1) return finish(Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::TooManyInputs));
		const auto& input = session.inputs.front();
		// V1 records written by this lifecycle always name their active input. A
		// null value is retained only as an explicit migration path for an older,
		// singleton manifest; never infer an active input for a multi-input record.
		const bool legacySingleInput = !session.activeInputId;
		if ((!legacySingleInput && *session.activeInputId != input.inputId)) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord));
		}
		if (input.inputTypeId != CWorkingCopyPersistenceCodec::kTextInputTypeId || !input.backupGeneration) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::UnsupportedInput));
		}
		const auto backupResult = m_backups.Load(request.scope, input.workingCopyIdentity);
		if (stopped()) return finish(Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped));
		if (backupResult.status == EWorkingCopyPersistenceLoadStatus::NotFound) return finish(Result(EEditorWorkingCopyLifecycleStatus::NotApplicable, EEditorWorkingCopyLifecycleReason::BackupNotFound));
		if (!backupResult.Loaded() || !backupResult.backup || !backupResult.backup->IsValid()
			|| !IsExactBackup(*backupResult.backup, session, input)) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord));
		}
		const EditorWorkingCopyRecoveryRequest recovery{ .input = input, .backup = *backupResult.backup };
		const auto prepared = m_applier.Prepare(recovery);
		if (prepared.status == EEditorWorkingCopyRecoveryStatus::Prepared) preparedGuard.MarkPrepared();
		if (stopped()) return finish(Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped));
		if (prepared.status == EEditorWorkingCopyRecoveryStatus::Cancelled) return finish(Result(EEditorWorkingCopyLifecycleStatus::Cancelled, EEditorWorkingCopyLifecycleReason::ApplierCancelled));
		if (prepared.status == EEditorWorkingCopyRecoveryStatus::Unsupported) return finish(Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::UnsupportedInput));
		if (prepared.status != EEditorWorkingCopyRecoveryStatus::Prepared || !prepared.coreIdentity || !prepared.coreIdentity->IsValid()) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::ChecksumOrEncodingInvalid));
		}

		// Allocate every potentially-throwing durable-state value before the native
		// transfer.  The post-Commit publication below uses only no-throw swaps and
		// scalar assignments, so a committed native document always has a terminal
		// lifecycle state.
		std::optional<WorkingCopyPersistenceIdentity> restoredBackupIdentity{ backupResult.backup->identity };
		std::optional<WorkingCopyPersistenceScope> restoredScope{ request.scope };
		std::optional<WorkingCopyPersistenceIdentity> restoredPersistedIdentity{ backupResult.backup->identity };
		std::optional<std::string> restoredInputId{ input.inputId };
		std::optional<std::string> effectiveActiveInputId{
			legacySingleInput ? input.inputId : *session.activeInputId };

		bool adopted = false;
		try {
			adopted = m_adopter.AdoptInactive(input, *prepared.coreIdentity, backupResult.backup->contentVersion);
		} catch (...) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed,
				EEditorWorkingCopyLifecycleReason::CoreAdoptionFailed));
		}
		if (!adopted) {
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::CoreAdoptionFailed));
		}
		if (stopped()) {
			static_cast<void>(m_adopter.RollbackInactive(input, *prepared.coreIdentity,
				backupResult.backup->contentVersion));
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped));
		}
		// Core adoption is deliberately first: it is inactive and therefore not projected.
		// Commit remains no-throw but still detects a target changed by synchronous
		// reentrancy. Compensate that exceptional path by removing only this exact
		// inactive recovery before exposing a terminal failure.
		preparedGuard.MarkCommitCalled();
		if (m_applier.Commit(recovery) != EEditorWorkingCopyRecoveryCommitStatus::Committed) {
			bool rolledBack = false;
			try {
				rolledBack = m_adopter.RollbackInactive(input, *prepared.coreIdentity,
					backupResult.backup->contentVersion);
			} catch (...) {
				rolledBack = false;
			}
			return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed,
				rolledBack ? EEditorWorkingCopyLifecycleReason::ApplierFailed
					: EEditorWorkingCopyLifecycleReason::CoreRollbackFailed));
		}
		{
			std::scoped_lock lock(m_mutex);
			m_backupGeneration = backupResult.backup->generation;
			m_backupIdentity.swap(restoredBackupIdentity);
			m_sessionGeneration = session.generation;
			m_stateScope.swap(restoredScope);
			m_lastPersistedIdentity.swap(restoredPersistedIdentity);
			m_lastPersistedContentVersion = backupResult.backup->contentVersion;
			++m_completionFence;
		}
		auto success = Result(EEditorWorkingCopyLifecycleStatus::Succeeded);
		success.restoredInputId.swap(restoredInputId);
		success.effectiveActiveInputId.swap(effectiveActiveInputId);
		return finish(std::move(success));
	} catch (...) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed));
	}
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::NotifyChanged(
	const WorkingCopyPersistenceScope& scope, const WorkingCopyPersistenceIdentity& identity,
	std::uint64_t contentVersion, std::uint64_t nowTicks)
{
	if (!scope.IsValid()) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidScope);
	if (!identity.IsValid()) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	if (contentVersion == 0) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	std::scoped_lock lock(m_mutex);
	if (m_shutdown == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
	}
	if (m_shutdown != EEditorWorkingCopyShutdownState::Running) {
		return Result(EEditorWorkingCopyLifecycleStatus::Deferred, EEditorWorkingCopyLifecycleReason::ShutdownInProgress);
	}
	if ((m_pendingScope && *m_pendingScope != scope) || (m_pendingIdentity && *m_pendingIdentity != identity)) {
		return Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot);
	}
	const bool samePersistedWorkingCopy = m_stateScope && *m_stateScope == scope
		&& m_lastPersistedIdentity && *m_lastPersistedIdentity == identity;
	if (samePersistedWorkingCopy && m_lastPersistedContentVersion
		&& contentVersion < *m_lastPersistedContentVersion) {
		return Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot);
	}
	if (!m_pendingContentVersion && samePersistedWorkingCopy && m_lastPersistedContentVersion
		&& contentVersion == *m_lastPersistedContentVersion) {
		return Result(EEditorWorkingCopyLifecycleStatus::NotApplicable);
	}
	if (m_pendingContentVersion && contentVersion < *m_pendingContentVersion) {
		return Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot);
	}
	if (!m_pendingContentVersion) {
		m_pendingScope = scope;
		m_pendingIdentity = identity;
		m_pendingContentVersion = contentVersion;
		m_firstPendingTick = nowTicks;
		m_lastPendingTick = nowTicks;
	} else if (contentVersion > *m_pendingContentVersion) {
		m_pendingContentVersion = contentVersion;
		m_lastPendingTick = nowTicks;
	}
	// Reentrant notifications during a store call are intentionally coalesced;
	// they must not disappear merely because one older generation is in flight.
	return Result(EEditorWorkingCopyLifecycleStatus::Deferred,
		m_inFlight ? EEditorWorkingCopyLifecycleReason::OperationInFlight : EEditorWorkingCopyLifecycleReason::None);
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::Flush(
	const WorkingCopyPersistenceScope& scope, std::uint64_t nowTicks, bool force)
{
	if (!scope.IsValid()) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidScope);
	std::uint64_t scheduledVersion = 0;
	WorkingCopyPersistenceIdentity scheduledIdentity;
	{
		std::scoped_lock lock(m_mutex);
		if (m_shutdown == EEditorWorkingCopyShutdownState::Stopped) {
			return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
		}
		if (!m_pendingScope || !m_pendingIdentity || !m_pendingContentVersion) return Result(EEditorWorkingCopyLifecycleStatus::NotApplicable);
		if (*m_pendingScope != scope) return Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::InvalidScope);
		const auto elapsed = [nowTicks](std::uint64_t then) {
			return nowTicks >= then ? nowTicks - then : std::uint64_t{ 0 };
		};
		const bool due = force || elapsed(m_lastPendingTick) >= m_options.debounceTicks
			|| elapsed(m_firstPendingTick) >= m_options.maximumAgeTicks;
		if (!due) return Result(EEditorWorkingCopyLifecycleStatus::Deferred);
		if (m_inFlight) return Result(EEditorWorkingCopyLifecycleStatus::Deferred, EEditorWorkingCopyLifecycleReason::OperationInFlight);
		m_inFlight = true;
		scheduledVersion = *m_pendingContentVersion;
		scheduledIdentity = *m_pendingIdentity;
	}

	const auto finish = [this, &scope, scheduledIdentity, scheduledVersion](EditorWorkingCopyLifecycleResult result,
		std::optional<std::uint64_t> completedVersion = std::nullopt, bool clearPending = false) {
		std::scoped_lock lock(m_mutex);
		const bool stopped = m_shutdown == EEditorWorkingCopyShutdownState::Stopped;
		m_inFlight = false;
		if (!stopped && completedVersion && result.status == EEditorWorkingCopyLifecycleStatus::Succeeded) {
			m_stateScope = scope;
			m_lastPersistedIdentity = scheduledIdentity;
			m_lastPersistedContentVersion = *completedVersion;
		}
		if (!stopped && clearPending && m_pendingScope && *m_pendingScope == scope
			&& m_pendingContentVersion && *m_pendingContentVersion <= completedVersion.value_or(scheduledVersion)) {
			m_pendingScope.reset();
			m_pendingIdentity.reset();
			m_pendingContentVersion.reset();
		}
		return stopped
			? Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped,
				result.mutatedDurableState)
			: result;
	};

	EditorWorkingCopySnapshotResult captured;
	try {
		captured = m_snapshots.Capture();
	} catch (...) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidSnapshot));
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped));
	}
	if (captured.status == EEditorWorkingCopySnapshotStatus::NoInput) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot));
	}
	if (captured.status == EEditorWorkingCopySnapshotStatus::Unsupported) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::UnsupportedInput));
	}
	if (captured.status != EEditorWorkingCopySnapshotStatus::Captured || !captured.snapshot
		|| !captured.snapshot->IsValid()) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidSnapshot));
	}
	if (captured.snapshot->identity != scheduledIdentity) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot));
	}
	if (!captured.snapshot->dirty) {
		EditorWorkingCopyLifecycleResult cleanResult;
		try {
			cleanResult = PersistClean(scope,
				{ captured.snapshot->identity, captured.snapshot->contentVersion }, false);
		} catch (...) {
			cleanResult = Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
		return finish(cleanResult, captured.snapshot->contentVersion,
			cleanResult.status == EEditorWorkingCopyLifecycleStatus::Succeeded);
	}
	if (captured.snapshot->contentVersion < scheduledVersion) {
		return finish(Result(EEditorWorkingCopyLifecycleStatus::Conflict, EEditorWorkingCopyLifecycleReason::StaleSnapshot));
	}
	EditorWorkingCopyLifecycleResult result;
	try {
		result = PersistDirty(scope, *captured.snapshot);
	} catch (...) {
		result = Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
	return finish(result, captured.snapshot->contentVersion,
		result.status == EEditorWorkingCopyLifecycleStatus::Succeeded);
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::PersistDirty(const WorkingCopyPersistenceScope& scope,
	const EditorWorkingCopyPersistenceSnapshot& snapshot)
{
	std::optional<std::uint64_t> expectedBackup;
	std::optional<std::uint64_t> expectedSession;
	{
		std::scoped_lock lock(m_mutex);
		if (m_completionFence == kMaximumWorkingCopyPersistenceGeneration) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
		if (m_stateScope && *m_stateScope == scope) {
			if (m_backupIdentity && *m_backupIdentity == snapshot.identity) expectedBackup = m_backupGeneration;
			expectedSession = m_sessionGeneration;
		}
	}
	if (!expectedBackup) {
		const auto loaded = m_backups.Load(scope, snapshot.identity);
		if (loaded.status == EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
		if (loaded.status == EWorkingCopyPersistenceLoadStatus::Loaded) {
			if (!loaded.Loaded() || !loaded.backup->IsValid()
				|| loaded.backup->scope != scope || loaded.backup->identity != snapshot.identity) {
				return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
			}
			expectedBackup = loaded.backup->generation;
		} else if (loaded.status != EWorkingCopyPersistenceLoadStatus::NotFound) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
	}
	if (!expectedSession) {
		const auto loaded = m_sessions.Load(scope);
		if (loaded.status == EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
		if (loaded.status == EWorkingCopyPersistenceLoadStatus::Loaded) {
			if (!loaded.Loaded() || !loaded.manifest->IsValid() || loaded.manifest->scope != scope) {
				return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
			}
			if (loaded.manifest->inputs.size() > 1) {
				return Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::TooManyInputs);
			}
			expectedSession = loaded.manifest->generation;
		} else if (loaded.status != EWorkingCopyPersistenceLoadStatus::NotFound) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
	}
	const auto backupGeneration = NextGeneration(expectedBackup);
	const auto sessionGeneration = NextGeneration(expectedSession);
	const auto backupOperation = NextOperationId("backup");
	const auto sessionOperation = NextOperationId("session");
	if (!backupGeneration || !sessionGeneration || !backupOperation || !sessionOperation) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
	WorkingCopyBackup backup{ .scope = scope, .identity = snapshot.identity,
		.generation = *backupGeneration, .contentVersion = snapshot.contentVersion,
		.encoding = snapshot.encoding, .eol = snapshot.eol, .dirty = true, .content = snapshot.content };
	backup.checksum = CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content);
	const auto savedBackup = m_backups.Save(backup, expectedBackup, *backupOperation);
	if (!IsSaved(savedBackup)) return StoreFailure(savedBackup);
	if (savedBackup.generation != backup.generation) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed, true);
	}
	{
		std::scoped_lock lock(m_mutex);
		m_stateScope = scope;
		m_backupGeneration = savedBackup.generation;
		m_backupIdentity = snapshot.identity;
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped, true);
	}
	backup.generation = savedBackup.generation;
	EditorSessionManifest session{ .scope = scope, .generation = *sessionGeneration,
		.logicalGroupId = "workbench.editorGroup.primary",
		.activeInputId = snapshot.inputId,
		.inputs = { { snapshot.inputId, snapshot.inputTypeId, snapshot.identity, 1, {}, backup.generation } } };
	const auto savedSession = m_sessions.Save(session, expectedSession, *sessionOperation);
	if (!IsSaved(savedSession)) return StoreFailure(savedSession, true);
	if (savedSession.generation != session.generation) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed, true);
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped, true);
	}
	std::scoped_lock lock(m_mutex);
	m_sessionGeneration = savedSession.generation;
	++m_completionFence;
	return Result(EEditorWorkingCopyLifecycleStatus::Succeeded, EEditorWorkingCopyLifecycleReason::None, true);
}

std::optional<std::uint64_t> EditorWorkingCopyLifecycle::CaptureCompletionFence(
	const WorkingCopyPersistenceScope& scope) const noexcept
{
	if (!scope.IsValid()) return std::nullopt;
	std::scoped_lock lock(m_mutex);
	if (m_shutdown == EEditorWorkingCopyShutdownState::Stopped
		|| (m_stateScope && *m_stateScope != scope)) {
		return std::nullopt;
	}
	return m_completionFence;
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::OnSavedOrClosed(const WorkingCopyPersistenceScope& scope,
	const EditorWorkingCopyCleanEvent& cleanEvent, std::uint64_t completionFence,
	bool allowIdentityReplacement)
{
	if (!scope.IsValid() || !cleanEvent.IsValid() || completionFence == 0
		|| completionFence > kMaximumWorkingCopyPersistenceGeneration) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidSnapshot);
	}
	{
		std::scoped_lock lock(m_mutex);
		if (m_shutdown == EEditorWorkingCopyShutdownState::Stopped) return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
		const bool exactReplay = m_lastCompletedFence && *m_lastCompletedFence == completionFence
			&& m_lastCompletedScope && *m_lastCompletedScope == scope
			&& m_lastCompletedCleanEvent && m_lastCompletedCleanEvent->identity == cleanEvent.identity
			&& m_lastCompletedCleanEvent->contentVersion == cleanEvent.contentVersion;
		if (exactReplay) return Result(EEditorWorkingCopyLifecycleStatus::Succeeded);
		if (completionFence != m_completionFence) {
			return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
				EEditorWorkingCopyLifecycleReason::StaleSnapshot);
		}
		if (m_completionFence == kMaximumWorkingCopyPersistenceGeneration) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
		if (m_inFlight) return Result(EEditorWorkingCopyLifecycleStatus::Deferred, EEditorWorkingCopyLifecycleReason::OperationInFlight);
		m_inFlight = true;
	}
	EditorWorkingCopyLifecycleResult result;
	try {
		result = PersistClean(scope, cleanEvent, allowIdentityReplacement);
	} catch (...) {
		result = Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
	std::scoped_lock lock(m_mutex);
	const bool stopped = m_shutdown == EEditorWorkingCopyShutdownState::Stopped;
	m_inFlight = false;
	if (result.status == EEditorWorkingCopyLifecycleStatus::Succeeded) {
		m_lastCompletedFence = completionFence;
		m_lastCompletedScope = scope;
		m_lastCompletedCleanEvent = cleanEvent;
		++m_completionFence;
		if (m_pendingScope && *m_pendingScope == scope && m_pendingContentVersion
			&& *m_pendingContentVersion <= cleanEvent.contentVersion) {
			m_pendingScope.reset();
			m_pendingIdentity.reset();
			m_pendingContentVersion.reset();
		}
	}
	return stopped
		? Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped,
			result.mutatedDurableState)
		: result;
}

EditorWorkingCopyLifecycleResult EditorWorkingCopyLifecycle::PersistClean(const WorkingCopyPersistenceScope& scope,
	const EditorWorkingCopyCleanEvent& event, bool allowIdentityReplacement)
{
	struct BackupDeleteTarget final {
		WorkingCopyPersistenceIdentity identity;
		std::uint64_t generation = 0;
		std::uint64_t contentVersion = 0;
	};
	struct PublishedState final {
		BackupDeleteTarget backup;
		std::uint64_t sessionGeneration = 0;
	};
	std::optional<PublishedState> published;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stateScope && *m_stateScope == scope) {
			if (m_backupGeneration.has_value() != m_backupIdentity.has_value()) {
				return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
			}
			if (m_backupGeneration && m_backupIdentity && m_sessionGeneration
				&& m_lastPersistedIdentity && m_lastPersistedContentVersion
				&& *m_backupIdentity == *m_lastPersistedIdentity) {
				published = PublishedState{
					.backup = { *m_backupIdentity, *m_backupGeneration, *m_lastPersistedContentVersion },
					.sessionGeneration = *m_sessionGeneration,
				};
			}
		}
	}
	if (published && (event.contentVersion != published->backup.contentVersion
		|| (!allowIdentityReplacement && event.identity != published->backup.identity))) {
		return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
			EEditorWorkingCopyLifecycleReason::StaleSnapshot);
	}
	const auto loadedSession = m_sessions.Load(scope);
	if (loadedSession.status == EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord) {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
	}
	std::optional<std::uint64_t> sessionGeneration;
	bool sessionNeedsClearing = false;
	std::optional<BackupDeleteTarget> deleteTarget;
	if (loadedSession.status == EWorkingCopyPersistenceLoadStatus::Loaded) {
		if (!loadedSession.Loaded() || !loadedSession.manifest->IsValid() || loadedSession.manifest->scope != scope) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
		}
		if (loadedSession.manifest->inputs.size() > 1) {
			return Result(EEditorWorkingCopyLifecycleStatus::Unsupported, EEditorWorkingCopyLifecycleReason::TooManyInputs);
		}
		sessionGeneration = loadedSession.manifest->generation;
		sessionNeedsClearing = !loadedSession.manifest->inputs.empty();
		if (sessionNeedsClearing && loadedSession.manifest->inputs.front().backupGeneration) {
			const auto& input = loadedSession.manifest->inputs.front();
			if (published) {
				if (loadedSession.manifest->generation != published->sessionGeneration
					|| input.workingCopyIdentity != published->backup.identity
					|| *input.backupGeneration != published->backup.generation) {
					return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
						EEditorWorkingCopyLifecycleReason::StaleSnapshot);
				}
				deleteTarget = published->backup;
			} else {
				if (input.workingCopyIdentity != event.identity) {
					return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
						EEditorWorkingCopyLifecycleReason::StaleSnapshot);
				}
				deleteTarget = BackupDeleteTarget{ input.workingCopyIdentity, *input.backupGeneration,
					event.contentVersion };
			}
		} else if (sessionNeedsClearing) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::BackupMismatch);
		} else if (published) {
			if (loadedSession.manifest->generation != published->sessionGeneration) {
				return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
					EEditorWorkingCopyLifecycleReason::StaleSnapshot);
			}
			deleteTarget = published->backup;
		}
	} else if (loadedSession.status == EWorkingCopyPersistenceLoadStatus::NotFound) {
		if (published) {
			return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
				EEditorWorkingCopyLifecycleReason::StaleSnapshot);
		}
	} else {
		return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
	if (deleteTarget) {
		const auto loadedBackup = m_backups.Load(scope, deleteTarget->identity);
		if (loadedBackup.status == EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::InvalidStoredRecord);
		}
		if (!loadedBackup.Loaded() || !loadedBackup.backup || !loadedBackup.backup->IsValid()
			|| loadedBackup.backup->scope != scope || loadedBackup.backup->identity != deleteTarget->identity
			|| loadedBackup.backup->generation != deleteTarget->generation
			|| loadedBackup.backup->contentVersion != deleteTarget->contentVersion) {
			return Result(EEditorWorkingCopyLifecycleStatus::Conflict,
				EEditorWorkingCopyLifecycleReason::StaleSnapshot);
		}
	}
	std::optional<std::string> emptySessionOperation;
	if (sessionNeedsClearing) {
		emptySessionOperation = NextOperationId("empty-session");
		if (!emptySessionOperation || !NextGeneration(sessionGeneration)) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		}
	}
	std::optional<std::string> deleteOperation;
	if (deleteTarget) {
		deleteOperation = NextOperationId("delete-backup");
		if (!deleteOperation) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
	}
	if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
		return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped);
	}
	bool mutated = false;
	if (sessionNeedsClearing) {
		const auto nextSessionGeneration = NextGeneration(sessionGeneration);
		if (!nextSessionGeneration) return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed);
		EditorSessionManifest empty{ .scope = scope, .generation = *nextSessionGeneration,
			.logicalGroupId = "workbench.editorGroup.primary" };
		const auto savedSession = m_sessions.Save(empty, sessionGeneration, *emptySessionOperation);
		if (!IsSaved(savedSession)) return StoreFailure(savedSession);
		mutated = true;
		if (savedSession.generation != empty.generation) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed, true);
		}
		sessionGeneration = savedSession.generation;
		{
			std::scoped_lock lock(m_mutex);
			m_stateScope = scope;
			m_sessionGeneration = savedSession.generation;
		}
		if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
			return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped, true);
		}
	}
	if (deleteTarget) {
		const auto deleted = m_backups.Delete(scope, deleteTarget->identity,
			deleteTarget->generation, *deleteOperation);
		if (!IsDeleted(deleted)) return StoreFailure(deleted, mutated);
		mutated = true;
		if (deleted.generation != deleteTarget->generation) {
			return Result(EEditorWorkingCopyLifecycleStatus::Failed, EEditorWorkingCopyLifecycleReason::StoreFailed, true);
		}
		if (ShutdownState() == EEditorWorkingCopyShutdownState::Stopped) {
			return Result(EEditorWorkingCopyLifecycleStatus::Stopped, EEditorWorkingCopyLifecycleReason::Stopped, true);
		}
	}
	std::scoped_lock lock(m_mutex);
	m_stateScope = scope;
	m_backupGeneration.reset();
	m_backupIdentity.reset();
	m_sessionGeneration = sessionGeneration;
	m_lastPersistedIdentity = event.identity;
	m_lastPersistedContentVersion = event.contentVersion;
	return Result(EEditorWorkingCopyLifecycleStatus::Succeeded, EEditorWorkingCopyLifecycleReason::None, mutated);
}

void EditorWorkingCopyLifecycle::BeginShutdown() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_shutdown == EEditorWorkingCopyShutdownState::Running) {
		m_shutdown = EEditorWorkingCopyShutdownState::BeforeShutdown;
	}
}

void EditorWorkingCopyLifecycle::WillShutdown() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_shutdown == EEditorWorkingCopyShutdownState::BeforeShutdown) {
		m_shutdown = EEditorWorkingCopyShutdownState::WillShutdown;
	}
}

void EditorWorkingCopyLifecycle::Stop() noexcept
{
	std::scoped_lock lock(m_mutex);
	m_shutdown = EEditorWorkingCopyShutdownState::Stopped;
	m_pendingScope.reset();
	m_pendingIdentity.reset();
	m_pendingContentVersion.reset();
}

EEditorWorkingCopyShutdownState EditorWorkingCopyLifecycle::ShutdownState() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_shutdown;
}

bool EditorWorkingCopyLifecycle::IsExactBackup(const WorkingCopyBackup& backup, const EditorSessionManifest& session,
	const EditorSessionInputDescriptor& input) noexcept
{
	return backup.scope == session.scope && backup.identity == input.workingCopyIdentity && input.backupGeneration
		&& backup.generation == *input.backupGeneration && backup.dirty
		&& backup.checksum == CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content);
}

std::optional<std::string> EditorWorkingCopyLifecycle::NextOperationId(const char* prefix)
{
	std::scoped_lock lock(m_mutex);
	if (m_nextOperation == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	return std::string("workbench.lifecycle.") + prefix + "." + std::to_string(m_nextOperation++);
}

} // namespace workbench::editor::persistence
