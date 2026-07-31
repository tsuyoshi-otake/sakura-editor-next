/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib */
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/editor/persistence/EditorWorkingCopyLifecycle.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace workbench::editor::persistence {
namespace {

WorkingCopyPersistenceScope Scope(std::string profile = "profile.test")
{
	return { std::move(profile), "workspace.test" };
}

WorkingCopyPersistenceIdentity UntitledIdentity(std::string opaqueId = "untitled.test")
{
	return { "workbench.editor.text", std::nullopt, std::move(opaqueId) };
}

WorkingCopyPersistenceIdentity FileIdentity(std::string resource = "file:///c%3A/test/new.txt")
{
	return { "workbench.editor.text", std::move(resource), std::nullopt };
}

EditorWorkingCopyPersistenceSnapshot Snapshot(
	std::uint64_t version = 1, bool dirty = true,
	WorkingCopyPersistenceIdentity identity = UntitledIdentity())
{
	return { std::move(identity), "input.test", std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		version, dirty, EWorkingCopyTextEncoding::Utf8, EWorkingCopyEol::Lf, "text" };
}

EditorWorkingCopyCleanEvent Clean(
	std::uint64_t version = 1, WorkingCopyPersistenceIdentity identity = UntitledIdentity())
{
	return { std::move(identity), version };
}

class MemoryStore final : public IWorkingCopyBackupStore, public IEditorSessionStore {
public:
	std::optional<WorkingCopyBackup> backup;
	std::optional<EditorSessionManifest> session;
	std::optional<EWorkingCopyPersistenceWriteStatus> backupSaveStatus;
	std::optional<EWorkingCopyPersistenceWriteStatus> backupDeleteStatus;
	std::optional<EWorkingCopyPersistenceWriteStatus> sessionSaveStatus;
	std::function<void()> onBackupSave;
	std::function<void()> onBackupDelete;
	std::function<void()> onSessionSave;
	std::optional<WorkingCopyPersistenceIdentity> lastDeletedIdentity;
	int backupLoadCalls = 0;
	int sessionLoadCalls = 0;
	int backupSaveCalls = 0;
	int backupDeleteCalls = 0;
	int sessionSaveCalls = 0;

	WorkingCopyBackupLoadResult Load(
		const WorkingCopyPersistenceScope& scope,
		const WorkingCopyPersistenceIdentity& identity) override
	{
		++backupLoadCalls;
		if (!backup || backup->scope != scope || backup->identity != identity) {
			return { EWorkingCopyPersistenceLoadStatus::NotFound };
		}
		return { EWorkingCopyPersistenceLoadStatus::Loaded, backup };
	}

	WorkingCopyPersistenceWriteResult Save(
		const WorkingCopyBackup& value,
		std::optional<std::uint64_t> expected,
		const std::string&) override
	{
		++backupSaveCalls;
		if (backupSaveStatus && *backupSaveStatus != EWorkingCopyPersistenceWriteStatus::Persisted) {
			return { *backupSaveStatus, backup ? backup->generation : 0 };
		}
		if ((backup && (!expected || *expected != backup->generation)) || (!backup && expected)) {
			return { EWorkingCopyPersistenceWriteStatus::Conflict, backup ? backup->generation : 0 };
		}
		backup = value;
		if (onBackupSave) onBackupSave();
		return { EWorkingCopyPersistenceWriteStatus::Persisted, value.generation };
	}

	WorkingCopyPersistenceWriteResult Delete(
		const WorkingCopyPersistenceScope& scope,
		const WorkingCopyPersistenceIdentity& identity,
		std::uint64_t expected,
		const std::string&) override
	{
		++backupDeleteCalls;
		lastDeletedIdentity = identity;
		if (backupDeleteStatus && *backupDeleteStatus != EWorkingCopyPersistenceWriteStatus::Deleted) {
			return { *backupDeleteStatus, backup ? backup->generation : 0 };
		}
		if (!backup || backup->scope != scope || backup->identity != identity || backup->generation != expected) {
			return { EWorkingCopyPersistenceWriteStatus::Conflict, backup ? backup->generation : 0 };
		}
		backup.reset();
		if (onBackupDelete) onBackupDelete();
		return { EWorkingCopyPersistenceWriteStatus::Deleted, expected };
	}

	EditorSessionLoadResult Load(const WorkingCopyPersistenceScope& scope) override
	{
		++sessionLoadCalls;
		if (!session || session->scope != scope) return { EWorkingCopyPersistenceLoadStatus::NotFound };
		return { EWorkingCopyPersistenceLoadStatus::Loaded, session };
	}

	WorkingCopyPersistenceWriteResult Save(
		const EditorSessionManifest& value,
		std::optional<std::uint64_t> expected,
		const std::string&) override
	{
		++sessionSaveCalls;
		if (sessionSaveStatus && *sessionSaveStatus != EWorkingCopyPersistenceWriteStatus::Persisted) {
			return { *sessionSaveStatus, session ? session->generation : 0 };
		}
		if ((session && (!expected || *expected != session->generation)) || (!session && expected)) {
			return { EWorkingCopyPersistenceWriteStatus::Conflict, session ? session->generation : 0 };
		}
		session = value;
		if (onSessionSave) onSessionSave();
		return { EWorkingCopyPersistenceWriteStatus::Persisted, value.generation };
	}

	WorkingCopyPersistenceWriteResult Delete(
		const WorkingCopyPersistenceScope&, std::uint64_t, const std::string&) override
	{
		return { EWorkingCopyPersistenceWriteStatus::Deleted };
	}
};

class Source final : public IEditorWorkingCopySnapshotSource {
public:
	EditorWorkingCopySnapshotResult value{ EEditorWorkingCopySnapshotStatus::Captured, Snapshot() };
	int captureCalls = 0;

	EditorWorkingCopySnapshotResult Capture() override
	{
		++captureCalls;
		return value;
	}
};

class Applier final : public IEditorWorkingCopyRecoveryApplier {
public:
	int prepared = 0;
	int aborted = 0;
	int committed = 0;
	EEditorWorkingCopyRecoveryStatus status = EEditorWorkingCopyRecoveryStatus::Prepared;
	EEditorWorkingCopyRecoveryCommitStatus commitStatus = EEditorWorkingCopyRecoveryCommitStatus::Committed;
	std::vector<std::string>* order = nullptr;
	std::function<void()> onPrepare;
	bool throwOnPrepare = false;

	EditorWorkingCopyRecoveryPrepareResult Prepare(const EditorWorkingCopyRecoveryRequest&) override
	{
		++prepared;
		if (order) order->push_back("prepare");
		if (onPrepare) onPrepare();
		if (throwOnPrepare) throw std::runtime_error("prepare");
		return { status, status == EEditorWorkingCopyRecoveryStatus::Prepared
			? std::optional(EditorDocumentIdentity{ .opaqueId = "core.recovered" }) : std::nullopt };
	}

	void AbortPrepared() noexcept override
	{
		++aborted;
		if (order) order->push_back("abort");
	}

	EEditorWorkingCopyRecoveryCommitStatus Commit(const EditorWorkingCopyRecoveryRequest&) noexcept override
	{
		++committed;
		if (order) order->push_back("commit");
		return commitStatus;
	}
};

class Adopter final : public IEditorWorkingCopyRecoveredInputAdopter {
public:
	int calls = 0;
	int rollbacks = 0;
	bool accepted = true;
	bool rollbackAccepted = true;
	bool throwOnAdopt = false;
	std::vector<std::string>* order = nullptr;

	bool AdoptInactive(const EditorSessionInputDescriptor&, const EditorDocumentIdentity&, std::uint64_t) override
	{
		++calls;
		if (order) order->push_back("adopt");
		if (throwOnAdopt) throw std::runtime_error("adopt");
		return accepted;
	}

	bool RollbackInactive(const EditorSessionInputDescriptor&, const EditorDocumentIdentity&,
		std::uint64_t) noexcept override
	{
		++rollbacks;
		if (order) order->push_back("rollback");
		return rollbackAccepted;
	}
};

EditorWorkingCopyLifecycle Make(MemoryStore& store, Source& source, Applier& applier, Adopter& adopter)
{
	return EditorWorkingCopyLifecycle(store, store, source, applier, adopter,
		{ .debounceTicks = 10, .maximumAgeTicks = 30 });
}

void PublishDirty(EditorWorkingCopyLifecycle& lifecycle, Source& source,
	const WorkingCopyPersistenceScope& scope, const EditorWorkingCopyPersistenceSnapshot& snapshot)
{
	source.value = { EEditorWorkingCopySnapshotStatus::Captured, snapshot };
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(scope, snapshot.identity, snapshot.contentVersion, 0).status);
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(scope, 0, true).status);
}

