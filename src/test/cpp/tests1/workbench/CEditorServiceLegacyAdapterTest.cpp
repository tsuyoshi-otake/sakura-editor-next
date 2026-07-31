/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <utility>
#include <vector>

#include "workbench/editor/CEditorServiceLegacyAdapter.h"
#include "workbench/editor/EditorCommandIds.h"

namespace workbench::editor {
namespace {

EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
{
	auto uri = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(uri);
	return { .resource = std::move(*uri.value) };
}

class FakeLegacyEditorBackend final : public ILegacyEditorBackend {
public:
	std::optional<ResolvedEditorDocument> current;
	bool throwOnRead = false;
	mutable std::size_t readCount = 0;

	std::optional<ResolvedEditorDocument> TryGetCurrentDocument() const override
	{
		++readCount;
		if (throwOnRead) {
			throw std::runtime_error("legacy read failed");
		}
		return current;
	}
};

TEST(CEditorServiceLegacyAdapter, NoLegacyDocumentLeavesCoreAtLegitimateEmptyState)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	CEditorServiceLegacyAdapter adapter(core, legacy);

	const auto result = adapter.ResolveCurrentDocument({ .operationId = "resolve-empty" });
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, result.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, result.reason);
	EXPECT_EQ(0U, result.revision);
	EXPECT_EQ(1U, legacy.readCount);

	const auto snapshot = adapter.Snapshot();
	EXPECT_TRUE(snapshot.group.inputs.empty());
	EXPECT_FALSE(snapshot.group.activeInputId);
	EXPECT_TRUE(snapshot.documents.empty());
}

TEST(CEditorServiceLegacyAdapter, ResolvesLegacyDocumentWithoutInventingAVisibleInput)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	legacy.current = ResolvedEditorDocument{
		.identity = ResourceIdentity(L"file:///C:/workspace/adopted.txt"),
		.documentRevision = 7,
		.dirty = true,
	};
	CEditorServiceLegacyAdapter adapter(core, legacy);

	const auto result = adapter.ResolveCurrentDocument({ .operationId = "resolve-current", .expectedModelRevision = 0 });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, result.status);
	const auto snapshot = adapter.Snapshot();
	EXPECT_TRUE(snapshot.group.inputs.empty());
	EXPECT_FALSE(snapshot.group.activeInputId);
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(0U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ(1U, snapshot.documents[0].resolverReferenceCount);
	EXPECT_EQ(7U, snapshot.documents[0].documentRevision);
	EXPECT_TRUE(snapshot.documents[0].dirty);
}

TEST(CEditorServiceLegacyAdapter, AdoptsPreparedLegacyDocumentInOneVisibleActiveCommit)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	legacy.current = ResolvedEditorDocument{
		.identity = ResourceIdentity(L"file:///C:/workspace/adopted-visible.txt"),
		.documentRevision = 7,
		.dirty = true,
	};
	CEditorServiceLegacyAdapter adapter(core, legacy);
	std::vector<EditorCoreChangeBatch> batches;
	auto subscription = adapter.Subscribe([&batches](const EditorCoreChangeBatch& batch) {
		batches.push_back(batch);
	});

	const auto result = adapter.AdoptCurrentDocument(
		{ .operationId = "adopt-current", .expectedModelRevision = 0 }, "legacy-visible-input");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, result.status);
	EXPECT_EQ(1U, result.revision);
	EXPECT_EQ(1U, legacy.readCount);
	ASSERT_TRUE(result.changeBatch);
	EXPECT_EQ(1U, result.changeBatch->revision);
	ASSERT_EQ(3U, result.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, result.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputOpened, result.changeBatch->changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::ActiveInputChanged, result.changeBatch->changes[2].kind);

	const auto snapshot = adapter.Snapshot();
	ASSERT_EQ(1U, snapshot.documents.size());
	ASSERT_EQ(1U, snapshot.group.inputs.size());
	ASSERT_TRUE(snapshot.group.activeInputId);
	EXPECT_EQ("legacy-visible-input", *snapshot.group.activeInputId);
	ASSERT_EQ(1U, batches.size());
	EXPECT_EQ(1U, batches[0].revision);
}

