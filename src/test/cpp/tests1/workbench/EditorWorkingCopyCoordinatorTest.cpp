/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "workbench/editor/EditorWorkingCopyCoordinator.h"

namespace workbench::editor {
namespace {

EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
{
	auto uri = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(uri);
	return { .resource = std::move(*uri.value) };
}

class FakeWorkingCopyBackend final : public IEditorWorkingCopyBackend {
public:
	EditorWorkingCopyBackendResult saveResult{ .status = EEditorWorkingCopyBackendStatus::Succeeded };
	EditorWorkingCopyBackendResult saveAsResult{ .status = EEditorWorkingCopyBackendStatus::Succeeded };
	EditorWorkingCopyBackendRevertPrepareResult revertPrepareResult{
		.result = { .status = EEditorWorkingCopyBackendStatus::Succeeded },
	};
	EEditorWorkingCopyBackendRevertApplyStatus revertApplyStatus = EEditorWorkingCopyBackendRevertApplyStatus::Applied;
	EditorWorkingCopyBackendResult closeResult{ .status = EEditorWorkingCopyBackendStatus::Succeeded };
	std::function<void(const EditorWorkingCopyBackendRequest&)> onSave;
	std::function<void(const EditorWorkingCopyBackendRequest&)> onPrepareRevert;
	std::function<void(EditorWorkingCopyRevertTransaction)> onApplyRevert;
	int saveCalls = 0;
	int saveAsCalls = 0;
	int prepareRevertCalls = 0;
	int applyRevertCalls = 0;
	int finalizeRevertCalls = 0;
	int rollbackRevertCalls = 0;
	int prepareCloseCalls = 0;
	int commitCloseCalls = 0;
	bool nativeRevertApplied = false;
	bool nativeRevertFinalized = false;
	std::uint64_t nextRevertTransaction = 1;
	std::optional<EditorWorkingCopyBackendRequest> lastSaveRequest;
	std::optional<EditorWorkingCopySaveAsBackendRequest> lastSaveAsRequest;
	std::optional<EditorWorkingCopyBackendRequest> lastPrepareRevertRequest;
	std::optional<EditorWorkingCopyRevertTransaction> lastApplyRevertTransaction;
	std::optional<EditorWorkingCopyRevertTransaction> lastFinalizeRevertTransaction;
	std::optional<EditorWorkingCopyRevertTransaction> lastRollbackRevertTransaction;
	std::optional<EditorWorkingCopyBackendRequest> lastPrepareCloseRequest;
	std::optional<EditorWorkingCopyBackendRequest> lastCommitCloseRequest;

