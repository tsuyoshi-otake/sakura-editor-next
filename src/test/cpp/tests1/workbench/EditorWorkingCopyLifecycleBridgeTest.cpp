/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <optional>
#include <string>

namespace workbench::editor::persistence {
namespace {

WorkingCopyPersistenceScope Scope()
{
	return { .profileId = "profile.bridge", .workspaceId = "workspace.bridge" };
}

WorkingCopyPersistenceIdentity Identity()
{
	return {
		.typeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		.opaqueId = "untitled.bridge",
	};
}

WorkingCopyPersistenceIdentity NamedIdentity()
{
	return {
		.typeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		.canonicalResource = "file:///C:/workspace/saved.txt",
	};
}

EditorWorkingCopyPersistenceSnapshot Snapshot(std::uint64_t version = 3)
{
	return {
		.identity = Identity(),
		.inputId = "input.bridge",
		.inputTypeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		.contentVersion = version,
		.dirty = true,
		.encoding = EWorkingCopyTextEncoding::Utf8,
		.eol = EWorkingCopyEol::Lf,
		.content = "bridge content",
	};
}

class Store final : public IWorkingCopyBackupStore, public IEditorSessionStore {
public:
	std::optional<WorkingCopyBackup> backup;
	std::optional<EditorSessionManifest> session;
	int sessionLoadCalls = 0;
	int backupSaveCalls = 0;
	int backupDeleteCalls = 0;

	WorkingCopyBackupLoadResult Load(const WorkingCopyPersistenceScope& scope,
		const WorkingCopyPersistenceIdentity& identity) override
	{
		if (!backup || backup->scope != scope || backup->identity != identity) {
			return { .status = EWorkingCopyPersistenceLoadStatus::NotFound };
		}
		return { .status = EWorkingCopyPersistenceLoadStatus::Loaded, .backup = backup };
	}

	WorkingCopyPersistenceWriteResult Save(const WorkingCopyBackup& value,
		std::optional<std::uint64_t>, const std::string&) override
	{
		++backupSaveCalls;
		backup = value;
		return { .status = EWorkingCopyPersistenceWriteStatus::Persisted, .generation = value.generation };
	}

	WorkingCopyPersistenceWriteResult Delete(const WorkingCopyPersistenceScope&,
		const WorkingCopyPersistenceIdentity&, std::uint64_t expectedGeneration, const std::string&) override
	{
		++backupDeleteCalls;
		backup.reset();
		return { .status = EWorkingCopyPersistenceWriteStatus::Deleted, .generation = expectedGeneration };
	}

	EditorSessionLoadResult Load(const WorkingCopyPersistenceScope& scope) override
	{
		++sessionLoadCalls;
		if (!session || session->scope != scope) {
			return { .status = EWorkingCopyPersistenceLoadStatus::NotFound };
		}
		return { .status = EWorkingCopyPersistenceLoadStatus::Loaded, .manifest = session };
	}

	WorkingCopyPersistenceWriteResult Save(const EditorSessionManifest& value,
		std::optional<std::uint64_t>, const std::string&) override
	{
		session = value;
		return { .status = EWorkingCopyPersistenceWriteStatus::Persisted, .generation = value.generation };
	}

	WorkingCopyPersistenceWriteResult Delete(const WorkingCopyPersistenceScope&,
		std::uint64_t expectedGeneration, const std::string&) override
	{
		session.reset();
		return { .status = EWorkingCopyPersistenceWriteStatus::Deleted, .generation = expectedGeneration };
	}
};

class SnapshotSource final : public IEditorWorkingCopySnapshotSource {
public:
	EditorWorkingCopyPersistenceSnapshot snapshot = Snapshot();
	int captureCalls = 0;

	EditorWorkingCopySnapshotResult Capture() override
	{
		++captureCalls;
		return { .status = EEditorWorkingCopySnapshotStatus::Captured, .snapshot = snapshot };
	}
};

class Applier final : public IEditorWorkingCopyRecoveryApplier {
public:
	EditorWorkingCopyRecoveryPrepareResult Prepare(const EditorWorkingCopyRecoveryRequest&) override
	{
		return { .status = EEditorWorkingCopyRecoveryStatus::Failed };
	}
	EEditorWorkingCopyRecoveryCommitStatus Commit(
		const EditorWorkingCopyRecoveryRequest&) noexcept override
	{
		return EEditorWorkingCopyRecoveryCommitStatus::NotPrepared;
	}
};

class Adopter final : public IEditorWorkingCopyRecoveredInputAdopter {
public:
	bool AdoptInactive(const EditorSessionInputDescriptor&, const EditorDocumentIdentity&, std::uint64_t) override
	{
		return false;
	}
	bool RollbackInactive(const EditorSessionInputDescriptor&, const EditorDocumentIdentity&,
		std::uint64_t) noexcept override
	{
		return false;
	}
};

class CurrentChangeSource final : public IEditorWorkingCopyCurrentChangeSource {
public:
	std::optional<EditorWorkingCopyCurrentChange> current{
		EditorWorkingCopyCurrentChange{ .identity = Identity(), .contentVersion = 3 } };
	mutable int calls = 0;

