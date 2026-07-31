/*! @file @brief Hot Exit capture, session publication, and recovery coordination. */
/* Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/editor/persistence/EditorWorkingCopyLifecycleTypes.h"
#include "workbench/editor/persistence/IWorkingCopyPersistenceStore.h"

#include <mutex>
#include <optional>

namespace workbench::editor::persistence {

class EditorWorkingCopyLifecycle final {
public:
	EditorWorkingCopyLifecycle(IWorkingCopyBackupStore& backups, IEditorSessionStore& sessions,
		IEditorWorkingCopySnapshotSource& snapshots, IEditorWorkingCopyRecoveryApplier& applier,
		IEditorWorkingCopyRecoveredInputAdopter& adopter, EditorWorkingCopyLifecycleOptions options = {});

	[[nodiscard]] EditorWorkingCopyLifecycleResult Restore(const EditorWorkingCopyRestoreRequest& request);
	//! Records a lightweight change boundary. Full O(N) text capture occurs only in Flush.
	[[nodiscard]] EditorWorkingCopyLifecycleResult NotifyChanged(
		const WorkingCopyPersistenceScope& scope, const WorkingCopyPersistenceIdentity& identity,
		std::uint64_t contentVersion, std::uint64_t nowTicks);
	[[nodiscard]] EditorWorkingCopyLifecycleResult Flush(const WorkingCopyPersistenceScope& scope, std::uint64_t nowTicks, bool force = false);
	//! Call only after a successful save or completed close. A cancelled close must not call this method.
	//! Completion fences are opaque, bounded lifecycle revisions. A caller must capture
	//! one before beginning its native/core operation and present that same value when
	//! it has committed; an old fence can only replay its own completed cleanup.
	[[nodiscard]] std::optional<std::uint64_t> CaptureCompletionFence(
		const WorkingCopyPersistenceScope& scope) const noexcept;
	[[nodiscard]] EditorWorkingCopyLifecycleResult OnSavedOrClosed(const WorkingCopyPersistenceScope& scope,
		const EditorWorkingCopyCleanEvent& cleanEvent, std::uint64_t completionFence,
		bool allowIdentityReplacement = false);
	void BeginShutdown() noexcept;
	void WillShutdown() noexcept;
	void Stop() noexcept;
	[[nodiscard]] EEditorWorkingCopyShutdownState ShutdownState() const noexcept;

private:
	[[nodiscard]] EditorWorkingCopyLifecycleResult PersistDirty(const WorkingCopyPersistenceScope& scope,
		const EditorWorkingCopyPersistenceSnapshot& snapshot);
	[[nodiscard]] EditorWorkingCopyLifecycleResult PersistClean(const WorkingCopyPersistenceScope& scope,
		const EditorWorkingCopyCleanEvent& event, bool allowIdentityReplacement);
	[[nodiscard]] static bool IsExactBackup(const WorkingCopyBackup& backup, const EditorSessionManifest& session,
		const EditorSessionInputDescriptor& input) noexcept;
	[[nodiscard]] std::optional<std::string> NextOperationId(const char* prefix);

	IWorkingCopyBackupStore& m_backups;
	IEditorSessionStore& m_sessions;
	IEditorWorkingCopySnapshotSource& m_snapshots;
	IEditorWorkingCopyRecoveryApplier& m_applier;
	IEditorWorkingCopyRecoveredInputAdopter& m_adopter;
	const EditorWorkingCopyLifecycleOptions m_options;
	mutable std::mutex m_mutex;
	EEditorWorkingCopyShutdownState m_shutdown = EEditorWorkingCopyShutdownState::Running;
	bool m_inFlight = false;
	std::uint64_t m_nextOperation = 1;
	std::optional<WorkingCopyPersistenceScope> m_pendingScope;
	std::optional<WorkingCopyPersistenceIdentity> m_pendingIdentity;
	std::optional<std::uint64_t> m_pendingContentVersion;
	std::uint64_t m_firstPendingTick = 0;
	std::uint64_t m_lastPendingTick = 0;
	std::optional<std::uint64_t> m_backupGeneration;
	std::optional<WorkingCopyPersistenceIdentity> m_backupIdentity;
	std::optional<std::uint64_t> m_sessionGeneration;
	std::optional<WorkingCopyPersistenceScope> m_stateScope;
	std::optional<WorkingCopyPersistenceIdentity> m_lastPersistedIdentity;
	std::optional<std::uint64_t> m_lastPersistedContentVersion;
	//! Advances only after a complete durable publication or cleanup. It is never an
	//! operation ID and is intentionally local to this lifecycle instance.
	std::uint64_t m_completionFence = 1;
	std::optional<std::uint64_t> m_lastCompletedFence;
	std::optional<WorkingCopyPersistenceScope> m_lastCompletedScope;
	std::optional<EditorWorkingCopyCleanEvent> m_lastCompletedCleanEvent;
};

} // namespace workbench::editor::persistence