	EditorWorkingCopyBackendResult Save(const EditorWorkingCopyBackendRequest& request) override
	{
		++saveCalls;
		lastSaveRequest = request;
		if (onSave) onSave(request);
		return saveResult;
	}
	EditorWorkingCopyBackendResult SaveAs(const EditorWorkingCopySaveAsBackendRequest& request) override
	{
		++saveAsCalls;
		lastSaveAsRequest = request;
		return saveAsResult;
	}
	EditorWorkingCopyBackendRevertPrepareResult PrepareRevert(const EditorWorkingCopyBackendRequest& request) override
	{
		++prepareRevertCalls;
		lastPrepareRevertRequest = request;
		if (onPrepareRevert) onPrepareRevert(request);
		auto result = revertPrepareResult;
		if (result.result.status == EEditorWorkingCopyBackendStatus::Succeeded && !result.transaction.IsValid()) {
			result.transaction.value = nextRevertTransaction++;
		}
		return result;
	}
	EEditorWorkingCopyBackendRevertApplyStatus ApplyPreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept override
	{
		++applyRevertCalls;
		lastApplyRevertTransaction = transaction;
		if (onApplyRevert) onApplyRevert(transaction);
		if (revertApplyStatus == EEditorWorkingCopyBackendRevertApplyStatus::Applied) nativeRevertApplied = true;
		return revertApplyStatus;
	}
	void FinalizePreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept override
	{
		++finalizeRevertCalls;
		lastFinalizeRevertTransaction = transaction;
		nativeRevertFinalized = true;
	}
	void RollbackPreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept override
	{
		++rollbackRevertCalls;
		lastRollbackRevertTransaction = transaction;
		nativeRevertApplied = false;
	}
	EditorWorkingCopyBackendResult PrepareClose(const EditorWorkingCopyBackendRequest& request) override
	{
		++prepareCloseCalls;
		lastPrepareCloseRequest = request;
		return closeResult;
	}
	void CommitClose(const EditorWorkingCopyBackendRequest& request) noexcept override
	{
		++commitCloseCalls;
		lastCommitCloseRequest = request;
	}
};

void Open(EditorCoreService& core, const EditorDocumentIdentity& identity, std::string inputId = "input")
{
	ASSERT_EQ(EEditorOperationStatus::Succeeded, core.OpenResolvedInput({
		.operation = { .operationId = "open-" + inputId },
		.input = { .inputId = std::move(inputId), .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity },
	}).status);
}

EditorDocumentSnapshot OnlyDocument(const EditorCoreService& core)
{
	const auto snapshot = core.Snapshot();
	EXPECT_EQ(1U, snapshot.documents.size());
	return snapshot.documents.empty() ? EditorDocumentSnapshot{} : snapshot.documents.front();
}

TEST(EditorWorkingCopyCoordinator, DirtySavePublishesCleanOnlyAfterBackendSuccess)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/save.txt"));

	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 3 }).status);
	backend.saveResult.successfulVersion = 3;
	const auto saved = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, saved.status);
	EXPECT_EQ(1, backend.saveCalls);
	EXPECT_FALSE(OnlyDocument(core).dirty);
	EXPECT_EQ(3U, OnlyDocument(core).documentRevision);
	ASSERT_TRUE(saved.workingCopy);
	EXPECT_EQ(EEditorWorkingCopyState::Saved, saved.workingCopy->state);
}

TEST(EditorWorkingCopyCoordinator, LateSaveCompletionCannotCleanANewerEdit)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/stale-save.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit-one" }, .inputId = "input", .version = 1 }).status);
	backend.saveResult.successfulVersion = 1;
	backend.onSave = [&coordinator](const EditorWorkingCopyBackendRequest&) {
		EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
			coordinator.SetDirty({ .operation = { .operationId = "edit-two" }, .inputId = "input", .version = 2 }).status);
	};

	const auto saved = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, saved.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::DocumentStateConflict, saved.reason);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(2U, OnlyDocument(core).documentRevision);
	ASSERT_EQ(1U, coordinator.Snapshot().workingCopies.size());
	EXPECT_EQ(EEditorWorkingCopyState::Conflict, coordinator.Snapshot().workingCopies.front().state);
}

TEST(EditorWorkingCopyCoordinator, CancelledAndFailedSaveLeaveCoreUntouched)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/cancel-save.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 4 }).status);
	const auto before = core.Snapshot();

	backend.saveResult.status = EEditorWorkingCopyBackendStatus::Cancelled;
	const auto cancelled = coordinator.Save({ .operation = { .operationId = "save-cancel" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Cancelled, cancelled.status);
	EXPECT_EQ(before.revision, core.Snapshot().revision);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(EEditorWorkingCopyState::Dirty, coordinator.Snapshot().workingCopies.front().state);

	backend.saveResult.status = EEditorWorkingCopyBackendStatus::Failed;
	const auto failed = coordinator.Save({ .operation = { .operationId = "save-fail" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed, failed.status);
	EXPECT_EQ(before.revision, core.Snapshot().revision);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(EEditorWorkingCopyState::Error, coordinator.Snapshot().workingCopies.front().state);
}

TEST(EditorWorkingCopyCoordinator, SaveAsPublishesTheBackendIdentityThroughOneAtomicCoreReplacement)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	const auto source = ResourceIdentity(L"file:///C:/workspace/source.txt");
	Open(core, source);
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 5 }).status);
	const auto target = ResourceIdentity(L"file:///C:/workspace/target.txt");
	backend.saveAsResult.successfulVersion = 5;
	backend.saveAsResult.resultingIdentity = target;
	const auto result = coordinator.SaveAs({ .operation = { .operationId = "save-as" }, .inputId = "input",
		.targetIdentity = target });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, result.status);
	EXPECT_EQ(1, backend.saveAsCalls);
	ASSERT_EQ(1U, core.Snapshot().group.inputs.size());
	EXPECT_EQ("input", core.Snapshot().group.inputs.front().descriptor.inputId);
	std::wstring currentKey;
	std::wstring targetKey;
	ASSERT_TRUE(target.TryComparisonKey(targetKey));
	ASSERT_TRUE(core.Snapshot().group.inputs.front().descriptor.documentIdentity.TryComparisonKey(currentKey));
	EXPECT_EQ(targetKey, currentKey);
	EXPECT_FALSE(OnlyDocument(core).dirty);
	EXPECT_EQ(5U, OnlyDocument(core).documentRevision);
}