TEST(EditorWorkingCopyLifecycle, RestoreSuppressionNeverLoadsOrApplies)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Suppressed,
		lifecycle.Restore({ Scope(), true, true }).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Suppressed,
		lifecycle.Restore({ Scope(), true, false, true }).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Suppressed,
		lifecycle.Restore({ Scope(), true, false, false, true }).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.Restore({ Scope(), false }).status);
	EXPECT_EQ(0, store.sessionLoadCalls);
	EXPECT_EQ(0, store.backupLoadCalls);
	EXPECT_EQ(0, applier.prepared);
}

TEST(EditorWorkingCopyLifecycle, ChangeBoundaryIsLightweightAndDebounceCoalescesLatestVersion)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	const auto first = Snapshot(1);
	const auto second = Snapshot(2);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), first.identity, 1, 0).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), second.identity, 2, 5).status);
	EXPECT_EQ(0, source.captureCalls);
	source.value = { EEditorWorkingCopySnapshotStatus::Captured, second };
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred, lifecycle.Flush(Scope(), 14).status);
	EXPECT_EQ(0, source.captureCalls);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(Scope(), 15).status);
	EXPECT_EQ(1, source.captureCalls);
	ASSERT_TRUE(store.backup);
	EXPECT_EQ(2U, store.backup->contentVersion);
	EXPECT_EQ(1, store.sessionSaveCalls);
	ASSERT_TRUE(store.session);
	EXPECT_EQ(std::optional<std::string>("input.test"), store.session->activeInputId);
}