	std::optional<EditorWorkingCopyCurrentChange> CurrentChange() const override
	{
		++calls;
		return current;
	}
};

struct Fixture final {
	Store store;
	SnapshotSource snapshots;
	Applier applier;
	Adopter adopter;
	CurrentChangeSource current;
	EditorWorkingCopyLifecycle lifecycle{ store, store, snapshots, applier, adopter,
		{ .debounceTicks = 10, .maximumAgeTicks = 30 } };
	EditorWorkingCopyLifecycleBridge bridge{ Scope(), lifecycle, current };
};

TEST(EditorWorkingCopyLifecycleBridge, RestoreForwardsPolicyAndLayoutReadinessWithoutReadingCurrentChange)
{
	Fixture fixture;
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		fixture.bridge.Restore({}, false).status);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Suppressed,
		fixture.bridge.Restore({ .explicitCommandLine = true }, true).status);
	EXPECT_EQ(0, fixture.store.sessionLoadCalls);
	EXPECT_EQ(0, fixture.current.calls);
}

TEST(EditorWorkingCopyLifecycleBridge, ChangeAndCompletionBoundariesNeverCaptureTextAndFlushDoes)
{
	Fixture fixture;
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		fixture.bridge.NotifyCurrentChanged(0).status);
	EXPECT_EQ(0, fixture.snapshots.captureCalls);

	const auto token = fixture.bridge.CaptureCurrentCompletionToken();
	ASSERT_TRUE(token);
	EXPECT_EQ(0, fixture.snapshots.captureCalls);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded,
		fixture.bridge.Flush(0, true).status);
	EXPECT_EQ(1, fixture.snapshots.captureCalls);
	EXPECT_EQ(1, fixture.store.backupSaveCalls);

	fixture.current.current->contentVersion = 4;
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		fixture.bridge.CompleteCurrentSave(*token).status);
	EXPECT_EQ(1, fixture.snapshots.captureCalls);
	EXPECT_TRUE(fixture.store.backup);
}

TEST(EditorWorkingCopyLifecycleBridge, SaveAsExplicitlyAllowsIdentityReplacementAtTheSameVersion)
{
	Fixture fixture;
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		fixture.bridge.NotifyCurrentChanged(0).status);
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded,
		fixture.bridge.Flush(0, true).status);
	const auto token = fixture.bridge.CaptureCurrentCompletionToken();
	ASSERT_TRUE(token);
	fixture.current.current->identity = NamedIdentity();

	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Conflict,
		fixture.bridge.CompleteCurrentSave(*token).status);
	EXPECT_TRUE(fixture.store.backup);

	const auto completed = fixture.bridge.CompleteCurrentSave(*token,
		EEditorWorkingCopySaveCompletionMode::AllowIdentityReplacement);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, completed.status);
	EXPECT_TRUE(completed.mutatedDurableState);
	EXPECT_FALSE(fixture.store.backup);
}

TEST(EditorWorkingCopyLifecycleBridge, CompletedPreCloseUsesThePreCloseTokenAfterCoreRemovesCurrentInput)
{
	Fixture fixture;
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		fixture.bridge.NotifyCurrentChanged(0).status);
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded,
		fixture.bridge.Flush(0, true).status);
	const auto token = fixture.bridge.CaptureCurrentCompletionToken();
	ASSERT_TRUE(token);
	fixture.current.current.reset(); // Core close has already committed and removed the active input.

	const auto completed = fixture.bridge.CompletePreClose(*token);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, completed.status);
	EXPECT_TRUE(completed.mutatedDurableState);
	EXPECT_EQ(1, fixture.snapshots.captureCalls);
	EXPECT_EQ(1, fixture.store.backupDeleteCalls);
}

TEST(EditorWorkingCopyLifecycleBridge, DuplicateCompletionTokenIsAnExactNoMutationReplay)
{
	Fixture fixture;
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Deferred,
		fixture.bridge.NotifyCurrentChanged(0).status);
	ASSERT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded,
		fixture.bridge.Flush(0, true).status);
	const auto token = fixture.bridge.CaptureCurrentCompletionToken();
	ASSERT_TRUE(token);

	const auto first = fixture.bridge.CompleteCurrentSave(*token);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, first.status);
	EXPECT_TRUE(first.mutatedDurableState);
	const auto duplicate = fixture.bridge.CompleteCurrentSave(*token);
	EXPECT_EQ(EEditorWorkingCopyLifecycleStatus::Succeeded, duplicate.status);
	EXPECT_FALSE(duplicate.mutatedDurableState);
	EXPECT_EQ(1, fixture.store.backupDeleteCalls);
}

TEST(EditorWorkingCopyLifecycleBridge, DelegatesTheExplicitShutdownSequence)
{
	Fixture fixture;
	EXPECT_EQ(EEditorWorkingCopyShutdownState::Running, fixture.bridge.ShutdownState());
	fixture.bridge.BeginShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::BeforeShutdown, fixture.bridge.ShutdownState());
	fixture.bridge.WillShutdown();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::WillShutdown, fixture.bridge.ShutdownState());
	fixture.bridge.Stop();
	EXPECT_EQ(EEditorWorkingCopyShutdownState::Stopped, fixture.bridge.ShutdownState());
}

} // namespace
} // namespace workbench::editor::persistence