TEST(EditorWorkingCopyCoordinator, OrdinarySaveCanRetargetAnUntitledInputToTheSavedResource)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	const EditorDocumentIdentity untitled{ .opaqueId = "untitled.working-copy" };
	const auto savedResource = ResourceIdentity(L"file:///C:/workspace/from-untitled.txt");
	Open(core, untitled);
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 5 }).status);
	backend.saveResult.successfulVersion = 5;
	backend.saveResult.resultingIdentity = savedResource;

	const auto result = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, result.status);
	EXPECT_EQ(1, backend.saveCalls);
	ASSERT_EQ(1U, core.Snapshot().group.inputs.size());
	std::wstring expectedKey;
	std::wstring actualKey;
	ASSERT_TRUE(savedResource.TryComparisonKey(expectedKey));
	ASSERT_TRUE(core.Snapshot().group.inputs.front().descriptor.documentIdentity.TryComparisonKey(actualKey));
	EXPECT_EQ(expectedKey, actualKey);
	EXPECT_FALSE(OnlyDocument(core).dirty);
}

TEST(EditorWorkingCopyCoordinator, SaveAsRejectsBackendIdentityAndVersionMismatchesWithoutCoreReplacement)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	const auto source = ResourceIdentity(L"file:///C:/workspace/source.txt");
	const auto target = ResourceIdentity(L"file:///C:/workspace/target.txt");
	Open(core, source);
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 5 }).status);

	backend.saveAsResult.successfulVersion = 4;
	backend.saveAsResult.resultingIdentity = target;
	const auto wrongVersion = coordinator.SaveAs({ .operation = { .operationId = "save-as-version" },
		.inputId = "input", .targetIdentity = target });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, wrongVersion.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::BackendVersionMismatch, wrongVersion.reason);

	backend.saveAsResult.successfulVersion = 5;
	backend.saveAsResult.resultingIdentity = ResourceIdentity(L"file:///C:/workspace/other.txt");
	const auto wrongIdentity = coordinator.SaveAs({ .operation = { .operationId = "save-as-identity" },
		.inputId = "input", .targetIdentity = target });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, wrongIdentity.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::BackendIdentityMismatch, wrongIdentity.reason);
	std::wstring sourceKey;
	std::wstring currentKey;
	ASSERT_TRUE(source.TryComparisonKey(sourceKey));
	ASSERT_TRUE(core.Snapshot().group.inputs.front().descriptor.documentIdentity.TryComparisonKey(currentKey));
	EXPECT_EQ(sourceKey, currentKey);
}