TEST(EditorWorkingCopyLifecycle, MaximumAgeForcesProgressDespiteContinuedEdits)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	const auto identity = UntitledIdentity();
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), identity, 1, 0).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), identity, 2, 25).status);
	source.value = { EEditorWorkingCopySnapshotStatus::Captured, Snapshot(2) };
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(Scope(), 30).status);
}

TEST(EditorWorkingCopyLifecycle, RestoreAdoptsInactiveBeforeNoThrowNativeCommit)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot(3));

	Source restoreSource;
	Applier restoreApplier;
	Adopter restoreAdopter;
	std::vector<std::string> order;
	restoreApplier.order = &order;
	restoreAdopter.order = &order;
	auto lifecycle = Make(store, restoreSource, restoreApplier, restoreAdopter);
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, result.status);
	EXPECT_EQ(std::optional<std::string>("input.test"), result.restoredInputId);
	EXPECT_EQ(std::optional<std::string>("input.test"), result.effectiveActiveInputId);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "adopt", "commit" }), order);
	EXPECT_EQ(1, restoreApplier.committed);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::NotApplicable,
		lifecycle.NotifyChanged(Scope(), UntitledIdentity(), 3, 10).status);
}

TEST(EditorWorkingCopyLifecycle, AdoptionFailureNeverCommitsPreparedNativeDocument)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());
	Applier restoreApplier;
	Adopter rejected;
	rejected.accepted = false;
	auto lifecycle = Make(store, source, restoreApplier, rejected);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed,
		lifecycle.Restore({ Scope(), true }).status);
	EXPECT_EQ(1, rejected.calls);
	EXPECT_EQ(0, restoreApplier.committed);
	EXPECT_EQ(1, restoreApplier.aborted);
}

TEST(EditorWorkingCopyLifecycle, LegacySingletonWithoutActiveInputMigratesItsEffectivePlacementOnly)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());
	ASSERT_TRUE(store.session);
	store.session->activeInputId.reset();

	Applier restoreApplier;
	Adopter restoreAdopter;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, result.status);
	EXPECT_EQ(std::optional<std::string>("input.test"), result.restoredInputId);
	EXPECT_EQ(std::optional<std::string>("input.test"), result.effectiveActiveInputId);
	ASSERT_TRUE(store.session);
	EXPECT_FALSE(store.session->activeInputId);
}

TEST(EditorWorkingCopyLifecycle, RestoreRejectsAStoredActiveInputThatIsNotTheRecoveredInput)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());
	ASSERT_TRUE(store.session);
	store.session->activeInputId = "input.other";

	Applier restoreApplier;
	Adopter restoreAdopter;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, result.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::InvalidStoredRecord, result.reason);
	EXPECT_EQ(0, restoreApplier.prepared);
}