TEST(CEditorServiceLegacyAdapter, AdoptReplaysOnlyTheCoreOperationAndPreservesItsRevision)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	legacy.current = ResolvedEditorDocument{ .identity = ResourceIdentity(L"file:///C:/workspace/replay-adopt.txt") };
	CEditorServiceLegacyAdapter adapter(core, legacy);
	const EditorOperationMetadata operation{ .operationId = "same-adopt", .expectedModelRevision = 0 };

	const auto first = adapter.AdoptCurrentDocument(operation, "legacy-visible-input");
	const auto replay = adapter.AdoptCurrentDocument(operation, "legacy-visible-input");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, first.status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, replay.status);
	EXPECT_FALSE(first.replayed);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(first.revision, replay.revision);
	EXPECT_EQ(2U, legacy.readCount);
	EXPECT_EQ(1U, adapter.Snapshot().revision);
}

TEST(CEditorServiceLegacyAdapter, AdoptBackendFailureAndInvalidInputLeaveCoreEmpty)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	CEditorServiceLegacyAdapter adapter(core, legacy);

	const auto unresolved = adapter.AdoptCurrentDocument({ .operationId = "empty-adopt" }, "legacy-visible-input");
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, unresolved.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, unresolved.reason);
	EXPECT_EQ(0U, unresolved.revision);

	legacy.current = ResolvedEditorDocument{ .identity = ResourceIdentity(L"file:///C:/workspace/invalid-input.txt") };
	const auto invalid = adapter.AdoptCurrentDocument({ .operationId = "invalid-input" }, "");
	EXPECT_EQ(EEditorOperationStatus::Failed, invalid.status);
	EXPECT_EQ(EEditorOperationReason::InvalidInput, invalid.reason);
	EXPECT_EQ(0U, adapter.Snapshot().revision);

	legacy.throwOnRead = true;
	const auto failed = adapter.AdoptCurrentDocument({ .operationId = "failing-adopt" }, "legacy-visible-input");
	EXPECT_EQ(EEditorOperationStatus::Failed, failed.status);
	EXPECT_EQ(EEditorOperationReason::LegacyBackendFailure, failed.reason);
	EXPECT_EQ(0U, adapter.Snapshot().revision);
}

TEST(CEditorServiceLegacyAdapter, ReplacesPreparedLegacyDocumentInOneCommitWhilePreservingInputPositionAndActiveSelection)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	CEditorServiceLegacyAdapter adapter(core, legacy);
	const auto source = ResourceIdentity(L"file:///C:/workspace/replace-source.txt");
	const auto neighbor = ResourceIdentity(L"file:///C:/workspace/replace-neighbor.txt");
	const auto saved = ResourceIdentity(L"file:///C:/workspace/replace-saved.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, adapter.OpenResolvedInput({
		.operation = { .operationId = "open-source" },
		.input = { .inputId = "source", .documentIdentity = source },
		.resolvedDocument = ResolvedEditorDocument{ .identity = source },
	}).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, adapter.OpenResolvedInput({
		.operation = { .operationId = "open-neighbor" },
		.input = { .inputId = "neighbor", .documentIdentity = neighbor },
		.resolvedDocument = ResolvedEditorDocument{ .identity = neighbor },
	}).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, adapter.ShowInput({
		.operation = { .operationId = "show-source" }, .inputId = "source",
	}).status);
	legacy.current = ResolvedEditorDocument{ .identity = saved, .documentRevision = 9, .dirty = true };
	std::vector<EditorCoreChangeBatch> batches;
	auto subscription = adapter.Subscribe([&batches](const EditorCoreChangeBatch& batch) { batches.push_back(batch); });

	const auto result = adapter.ReplaceInputDocumentWithCurrent(
		{ .operationId = "replace-current", .expectedModelRevision = 3 }, "source");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, result.status);
	ASSERT_TRUE(result.changeBatch);
	ASSERT_EQ(3U, result.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, result.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputDocumentReplaced, result.changeBatch->changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, result.changeBatch->changes[2].kind);
	ASSERT_EQ(1U, batches.size());
	EXPECT_EQ(result.revision, batches[0].revision);

	const auto snapshot = adapter.Snapshot();
	ASSERT_EQ(2U, snapshot.group.inputs.size());
	EXPECT_EQ("source", snapshot.group.inputs[0].descriptor.inputId);
	EXPECT_EQ("neighbor", snapshot.group.inputs[1].descriptor.inputId);
	ASSERT_TRUE(snapshot.group.activeInputId);
	EXPECT_EQ("source", *snapshot.group.activeInputId);
	ASSERT_TRUE(snapshot.group.inputs[0].descriptor.documentIdentity.resource);
	EXPECT_EQ(L"file:///C:/workspace/replace-saved.txt", snapshot.group.inputs[0].descriptor.documentIdentity.resource->ToString());
	EXPECT_EQ(4U, snapshot.revision);
}