TEST(EditorWorkingCopyCoordinator, CleanSaveIsNotApplicableAndDoesNotInvokeTheBackend)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/clean.txt"));
	const auto result = coordinator.Save({ .operation = { .operationId = "save-clean" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::NotApplicable, result.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::NoWorkingCopyStateChange, result.reason);
	EXPECT_EQ(0, backend.saveCalls);
}

TEST(EditorWorkingCopyCoordinator, ForceWritePersistsACleanDocumentAndParticipatesInReplayIdentity)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/force-clean.txt"));
	backend.saveResult.successfulVersion = OnlyDocument(core).documentRevision;
	const auto forced = coordinator.Save({
		.operation = { .operationId = "save-clean-force" },
		.inputId = "input",
		.options = { .forceWrite = true },
	});
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, forced.status);
	EXPECT_EQ(1, backend.saveCalls);
	ASSERT_TRUE(backend.lastSaveRequest);
	EXPECT_TRUE(backend.lastSaveRequest->saveOptions.forceWrite);

	const auto differentIntent = coordinator.Save({
		.operation = { .operationId = "save-clean-force" },
		.inputId = "input",
	});
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, differentIntent.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationIdConflict, differentIntent.reason);
	EXPECT_EQ(1, backend.saveCalls);
}

TEST(EditorWorkingCopyCoordinator, SaveOptionsAreForwardedAndParticipateInReplayIdentity)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/options.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	backend.saveResult.successfulVersion = 1;
	const EditorWorkingCopySaveOptions saveOptions{
		.targetPolicy = EEditorWorkingCopySaveTargetPolicy::AcquireIfMissing,
		.suppressFeedback = true,
		.suggestedTarget = "C:/workspace/seed.txt",
		.encodingId = "utf-8",
		.lineEnding = EEditorWorkingCopyLineEnding::Lf,
	};
	const SaveWorkingCopyRequest request{ .operation = { .operationId = "save-options" }, .inputId = "input", .options = saveOptions };
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, coordinator.Save(request).status);
	ASSERT_TRUE(backend.lastSaveRequest);
	EXPECT_TRUE(backend.lastSaveRequest->saveOptions.suppressFeedback);
	EXPECT_EQ("C:/workspace/seed.txt", backend.lastSaveRequest->saveOptions.suggestedTarget);
	ASSERT_TRUE(backend.lastSaveRequest->saveOptions.encodingId);
	EXPECT_EQ("utf-8", *backend.lastSaveRequest->saveOptions.encodingId);
	EXPECT_EQ(EEditorWorkingCopyLineEnding::Lf, backend.lastSaveRequest->saveOptions.lineEnding);

	auto changedOptions = saveOptions;
	changedOptions.lineEnding = EEditorWorkingCopyLineEnding::CrLf;
	const auto conflict = coordinator.Save({ .operation = { .operationId = "save-options" }, .inputId = "input", .options = changedOptions });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationIdConflict, conflict.reason);
}

TEST(EditorWorkingCopyCoordinator, SaveAsForwardsTypedOptionsToTheBackend)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	const auto source = ResourceIdentity(L"file:///C:/workspace/save-as-options.txt");
	Open(core, source);
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	backend.saveAsResult.successfulVersion = 1;
	backend.saveAsResult.resultingIdentity = source;
	const auto result = coordinator.SaveAs({ .operation = { .operationId = "save-as-options" }, .inputId = "input",
		.options = { .suppressFeedback = true, .suggestedTarget = "C:/workspace/suggested.txt",
			.encodingId = "windows-1252", .lineEnding = EEditorWorkingCopyLineEnding::Cr } });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, result.status);
	ASSERT_TRUE(backend.lastSaveAsRequest);
	EXPECT_TRUE(backend.lastSaveAsRequest->source.saveOptions.suppressFeedback);
	EXPECT_EQ("C:/workspace/suggested.txt", backend.lastSaveAsRequest->source.saveOptions.suggestedTarget);
	ASSERT_TRUE(backend.lastSaveAsRequest->source.saveOptions.encodingId);
	EXPECT_EQ("windows-1252", *backend.lastSaveAsRequest->source.saveOptions.encodingId);
	EXPECT_EQ(EEditorWorkingCopyLineEnding::Cr, backend.lastSaveAsRequest->source.saveOptions.lineEnding);
}