TEST(EditorWorkingCopyLifecycle, StoppedAfterPrepareAbortsStagedNativeRecovery)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());

	Applier restoreApplier;
	Adopter restoreAdopter;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	restoreApplier.onPrepare = [&] { lifecycle.Stop(); };
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Stopped, result.status);
	EXPECT_EQ(1, restoreApplier.prepared);
	EXPECT_EQ(1, restoreApplier.aborted);
	EXPECT_EQ(0, restoreApplier.committed);
	EXPECT_EQ(0, restoreAdopter.calls);
}

TEST(EditorWorkingCopyLifecycle, ThrowingAdoptionAbortsAndDoesNotLeaveRestoreInFlight)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());

	Applier restoreApplier;
	Adopter restoreAdopter;
	restoreAdopter.throwOnAdopt = true;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	const auto first = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, first.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::CoreAdoptionFailed, first.reason);
	EXPECT_EQ(1, restoreApplier.aborted);

	restoreAdopter.throwOnAdopt = false;
	const auto retry = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, retry.status);
	EXPECT_EQ(2, restoreApplier.prepared);
	EXPECT_EQ(1, restoreApplier.aborted);
	EXPECT_EQ(1, restoreApplier.committed);
}

TEST(EditorWorkingCopyLifecycle, ChangedNativeTargetCompensatesInactiveCoreAdoption)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());

	Applier restoreApplier;
	restoreApplier.commitStatus = EEditorWorkingCopyRecoveryCommitStatus::TargetChanged;
	Adopter restoreAdopter;
	std::vector<std::string> order;
	restoreApplier.order = &order;
	restoreAdopter.order = &order;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, result.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::ApplierFailed, result.reason);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "adopt", "commit", "rollback" }), order);
	EXPECT_EQ(1, restoreAdopter.rollbacks);
}

TEST(EditorWorkingCopyLifecycle, FailedCompensationIsExplicitlyTerminal)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());

	Applier restoreApplier;
	restoreApplier.commitStatus = EEditorWorkingCopyRecoveryCommitStatus::TargetChanged;
	Adopter restoreAdopter;
	restoreAdopter.rollbackAccepted = false;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	const auto result = lifecycle.Restore({ Scope(), true });
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, result.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::CoreRollbackFailed, result.reason);
	EXPECT_EQ(1, restoreAdopter.rollbacks);
}

TEST(EditorWorkingCopyLifecycle, CorruptBackupNeverInvokesRecoveryApplier)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto publisher = Make(store, source, applier, adopter);
	PublishDirty(publisher, source, Scope(), Snapshot());
	ASSERT_TRUE(store.backup);
	store.backup->checksum = "0000000000000000";
	Applier restoreApplier;
	Adopter restoreAdopter;
	auto lifecycle = Make(store, source, restoreApplier, restoreAdopter);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed,
		lifecycle.Restore({ Scope(), true }).status);
	EXPECT_EQ(0, restoreApplier.prepared);
	EXPECT_EQ(0, restoreAdopter.calls);
}

TEST(EditorWorkingCopyLifecycle, SaveAsUnpublishesSessionBeforeDeletingOldIdentity)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	const auto oldIdentity = UntitledIdentity();
	PublishDirty(lifecycle, source, Scope(), Snapshot(1, true, oldIdentity));
	bool sessionWasEmptyAtDelete = false;
	store.onBackupDelete = [&] {
		sessionWasEmptyAtDelete = store.session && store.session->inputs.empty();
	};
	const auto clean = Clean(1, FileIdentity());
	const auto fence = lifecycle.CaptureCompletionFence(Scope());
	ASSERT_TRUE(fence);
	const auto result = lifecycle.OnSavedOrClosed(Scope(), clean, *fence, true);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, result.status);
	EXPECT_TRUE(result.mutatedDurableState);
	EXPECT_TRUE(sessionWasEmptyAtDelete);
	ASSERT_TRUE(store.lastDeletedIdentity);
	EXPECT_EQ(oldIdentity, *store.lastDeletedIdentity);
	EXPECT_FALSE(store.backup);
	ASSERT_TRUE(store.session);
	EXPECT_TRUE(store.session->inputs.empty());
}