TEST(CEditorServiceLegacyAdapter, ReplaceCurrentUsesTheCoreValidationAndReplayContract)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	CEditorServiceLegacyAdapter adapter(core, legacy);
	const auto source = ResourceIdentity(L"file:///C:/workspace/replace-contract-source.txt");
	const auto target = ResourceIdentity(L"file:///C:/workspace/replace-contract-target.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, adapter.OpenResolvedInput({
		.operation = { .operationId = "open-source" },
		.input = { .inputId = "source", .documentIdentity = source },
		.resolvedDocument = ResolvedEditorDocument{ .identity = source },
	}).status);

	const auto noCandidate = adapter.ReplaceInputDocumentWithCurrent({ .operationId = "no-candidate" }, "source");
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, noCandidate.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, noCandidate.reason);

	legacy.throwOnRead = true;
	const auto readFailure = adapter.ReplaceInputDocumentWithCurrent({ .operationId = "read-failure" }, "source");
	EXPECT_EQ(EEditorOperationStatus::Failed, readFailure.status);
	EXPECT_EQ(EEditorOperationReason::LegacyBackendFailure, readFailure.reason);
	legacy.throwOnRead = false;
	legacy.current = ResolvedEditorDocument{ .identity = target };

	const auto invalid = adapter.ReplaceInputDocumentWithCurrent({ .operationId = "invalid" }, "");
	EXPECT_EQ(EEditorOperationStatus::Failed, invalid.status);
	EXPECT_EQ(EEditorOperationReason::InvalidInput, invalid.reason);
	const auto stale = adapter.ReplaceInputDocumentWithCurrent(
		{ .operationId = "stale", .expectedModelRevision = 0 }, "source");
	EXPECT_EQ(EEditorOperationStatus::Failed, stale.status);
	EXPECT_EQ(EEditorOperationReason::RevisionConflict, stale.reason);

	const EditorOperationMetadata operation{ .operationId = "replace", .expectedModelRevision = 1 };
	const auto first = adapter.ReplaceInputDocumentWithCurrent(operation, "source");
	const auto replay = adapter.ReplaceInputDocumentWithCurrent(operation, "source");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, first.status);
	EXPECT_FALSE(first.replayed);
	EXPECT_EQ(EEditorOperationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(first.revision, replay.revision);

	legacy.current = ResolvedEditorDocument{ .identity = target };
	const auto same = adapter.ReplaceInputDocumentWithCurrent({ .operationId = "same", .expectedModelRevision = 2 }, "source");
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, same.status);
	EXPECT_EQ(EEditorOperationReason::NoDocumentStateChange, same.reason);
}