TEST(EditorWorkingCopyCoordinator, InvalidSaveOptionBoundsAreTerminalBeforeBackendEffects)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/invalid-options.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	const auto overlongSuggested = coordinator.Save({ .operation = { .operationId = "long-suggestion" }, .inputId = "input",
		.options = { .suggestedTarget = std::string(4097, 'x') } });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed, overlongSuggested.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::InvalidInput, overlongSuggested.reason);
	const auto emptyEncoding = coordinator.SaveAs({ .operation = { .operationId = "empty-encoding" }, .inputId = "input",
		.options = { .encodingId = std::string{} } });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed, emptyEncoding.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::InvalidInput, emptyEncoding.reason);
	EXPECT_EQ(0, backend.saveCalls);
	EXPECT_EQ(0, backend.saveAsCalls);
}

TEST(EditorWorkingCopyCoordinator, ExistingOnlySaveOfAnUntitledInputDoesNotInvokeTheBackend)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, EditorDocumentIdentity{ .opaqueId = "untitled.save-existing-only" });
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	const auto result = coordinator.Save({ .operation = { .operationId = "save-existing-only" }, .inputId = "input",
		.options = { .targetPolicy = EEditorWorkingCopySaveTargetPolicy::ExistingOnly } });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::NotApplicable, result.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::NoWorkingCopyStateChange, result.reason);
	EXPECT_EQ(0, backend.saveCalls);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	const auto replay = coordinator.Save({ .operation = { .operationId = "save-existing-only" }, .inputId = "input",
		.options = { .targetPolicy = EEditorWorkingCopySaveTargetPolicy::ExistingOnly } });
	EXPECT_TRUE(replay.replayed);
}