TEST(EditorWorkingCopyLifecycle, FailedSessionUnpublishRetainsRecoverableBackupAndReference)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	PublishDirty(lifecycle, source, Scope(), Snapshot());
	store.sessionSaveStatus = EWorkingCopyPersistenceWriteStatus::Failed;
	const auto fence = lifecycle.CaptureCompletionFence(Scope());
	ASSERT_TRUE(fence);
	const auto result = lifecycle.OnSavedOrClosed(Scope(), Clean(), *fence);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, result.status);
	EXPECT_FALSE(result.mutatedDurableState);
	EXPECT_TRUE(store.backup);
	ASSERT_TRUE(store.session);
	EXPECT_EQ(1U, store.session->inputs.size());
	EXPECT_EQ(0, store.backupDeleteCalls);
}

TEST(EditorWorkingCopyLifecycle, FailedBackupDeleteLeavesOnlyAnUnreferencedRetryableBackup)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	PublishDirty(lifecycle, source, Scope(), Snapshot());
	store.backupDeleteStatus = EWorkingCopyPersistenceWriteStatus::Failed;
	const auto fence = lifecycle.CaptureCompletionFence(Scope());
	ASSERT_TRUE(fence);
	const auto first = lifecycle.OnSavedOrClosed(Scope(), Clean(), *fence);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Failed, first.status);
	EXPECT_TRUE(first.mutatedDurableState);
	EXPECT_TRUE(store.backup);
	ASSERT_TRUE(store.session);
	EXPECT_TRUE(store.session->inputs.empty());
	store.backupDeleteStatus.reset();
	const auto retry = lifecycle.OnSavedOrClosed(Scope(), Clean(), *fence);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, retry.status);
	EXPECT_FALSE(store.backup);
}

TEST(EditorWorkingCopyLifecycle, ReentrantNewerChangeSurvivesOlderBackupPublication)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	const auto identity = UntitledIdentity();
	source.value = { EEditorWorkingCopySnapshotStatus::Captured, Snapshot(1) };
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), identity, 1, 0).status);
	EditorWorkingCopyLifecycleResult reentrant;
	store.onBackupSave = [&] {
		source.value = { EEditorWorkingCopySnapshotStatus::Captured, Snapshot(2) };
		reentrant = lifecycle.NotifyChanged(Scope(), identity, 2, 20);
	};
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(Scope(), 0, true).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred, reentrant.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::OperationInFlight, reentrant.reason);
	store.onBackupSave = {};
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(Scope(), 20, true).status);
	ASSERT_TRUE(store.backup);
	EXPECT_EQ(2U, store.backup->contentVersion);
	EXPECT_EQ(2, store.backupSaveCalls);
}

TEST(EditorWorkingCopyLifecycle, StopDuringBackupSavePreventsSessionPublication)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), UntitledIdentity(), 1, 0).status);
	store.onBackupSave = [&] { lifecycle.Stop(); };
	const auto result = lifecycle.Flush(Scope(), 0, true);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Stopped, result.status);
	EXPECT_TRUE(result.mutatedDurableState);
	EXPECT_TRUE(store.backup);
	EXPECT_EQ(0, store.sessionSaveCalls);
}

TEST(EditorWorkingCopyLifecycle, IdentityScopeAndVersionMismatchesAreExplicitConflicts)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	const auto identity = UntitledIdentity();
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), identity, 3, 0).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		lifecycle.NotifyChanged(Scope(), FileIdentity(), 4, 1).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		lifecycle.NotifyChanged(Scope("other.profile"), identity, 4, 1).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		lifecycle.NotifyChanged(Scope(), identity, 2, 1).status);
	source.value = { EEditorWorkingCopySnapshotStatus::Captured, Snapshot(3) };
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, lifecycle.Flush(Scope(), 1, true).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::NotApplicable,
		lifecycle.NotifyChanged(Scope(), identity, 3, 2).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		lifecycle.NotifyChanged(Scope(), identity, 2, 2).status);
}