TEST(CEditorServiceLegacyAdapter, CoreDelegatesOpenShowCloseAndReturnToEmptyTransactionally)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	CEditorServiceLegacyAdapter adapter(core, legacy);
	const auto identity = ResourceIdentity(L"file:///C:/workspace/delegated.txt");

	const auto open = adapter.OpenResolvedInput({
		.operation = { .operationId = "open", .expectedModelRevision = 0 },
		.input = { .inputId = "legacy-input", .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity },
	});
	ASSERT_EQ(EEditorOperationStatus::Succeeded, open.status);
	EXPECT_EQ("legacy-input", *adapter.Snapshot().group.activeInputId);

	const auto show = adapter.ShowInput({
		.operation = { .operationId = "show", .expectedModelRevision = 1 },
		.inputId = "legacy-input",
	});
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, show.status);
	EXPECT_EQ(EEditorOperationReason::AlreadyActive, show.reason);

	const auto close = adapter.CloseInput({
		.operation = { .operationId = "close", .expectedModelRevision = 1 },
		.inputId = "legacy-input",
	});
	ASSERT_EQ(EEditorOperationStatus::Succeeded, close.status);
	const auto empty = adapter.Snapshot();
	EXPECT_TRUE(empty.group.inputs.empty());
	EXPECT_FALSE(empty.group.activeInputId);
	EXPECT_TRUE(empty.documents.empty());
}

TEST(CEditorServiceLegacyAdapter, BackendFailureAndCoreConflictPreserveTheStableModel)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	legacy.throwOnRead = true;
	CEditorServiceLegacyAdapter adapter(core, legacy);

	const auto backendFailure = adapter.ResolveCurrentDocument({ .operationId = "backend-failure" });
	EXPECT_EQ(EEditorOperationStatus::Failed, backendFailure.status);
	EXPECT_EQ(EEditorOperationReason::LegacyBackendFailure, backendFailure.reason);
	EXPECT_EQ(0U, adapter.Snapshot().revision);

	legacy.throwOnRead = false;
	legacy.current = ResolvedEditorDocument{ .identity = ResourceIdentity(L"file:///C:/workspace/stable.txt") };
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		adapter.ResolveCurrentDocument({ .operationId = "resolve" }).status);
	const auto stable = adapter.Snapshot();

	const auto stale = adapter.OpenResolvedInput({
		.operation = { .operationId = "stale-open", .expectedModelRevision = 0 },
		.input = { .inputId = "legacy-input", .documentIdentity = legacy.current->identity },
		.resolvedDocument = std::nullopt,
	});
	EXPECT_EQ(EEditorOperationStatus::Failed, stale.status);
	EXPECT_EQ(EEditorOperationReason::RevisionConflict, stale.reason);
	const auto after = adapter.Snapshot();
	EXPECT_EQ(stable.revision, after.revision);
	EXPECT_EQ(stable.group.inputs.size(), after.group.inputs.size());
	EXPECT_EQ(stable.documents.size(), after.documents.size());
}

TEST(CEditorServiceLegacyAdapter, SameResolveRequestUsesTheCoreReplayContract)
{
	EditorCoreService core;
	FakeLegacyEditorBackend legacy;
	legacy.current = ResolvedEditorDocument{ .identity = ResourceIdentity(L"file:///C:/workspace/replay-adapter.txt") };
	CEditorServiceLegacyAdapter adapter(core, legacy);
	const EditorOperationMetadata operation{ .operationId = "same-resolve", .expectedModelRevision = 0 };

	const auto first = adapter.ResolveCurrentDocument(operation);
	const auto replay = adapter.ResolveCurrentDocument(operation);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, first.status);
	EXPECT_EQ(EEditorOperationStatus::Succeeded, replay.status);
	EXPECT_FALSE(first.replayed);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(first.revision, replay.revision);
	EXPECT_EQ(1U, adapter.Snapshot().revision);
	EXPECT_EQ(2U, legacy.readCount);
}

TEST(CEditorServiceLegacyAdapter, UsesStableVsCodeWorkbenchCommandIds)
{
	EXPECT_EQ("workbench.action.files.newUntitledFile", command_ids::NewUntitledFile);
	EXPECT_EQ("workbench.action.files.openFile", command_ids::OpenFile);
	EXPECT_EQ("workbench.action.files.save", command_ids::Save);
	EXPECT_EQ("workbench.action.files.saveAs", command_ids::SaveAs);
	EXPECT_EQ("workbench.action.files.revert", command_ids::Revert);
	EXPECT_EQ("workbench.action.closeActiveEditor", command_ids::CloseActiveEditor);
}

} // namespace
} // namespace workbench::editor