TEST(EditorWorkingCopyCoordinator, RevertSuccessFinalizesTheNativeTransactionOnlyAfterCoreCommit)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/revert.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 6 }).status);
	backend.revertPrepareResult.result.successfulVersion = 7;
	const auto succeeded = coordinator.Revert({ .operation = { .operationId = "revert" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, succeeded.status);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	EXPECT_EQ(1, backend.applyRevertCalls);
	EXPECT_EQ(1, backend.finalizeRevertCalls);
	EXPECT_EQ(0, backend.rollbackRevertCalls);
	EXPECT_TRUE(backend.nativeRevertApplied);
	EXPECT_TRUE(backend.nativeRevertFinalized);
	EXPECT_FALSE(OnlyDocument(core).dirty);
	EXPECT_EQ(7U, OnlyDocument(core).documentRevision);

	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit-again" }, .inputId = "input", .version = 8 }).status);
	backend.revertPrepareResult.result.successfulVersion = 7;
	const auto rewind = coordinator.Revert({ .operation = { .operationId = "revert-rewind" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, rewind.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::BackendVersionMismatch, rewind.reason);
	EXPECT_EQ(2, backend.prepareRevertCalls);
	EXPECT_EQ(1, backend.applyRevertCalls);
	EXPECT_EQ(1, backend.finalizeRevertCalls);
	EXPECT_EQ(1, backend.rollbackRevertCalls);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(8U, OnlyDocument(core).documentRevision);
}

TEST(EditorWorkingCopyCoordinator, RevertPrepareFailureLeavesCoreAndNativeUntouchedWithoutATerminalTransaction)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/revert-prepare-failure.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 3 }).status);
	const auto before = core.Snapshot();
	backend.revertPrepareResult.result.status = EEditorWorkingCopyBackendStatus::Failed;

	const auto failed = coordinator.Revert({ .operation = { .operationId = "revert-prepare-fail" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed, failed.status);
	EXPECT_EQ(before.revision, core.Snapshot().revision);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	EXPECT_EQ(0, backend.applyRevertCalls);
	EXPECT_EQ(0, backend.finalizeRevertCalls);
	EXPECT_EQ(0, backend.rollbackRevertCalls);
	EXPECT_FALSE(backend.nativeRevertApplied);
}

TEST(EditorWorkingCopyCoordinator, RevertNativeApplyFailureDiscardsTheStageWithoutChangingCoreOrNative)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/revert-apply-failure.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 3 }).status);
	const auto before = core.Snapshot();
	backend.revertPrepareResult.result.successfulVersion = 4;
	backend.revertApplyStatus = EEditorWorkingCopyBackendRevertApplyStatus::TargetChanged;

	const auto failed = coordinator.Revert({ .operation = { .operationId = "revert-apply-fail" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed, failed.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::BackendApplyFailed, failed.reason);
	EXPECT_EQ(before.revision, core.Snapshot().revision);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	EXPECT_EQ(1, backend.applyRevertCalls);
	EXPECT_EQ(0, backend.finalizeRevertCalls);
	EXPECT_EQ(1, backend.rollbackRevertCalls);
	EXPECT_FALSE(backend.nativeRevertApplied);
}

TEST(EditorWorkingCopyCoordinator, RevertCoreStaleConflictRollsBackTheExactAppliedNativeTransaction)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/revert-core-conflict.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 3 }).status);
	backend.revertPrepareResult.result.successfulVersion = 4;
	backend.onApplyRevert = [&coordinator](EditorWorkingCopyRevertTransaction) {
		EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
			coordinator.SetDirty({ .operation = { .operationId = "edit-during-revert" }, .inputId = "input", .version = 5 }).status);
	};

	const auto conflict = coordinator.Revert({ .operation = { .operationId = "revert-core-conflict" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::RevisionConflict, conflict.reason);
	EXPECT_TRUE(OnlyDocument(core).dirty);
	EXPECT_EQ(5U, OnlyDocument(core).documentRevision);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	EXPECT_EQ(1, backend.applyRevertCalls);
	EXPECT_EQ(0, backend.finalizeRevertCalls);
	EXPECT_EQ(1, backend.rollbackRevertCalls);
	EXPECT_FALSE(backend.nativeRevertApplied);
	EXPECT_EQ(EEditorWorkingCopyState::Conflict, coordinator.Snapshot().workingCopies.front().state);
}

TEST(EditorWorkingCopyCoordinator, RevertPreparationCannotReenterAndTheOwningOperationPublishesTheOnlyReplay)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/revert-reentrant.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 3 }).status);
	backend.revertPrepareResult.result.successfulVersion = 4;
	EditorWorkingCopyOperationResult reentrant;
	backend.onPrepareRevert = [&coordinator, &reentrant](const EditorWorkingCopyBackendRequest&) {
		reentrant = coordinator.Revert({ .operation = { .operationId = "revert" }, .inputId = "input" });
	};

	const auto succeeded = coordinator.Revert({ .operation = { .operationId = "revert" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, succeeded.status);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, reentrant.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationInProgress, reentrant.reason);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	const auto replay = coordinator.Revert({ .operation = { .operationId = "revert" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(1, backend.prepareRevertCalls);
	EXPECT_EQ(1, backend.finalizeRevertCalls);
	EXPECT_EQ(0, backend.rollbackRevertCalls);
}

TEST(EditorWorkingCopyCoordinator, EffectfulBackendCallbacksCannotReenterTheSameWorkingCopy)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/reentrant.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	backend.saveResult.successfulVersion = 1;
	EditorWorkingCopyOperationResult reentrant;
	backend.onSave = [&coordinator, &reentrant](const EditorWorkingCopyBackendRequest&) {
		reentrant = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	};

	const auto saved = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, saved.status);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, reentrant.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationInProgress, reentrant.reason);
	EXPECT_EQ(1, backend.saveCalls);
	EXPECT_FALSE(OnlyDocument(core).dirty);
	const auto replay = coordinator.Save({ .operation = { .operationId = "save" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(1, backend.saveCalls);
}

TEST(EditorWorkingCopyCoordinator, ClosePreparationIsAtomicAndLastCloseProducesTheLegitimateZeroInputState)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/close.txt"));
	backend.closeResult.status = EEditorWorkingCopyBackendStatus::Failed;
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed,
		coordinator.Close({ .operation = { .operationId = "close-fail" }, .inputId = "input" }).status);
	EXPECT_EQ(EEditorWorkingCopyState::Error, coordinator.Snapshot().workingCopies.front().state);
	backend.closeResult.status = EEditorWorkingCopyBackendStatus::Cancelled;
	const auto cancelled = coordinator.Close({ .operation = { .operationId = "close-cancel" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Cancelled, cancelled.status);
	EXPECT_EQ(1U, core.Snapshot().group.inputs.size());
	EXPECT_EQ(0, backend.commitCloseCalls);
	EXPECT_EQ(EEditorWorkingCopyState::Saved, coordinator.Snapshot().workingCopies.front().state);

	backend.closeResult.status = EEditorWorkingCopyBackendStatus::Succeeded;
	const auto closed = coordinator.Close({ .operation = { .operationId = "close" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, closed.status);
	EXPECT_TRUE(core.Snapshot().group.inputs.empty());
	EXPECT_FALSE(core.Snapshot().group.activeInputId);
	EXPECT_EQ(1, backend.commitCloseCalls);
}

TEST(EditorWorkingCopyCoordinator, CloseDispositionIsForwardedAndPartOfTheReplayFingerprint)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	Open(core, ResourceIdentity(L"file:///C:/workspace/window-close.txt"));

	const CloseWorkingCopyRequest closeWindow{ .operation = { .operationId = "close-window" }, .inputId = "input",
		.disposition = EEditorWorkingCopyCloseDisposition::DisposeWindow };
	const auto closed = coordinator.Close(closeWindow);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, closed.status);
	ASSERT_TRUE(backend.lastPrepareCloseRequest);
	ASSERT_TRUE(backend.lastCommitCloseRequest);
	EXPECT_EQ(EEditorWorkingCopyCloseDisposition::DisposeWindow, backend.lastPrepareCloseRequest->closeDisposition);
	EXPECT_EQ(EEditorWorkingCopyCloseDisposition::DisposeWindow, backend.lastCommitCloseRequest->closeDisposition);

	const auto conflictingReplay = coordinator.Close({ .operation = { .operationId = "close-window" }, .inputId = "input",
		.disposition = EEditorWorkingCopyCloseDisposition::InitializeEmptyDocument });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, conflictingReplay.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationIdConflict, conflictingReplay.reason);
}

TEST(EditorWorkingCopyCoordinator, ReplaysExactOperationsAndConflictsOnOperationIdReuse)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend, 2);
	Open(core, ResourceIdentity(L"file:///C:/workspace/replay.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "edit" }, .inputId = "input", .version = 1 }).status);
	backend.saveResult.successfulVersion = 1;
	const SaveWorkingCopyRequest request{ .operation = { .operationId = "same" }, .inputId = "input" };
	const auto first = coordinator.Save(request);
	const auto replay = coordinator.Save(request);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Succeeded, first.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(1, backend.saveCalls);
	const auto conflict = coordinator.Revert({ .operation = { .operationId = "same" }, .inputId = "input" });
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EEditorWorkingCopyOperationReason::OperationIdConflict, conflict.reason);
}

TEST(EditorWorkingCopyCoordinator, EveryPublicFailureBranchIsTerminalWithoutAVisibleEditor)
{
	EditorCoreService core;
	FakeWorkingCopyBackend backend;
	EditorWorkingCopyCoordinator coordinator(core, backend);
	EXPECT_TRUE(coordinator.Snapshot().workingCopies.empty());
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Failed,
		coordinator.Save({ .operation = { .operationId = "" }, .inputId = "input" }).status);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::NotApplicable,
		coordinator.Save({ .operation = { .operationId = "missing" }, .inputId = "input" }).status);

	Open(core, ResourceIdentity(L"file:///C:/workspace/terminal.txt"));
	ASSERT_EQ(EEditorWorkingCopyOperationStatus::Succeeded,
		coordinator.SetDirty({ .operation = { .operationId = "terminal-edit" }, .inputId = "input", .version = 1 }).status);
	backend.saveResult.status = EEditorWorkingCopyBackendStatus::Unsupported;
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Unsupported,
		coordinator.Save({ .operation = { .operationId = "unsupported" }, .inputId = "input" }).status);
	EXPECT_EQ(EEditorWorkingCopyOperationStatus::Conflict,
		coordinator.Save({ .operation = { .operationId = "stale", .expectedModelRevision = 0 }, .inputId = "input" }).status);
}

} // namespace
} // namespace workbench::editor