TEST(EditorWorkingCopyLifecycle, MultiInputSessionIsExplicitlyUnsupportedAndPreserved)
{
	MemoryStore store;
	const auto backup = Snapshot();
	store.backup = WorkingCopyBackup{ Scope(), backup.identity, 1, 1,
		CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content), backup.encoding,
		backup.eol, true, backup.content };
	EditorSessionInputDescriptor first{ "input.one", std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		backup.identity, 1, {}, 1 };
	auto second = first;
	second.inputId = "input.two";
	store.session = EditorSessionManifest{ Scope(), 1, "workbench.editorGroup.primary",
		std::nullopt, { first, second } };
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Unsupported,
		lifecycle.Restore({ Scope(), true }).status);
	EXPECT_EQ(0, applier.prepared);
	EXPECT_EQ(0, store.backupDeleteCalls);
}

TEST(EditorWorkingCopyLifecycle, ShutdownTransitionsAreMonotonicAndStoppedIsTerminal)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		lifecycle.NotifyChanged(Scope(), UntitledIdentity(), 1, 0).status);
	lifecycle.WillShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::Running, lifecycle.ShutdownState());
	lifecycle.BeginShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::BeforeShutdown, lifecycle.ShutdownState());
	const auto duringShutdown = lifecycle.NotifyChanged(Scope(), UntitledIdentity(), 2, 1);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred, duringShutdown.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::ShutdownInProgress, duringShutdown.reason);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Suppressed,
		lifecycle.Restore({ Scope(), true }).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded,
		lifecycle.Flush(Scope(), 1, true).status);
	lifecycle.WillShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::WillShutdown, lifecycle.ShutdownState());
	lifecycle.BeginShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::WillShutdown, lifecycle.ShutdownState());
	lifecycle.Stop();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::Stopped, lifecycle.ShutdownState());
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Stopped,
		lifecycle.NotifyChanged(Scope(), UntitledIdentity(), 2, 2).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Stopped,
		lifecycle.Flush(Scope(), 2, true).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Stopped,
		lifecycle.OnSavedOrClosed(Scope(), Clean(2), 1).status);
}

TEST(EditorWorkingCopyLifecycle, StaleCompletionFencePreservesNewerBackupAndSessionPublication)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	PublishDirty(lifecycle, source, Scope(), Snapshot(1));
	const auto oldFence = lifecycle.CaptureCompletionFence(Scope());
	ASSERT_TRUE(oldFence);

	PublishDirty(lifecycle, source, Scope(), Snapshot(2));
	const auto result = lifecycle.OnSavedOrClosed(Scope(), Clean(1), *oldFence);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict, result.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::StaleSnapshot, result.reason);
	ASSERT_TRUE(store.backup);
	EXPECT_EQ(2U, store.backup->contentVersion);
	ASSERT_TRUE(store.session);
	ASSERT_EQ(1U, store.session->inputs.size());
	EXPECT_EQ(2U, *store.session->inputs.front().backupGeneration);
	EXPECT_EQ(0, store.backupDeleteCalls);
}

TEST(EditorWorkingCopyLifecycle, CompletionNeverCleansAStoreGenerationReplacedAfterItsFence)
{
	MemoryStore store;
	Source source;
	Applier applier;
	Adopter adopter;
	auto lifecycle = Make(store, source, applier, adopter);
	PublishDirty(lifecycle, source, Scope(), Snapshot(1));
	const auto fence = lifecycle.CaptureCompletionFence(Scope());
	ASSERT_TRUE(fence);

	const auto replacement = Snapshot(2);
	store.backup = WorkingCopyBackup{ Scope(), replacement.identity, 2, replacement.contentVersion,
		CWorkingCopyPersistenceCodec::ComputeContentChecksum(replacement.content), replacement.encoding,
		replacement.eol, true, replacement.content };
	store.session = EditorSessionManifest{ Scope(), 2, "workbench.editorGroup.primary", std::nullopt,
		{ { replacement.inputId, replacement.inputTypeId, replacement.identity, 1, {}, 2 } } };

	const auto result = lifecycle.OnSavedOrClosed(Scope(), Clean(), *fence);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict, result.status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleReason::StaleSnapshot, result.reason);
	ASSERT_TRUE(store.backup);
	EXPECT_EQ(2U, store.backup->generation);
	ASSERT_TRUE(store.session);
	EXPECT_EQ(2U, store.session->generation);
	ASSERT_EQ(1U, store.session->inputs.size());
	EXPECT_EQ(2U, *store.session->inputs.front().backupGeneration);
	EXPECT_EQ(0, store.backupDeleteCalls);
}

} // namespace
} // namespace workbench::editor::persistence
