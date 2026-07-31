/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "workbench/editor/EditorCoreService.h"

namespace workbench::editor {
namespace {

EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
{
	auto uri = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(uri);
	return { .resource = std::move(*uri.value) };
}

OpenResolvedInputRequest OpenRequest(std::string operationId, std::string inputId,
	const EditorDocumentIdentity& identity, std::optional<std::uint64_t> expectedRevision = std::nullopt,
	bool activate = true)
{
	return {
		.operation = { .operationId = std::move(operationId), .expectedModelRevision = expectedRevision },
		.input = { .inputId = std::move(inputId), .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity },
		.activate = activate,
	};
}

ResolveDocumentRequest ResolveRequest(std::string operationId, const EditorDocumentIdentity& identity,
	std::uint64_t documentRevision = 0, bool dirty = false)
{
	return {
		.operation = { .operationId = std::move(operationId) },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity, .documentRevision = documentRevision, .dirty = dirty },
	};
}

ReplaceInputDocumentRequest ReplaceRequest(std::string operationId, std::string inputId,
	const EditorDocumentIdentity& identity, std::uint64_t documentRevision = 0, bool dirty = false,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return {
		.operation = { .operationId = std::move(operationId), .expectedModelRevision = expectedRevision },
		.inputId = std::move(inputId),
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity, .documentRevision = documentRevision, .dirty = dirty },
	};
}

TEST(EditorCoreService, StartsWithOneEmptyGroupAndNoActiveInput)
{
	EditorCoreService service;
	const auto snapshot = service.Snapshot();

	EXPECT_EQ(1U, snapshot.generation);
	EXPECT_EQ(0U, snapshot.revision);
	EXPECT_TRUE(snapshot.group.inputs.empty());
	EXPECT_FALSE(snapshot.group.activeInputId);
	EXPECT_TRUE(snapshot.documents.empty());
}

TEST(EditorCoreService, OpensAResolvedInputInactiveIntoAnEmptyGroupWithoutClaimingAnActiveSelection)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/inactive-empty.txt");

	const auto opened = service.OpenResolvedInput(OpenRequest("open-inactive", "inactive", identity, 0, false));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, opened.status);
	ASSERT_TRUE(opened.changeBatch);
	ASSERT_EQ(2U, opened.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, opened.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputOpened, opened.changeBatch->changes[1].kind);
	EXPECT_TRUE(std::none_of(opened.changeBatch->changes.begin(), opened.changeBatch->changes.end(), [](const EditorCoreChange& change) {
		return change.kind == EEditorCoreChangeKind::ActiveInputChanged;
	}));

	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.group.inputs.size());
	EXPECT_EQ("inactive", snapshot.group.inputs[0].descriptor.inputId);
	EXPECT_FALSE(snapshot.group.activeInputId);
}

TEST(EditorCoreService, InactiveOpenRetainsAnExistingActiveInput)
{
	EditorCoreService service;
	const auto activeIdentity = ResourceIdentity(L"file:///C:/workspace/active.txt");
	const auto inactiveIdentity = ResourceIdentity(L"file:///C:/workspace/inactive.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.OpenResolvedInput(OpenRequest("open-active", "active", activeIdentity)).status);

	const auto opened = service.OpenResolvedInput(OpenRequest("open-inactive", "inactive", inactiveIdentity, 1, false));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, opened.status);
	ASSERT_TRUE(opened.changeBatch);
	ASSERT_EQ(2U, opened.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, opened.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputOpened, opened.changeBatch->changes[1].kind);
	EXPECT_TRUE(std::none_of(opened.changeBatch->changes.begin(), opened.changeBatch->changes.end(), [](const EditorCoreChange& change) {
		return change.kind == EEditorCoreChangeKind::ActiveInputChanged;
	}));

	const auto snapshot = service.Snapshot();
	ASSERT_TRUE(snapshot.group.activeInputId);
	EXPECT_EQ("active", *snapshot.group.activeInputId);
	ASSERT_EQ(2U, snapshot.group.inputs.size());
	EXPECT_EQ("inactive", snapshot.group.inputs[1].descriptor.inputId);

	const auto duplicate = service.OpenResolvedInput(OpenRequest("open-inactive-duplicate", "inactive", inactiveIdentity, 2, false));
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, duplicate.status);
	EXPECT_EQ(EEditorOperationReason::InputAlreadyOpen, duplicate.reason);
	EXPECT_EQ(2U, service.Snapshot().revision);
	ASSERT_TRUE(service.Snapshot().group.activeInputId);
	EXPECT_EQ("active", *service.Snapshot().group.activeInputId);
}

TEST(EditorCoreService, InactiveOpenReplaysExactlyAndConflictsWhenActivationIntentChanges)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/inactive-replay.txt");
	const auto inactiveRequest = OpenRequest("open", "input", identity, 0, false);

	const auto first = service.OpenResolvedInput(inactiveRequest);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, first.status);
	EXPECT_FALSE(service.Snapshot().group.activeInputId);
	const auto replay = service.OpenResolvedInput(inactiveRequest);
	EXPECT_EQ(EEditorOperationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(first.revision, replay.revision);
	EXPECT_EQ(1U, service.Snapshot().revision);

	const auto conflict = service.OpenResolvedInput(OpenRequest("open", "input", identity, 0, true));
	EXPECT_EQ(EEditorOperationStatus::Failed, conflict.status);
	EXPECT_EQ(EEditorOperationReason::OperationIdConflict, conflict.reason);
	EXPECT_FALSE(service.Snapshot().group.activeInputId);
}

TEST(EditorCoreService, OpensShowsAndClosesBackToTheLegitimateEmptyState)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/note.txt");

	const auto open = service.OpenResolvedInput(OpenRequest("open", "input-1", identity, 0));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, open.status);
	EXPECT_EQ(1U, open.revision);
	ASSERT_TRUE(open.changeBatch);
	ASSERT_EQ(3U, open.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, open.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputOpened, open.changeBatch->changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::ActiveInputChanged, open.changeBatch->changes[2].kind);

	const auto alreadyShown = service.ShowInput({ .operation = { .operationId = "show", .expectedModelRevision = 1 }, .inputId = "input-1" });
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, alreadyShown.status);
	EXPECT_EQ(EEditorOperationReason::AlreadyActive, alreadyShown.reason);

	const auto close = service.CloseInput({ .operation = { .operationId = "close", .expectedModelRevision = 1 }, .inputId = "input-1" });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, close.status);
	ASSERT_TRUE(close.changeBatch);
	ASSERT_EQ(3U, close.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::ActiveInputChanged, close.changeBatch->changes[0].kind);
	EXPECT_FALSE(close.changeBatch->changes[0].activeInputId);
	EXPECT_EQ(EEditorCoreChangeKind::InputClosed, close.changeBatch->changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, close.changeBatch->changes[2].kind);

	const auto snapshot = service.Snapshot();
	EXPECT_EQ(2U, snapshot.revision);
	EXPECT_TRUE(snapshot.group.inputs.empty());
	EXPECT_FALSE(snapshot.group.activeInputId);
	EXPECT_TRUE(snapshot.documents.empty());
}

TEST(EditorCoreService, ResolvesDocumentsWithoutInputsAndDisposesOnlyAfterResolverAndInputReferencesRelease)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/resolved-only.txt");
	const auto resolved = service.ResolveDocument(ResolveRequest("resolve", identity, 4, true));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, resolved.status);
	ASSERT_TRUE(resolved.changeBatch);
	ASSERT_EQ(2U, resolved.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, resolved.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentResolved, resolved.changeBatch->changes[1].kind);

	auto snapshot = service.Snapshot();
	EXPECT_TRUE(snapshot.group.inputs.empty());
	EXPECT_FALSE(snapshot.group.activeInputId);
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(0U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ(1U, snapshot.documents[0].resolverReferenceCount);

	const auto open = service.OpenResolvedInput(OpenRequest("open", "input", identity));
	// A supplied resolved state must match the already authoritative document state.
	EXPECT_EQ(EEditorOperationStatus::Failed, open.status);
	EXPECT_EQ(EEditorOperationReason::DocumentStateConflict, open.reason);
	const auto matchingOpen = service.OpenResolvedInput({
		.operation = { .operationId = "open-matching" },
		.input = { .inputId = "input", .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity, .documentRevision = 4, .dirty = true },
	});
	ASSERT_EQ(EEditorOperationStatus::Succeeded, matchingOpen.status);

	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.CloseInput({ .operation = { .operationId = "close" }, .inputId = "input" }).status);
	snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(0U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ(1U, snapshot.documents[0].resolverReferenceCount);

	const auto release = service.ReleaseDocument({ .operation = { .operationId = "release" }, .identity = identity });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, release.status);
	ASSERT_TRUE(release.changeBatch);
	ASSERT_EQ(2U, release.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentResolverReleased, release.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, release.changeBatch->changes[1].kind);
	EXPECT_TRUE(service.Snapshot().documents.empty());
}

TEST(EditorCoreService, OpensPreviouslyResolvedDocumentWithoutRepeatingResolverPayload)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/resolved-only-open.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.ResolveDocument(ResolveRequest("resolve", identity, 4, true)).status);

	const auto open = service.OpenResolvedInput({
		.operation = { .operationId = "open-existing" },
		.input = { .inputId = "input", .documentIdentity = identity },
		.resolvedDocument = std::nullopt,
	});
	ASSERT_EQ(EEditorOperationStatus::Succeeded, open.status);
	EXPECT_EQ(2U, service.Snapshot().revision);
	ASSERT_EQ(1U, service.Snapshot().documents.size());
	EXPECT_EQ(1U, service.Snapshot().documents[0].inputReferenceCount);
	EXPECT_EQ(1U, service.Snapshot().documents[0].resolverReferenceCount);

	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.CloseInput({ .operation = { .operationId = "close" }, .inputId = "input" }).status);
	auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(0U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ(1U, snapshot.documents[0].resolverReferenceCount);

	const auto release = service.ReleaseDocument({ .operation = { .operationId = "release" }, .identity = identity });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, release.status);
	EXPECT_TRUE(service.Snapshot().documents.empty());
}

TEST(EditorCoreService, SharesOneResolvedDocumentForEquivalentUrisUntilLastInputCloses)
{
	EditorCoreService service;
	const auto upper = ResourceIdentity(L"file:///C:/Workspace/ReadMe.md");
	const auto lower = ResourceIdentity(L"FILE:///c:/workspace/readme.md");

	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-1", "one", upper)).status);
	const auto second = service.OpenResolvedInput(OpenRequest("open-2", "two", lower));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, second.status);
	ASSERT_TRUE(second.changeBatch);
	ASSERT_EQ(2U, second.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::InputOpened, second.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::ActiveInputChanged, second.changeBatch->changes[1].kind);

	auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(2U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ("two", *snapshot.group.activeInputId);

	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.CloseInput({ .operation = { .operationId = "close-2" }, .inputId = "two" }).status);
	snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.documents.size());
	EXPECT_EQ(1U, snapshot.documents[0].inputReferenceCount);
	EXPECT_EQ("one", *snapshot.group.activeInputId);

	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.CloseInput({ .operation = { .operationId = "close-1" }, .inputId = "one" }).status);
	EXPECT_TRUE(service.Snapshot().documents.empty());
}

TEST(EditorCoreService, KeepsVisibleInputsActiveSelectionAndDocumentsSeparate)
{
	EditorCoreService service;
	const auto first = ResourceIdentity(L"file:///C:/workspace/one.txt");
	const auto second = EditorDocumentIdentity{ .opaqueId = "untitled-42" };

	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-one", "one", first)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-two", "two", second)).status);
	auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.group.inputs.size());
	ASSERT_EQ(2U, snapshot.documents.size());
	EXPECT_EQ("two", *snapshot.group.activeInputId);

	const auto show = service.ShowInput({ .operation = { .operationId = "show-one" }, .inputId = "one" });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, show.status);
	snapshot = service.Snapshot();
	EXPECT_EQ("one", *snapshot.group.activeInputId);
	EXPECT_EQ(2U, snapshot.group.inputs.size());
	EXPECT_EQ(2U, snapshot.documents.size());
	const auto untitled = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [](const EditorDocumentSnapshot& document) {
		return document.identity.opaqueId == "untitled-42";
	});
	ASSERT_NE(snapshot.documents.end(), untitled);
	EXPECT_EQ(L"opaque:untitled-42", untitled->documentKey);
}

TEST(EditorCoreService, ChoosesTheNextActiveNeighborThenThePreviousAtTheEnd)
{
	EditorCoreService service;
	const auto first = EditorDocumentIdentity{ .opaqueId = "untitled-one" };
	const auto second = EditorDocumentIdentity{ .opaqueId = "untitled-two" };
	const auto third = EditorDocumentIdentity{ .opaqueId = "untitled-three" };
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-1", "one", first)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-2", "two", second)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-3", "three", third)).status);

	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.ShowInput({ .operation = { .operationId = "show-2" }, .inputId = "two" }).status);
	const auto closeMiddle = service.CloseInput({ .operation = { .operationId = "close-2" }, .inputId = "two" });
	ASSERT_EQ(EEditorOperationStatus::Succeeded, closeMiddle.status);
	ASSERT_TRUE(closeMiddle.changeBatch);
	ASSERT_EQ(3U, closeMiddle.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::ActiveInputChanged, closeMiddle.changeBatch->changes[0].kind);
	EXPECT_EQ("three", *closeMiddle.changeBatch->changes[0].activeInputId);
	EXPECT_EQ(EEditorCoreChangeKind::InputClosed, closeMiddle.changeBatch->changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, closeMiddle.changeBatch->changes[2].kind);
	EXPECT_EQ("three", *service.Snapshot().group.activeInputId);

	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.CloseInput({ .operation = { .operationId = "close-3" }, .inputId = "three" }).status);
	EXPECT_EQ("one", *service.Snapshot().group.activeInputId);
}

TEST(EditorCoreService, RejectsStaleRevisionsAndReplaysOnlyTheSameOperationRequest)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/replay.txt");
	const auto request = OpenRequest("open", "one", identity, 0);
	const auto first = service.OpenResolvedInput(request);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, first.status);

	const auto replay = service.OpenResolvedInput(request);
	EXPECT_EQ(first.status, replay.status);
	EXPECT_EQ(first.reason, replay.reason);
	EXPECT_EQ(first.revision, replay.revision);
	EXPECT_FALSE(first.replayed);
	EXPECT_TRUE(replay.replayed);
	ASSERT_TRUE(replay.changeBatch);
	EXPECT_EQ(1U, replay.changeBatch->revision);
	EXPECT_EQ(1U, service.Snapshot().revision);

	const auto stale = service.ShowInput({ .operation = { .operationId = "stale", .expectedModelRevision = 0 }, .inputId = "one" });
	EXPECT_EQ(EEditorOperationStatus::Failed, stale.status);
	EXPECT_EQ(EEditorOperationReason::RevisionConflict, stale.reason);
	EXPECT_EQ(1U, service.Snapshot().revision);

	const auto conflict = service.OpenResolvedInput(OpenRequest("open", "different-input", identity, 1));
	EXPECT_EQ(EEditorOperationStatus::Failed, conflict.status);
	EXPECT_EQ(EEditorOperationReason::OperationIdConflict, conflict.reason);
	const auto snapshot = service.Snapshot();
	EXPECT_EQ(1U, snapshot.revision);
	ASSERT_EQ(1U, snapshot.group.inputs.size());
	EXPECT_EQ("one", snapshot.group.inputs[0].descriptor.inputId);
}

TEST(EditorCoreService, RejectsInvalidIdentifiersAndDivergentResolvedDocumentStateWithoutMutation)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/conflict.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.ResolveDocument(ResolveRequest("resolve", identity, 1, false)).status);
	const auto conflict = service.ResolveDocument(ResolveRequest("resolve-conflict", identity, 2, false));
	EXPECT_EQ(EEditorOperationStatus::Failed, conflict.status);
	EXPECT_EQ(EEditorOperationReason::DocumentStateConflict, conflict.reason);
	EXPECT_EQ(1U, service.Snapshot().revision);

	const auto embeddedNul = service.ShowInput({ .operation = { .operationId = std::string("bad\0operation", 13) }, .inputId = "missing" });
	EXPECT_EQ(EEditorOperationStatus::Failed, embeddedNul.status);
	EXPECT_EQ(EEditorOperationReason::InvalidOperationId, embeddedNul.reason);
	const auto oversizedOpaque = EditorDocumentIdentity{ .opaqueId = std::string(kMaxEditorOpaqueDocumentIdLength + 1, 'x') };
	const auto invalidOpaque = service.ResolveDocument(ResolveRequest("invalid-opaque", oversizedOpaque));
	EXPECT_EQ(EEditorOperationStatus::Failed, invalidOpaque.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, invalidOpaque.reason);
	const auto oversizedInput = service.OpenResolvedInput(OpenRequest("invalid-input",
		std::string(kMaxEditorInputIdLength + 1, 'i'), identity));
	EXPECT_EQ(EEditorOperationStatus::Failed, oversizedInput.status);
	EXPECT_EQ(EEditorOperationReason::InvalidInput, oversizedInput.reason);
	EXPECT_EQ(1U, service.Snapshot().revision);
}

TEST(EditorCoreService, RejectsVeryLargeExternalIdentifiersBeforeFingerprintingOrReplayMutation)
{
	const auto identity = ResourceIdentity(L"file:///C:/workspace/large-identifiers.txt");

	EditorCoreService inputService;
	const auto initialInputSnapshot = inputService.Snapshot();
	const auto oversizedInput = inputService.OpenResolvedInput(OpenRequest("large-input-operation",
		std::string(kMaxEditorInputIdLength * 1024U, 'i'), identity));
	EXPECT_EQ(EEditorOperationStatus::Failed, oversizedInput.status);
	EXPECT_EQ(EEditorOperationReason::InvalidInput, oversizedInput.reason);
	EXPECT_EQ(initialInputSnapshot.revision, inputService.Snapshot().revision);
	EXPECT_TRUE(inputService.Snapshot().documents.empty());
	// Reusing the operation ID proves the rejected request never entered the replay table.
	EXPECT_EQ(EEditorOperationStatus::Succeeded,
		inputService.OpenResolvedInput(OpenRequest("large-input-operation", "valid-input", identity)).status);

	EditorCoreService opaqueService;
	const auto initialOpaqueSnapshot = opaqueService.Snapshot();
	const auto oversizedOpaque = EditorDocumentIdentity{ .opaqueId = std::string(kMaxEditorOpaqueDocumentIdLength * 1024U, 'o') };
	const auto invalidOpaque = opaqueService.ResolveDocument(ResolveRequest("large-opaque-operation", oversizedOpaque));
	EXPECT_EQ(EEditorOperationStatus::Failed, invalidOpaque.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, invalidOpaque.reason);
	EXPECT_EQ(initialOpaqueSnapshot.revision, opaqueService.Snapshot().revision);
	EXPECT_TRUE(opaqueService.Snapshot().documents.empty());
	// As above, the same ID remains available because no unbounded fingerprint/replay entry was created.
	EXPECT_EQ(EEditorOperationStatus::Succeeded,
		opaqueService.ResolveDocument(ResolveRequest("large-opaque-operation", identity)).status);
}

TEST(EditorCoreService, RejectsParsedResourceWhoseCanonicalComparisonKeyExceedsTheBound)
{
	std::wstring oversizedUri = L"file:///C:/workspace/";
	oversizedUri.append(kMaxEditorResourceComparisonKeyLength, L'x');
	const auto parsed = platform::uri::Uri::Parse(oversizedUri);
	ASSERT_TRUE(parsed);
	const EditorDocumentIdentity oversizedIdentity{ .resource = std::move(*parsed.value) };

	EditorCoreService service;
	const auto rejected = service.ResolveDocument(ResolveRequest("large-resource-operation", oversizedIdentity));
	EXPECT_EQ(EEditorOperationStatus::Failed, rejected.status);
	EXPECT_EQ(EEditorOperationReason::DocumentNotResolved, rejected.reason);
	EXPECT_EQ(0U, service.Snapshot().revision);
	EXPECT_TRUE(service.Snapshot().documents.empty());
	// The ID was never replay-bound to an overlarge canonical key.
	EXPECT_EQ(EEditorOperationStatus::Succeeded,
		service.ResolveDocument(ResolveRequest("large-resource-operation", ResourceIdentity(L"file:///C:/workspace/bounded.txt"))).status);
}

TEST(EditorCoreService, ReplacesOneInputDocumentAtomicallyWithoutChangingItsOrderOrActiveSelection)
{
	EditorCoreService service;
	const auto oldIdentity = ResourceIdentity(L"file:///C:/workspace/untitled-source.txt");
	const auto neighborIdentity = ResourceIdentity(L"file:///C:/workspace/neighbor.txt");
	const auto savedIdentity = ResourceIdentity(L"file:///C:/workspace/saved-as.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-source", "source", oldIdentity)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-neighbor", "neighbor", neighborIdentity)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.ShowInput({ .operation = { .operationId = "show-source" }, .inputId = "source" }).status);

	const auto replaced = service.ReplaceInputDocument(ReplaceRequest("save-as", "source", savedIdentity, 9, true, 3));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, replaced.status);
	ASSERT_TRUE(replaced.changeBatch);
	ASSERT_EQ(3U, replaced.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, replaced.changeBatch->changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputDocumentReplaced, replaced.changeBatch->changes[1].kind);
	EXPECT_EQ("source", *replaced.changeBatch->changes[1].inputId);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, replaced.changeBatch->changes[2].kind);

	const auto snapshot = service.Snapshot();
	EXPECT_EQ(4U, snapshot.revision);
	ASSERT_EQ(2U, snapshot.group.inputs.size());
	EXPECT_EQ("source", snapshot.group.inputs[0].descriptor.inputId);
	EXPECT_EQ("neighbor", snapshot.group.inputs[1].descriptor.inputId);
	ASSERT_TRUE(snapshot.group.activeInputId);
	EXPECT_EQ("source", *snapshot.group.activeInputId);
	ASSERT_TRUE(snapshot.group.inputs[0].descriptor.documentIdentity.resource);
	EXPECT_EQ(L"file:///C:/workspace/saved-as.txt", snapshot.group.inputs[0].descriptor.documentIdentity.resource->ToString());
	ASSERT_EQ(2U, snapshot.documents.size());
	const auto source = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [](const EditorDocumentSnapshot& document) {
		return document.identity.resource && document.identity.resource->ToString() == L"file:///C:/workspace/untitled-source.txt";
	});
	EXPECT_EQ(snapshot.documents.end(), source);
	const auto saved = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [](const EditorDocumentSnapshot& document) {
		return document.identity.resource && document.identity.resource->ToString() == L"file:///C:/workspace/saved-as.txt";
	});
	ASSERT_NE(snapshot.documents.end(), saved);
	EXPECT_EQ(1U, saved->inputReferenceCount);
	EXPECT_EQ(9U, saved->documentRevision);
	EXPECT_TRUE(saved->dirty);

	const auto replay = service.ReplaceInputDocument(ReplaceRequest("save-as", "source", savedIdentity, 9, true, 3));
	EXPECT_EQ(EEditorOperationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(replaced.revision, replay.revision);
	EXPECT_EQ(4U, service.Snapshot().revision);
	const auto operationConflict = service.ReplaceInputDocument(ReplaceRequest("save-as", "source", neighborIdentity, 4, false));
	EXPECT_EQ(EEditorOperationStatus::Failed, operationConflict.status);
	EXPECT_EQ(EEditorOperationReason::OperationIdConflict, operationConflict.reason);
}

TEST(EditorCoreService, ReplacesIntoExistingCanonicalDocumentAndRetainsTheOldDocumentWhileOtherInputsReferenceIt)
{
	EditorCoreService service;
	const auto oldIdentity = ResourceIdentity(L"file:///C:/workspace/shared-old.txt");
	const auto equivalentOldIdentity = ResourceIdentity(L"FILE:///c:/workspace/shared-old.txt");
	const auto targetIdentity = ResourceIdentity(L"file:///C:/workspace/already-saved.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-one", "one", oldIdentity)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-two", "two", equivalentOldIdentity)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-target", "target", targetIdentity)).status);

	const auto replaced = service.ReplaceInputDocument(ReplaceRequest("replace-one", "one", targetIdentity, 0, false, 3));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, replaced.status);
	ASSERT_TRUE(replaced.changeBatch);
	ASSERT_EQ(1U, replaced.changeBatch->changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::InputDocumentReplaced, replaced.changeBatch->changes[0].kind);

	const auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.documents.size());
	const auto old = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [](const EditorDocumentSnapshot& document) {
		return document.identity.resource && document.identity.resource->ToString() == L"file:///C:/workspace/shared-old.txt";
	});
	ASSERT_NE(snapshot.documents.end(), old);
	EXPECT_EQ(1U, old->inputReferenceCount);
	const auto target = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [](const EditorDocumentSnapshot& document) {
		return document.identity.resource && document.identity.resource->ToString() == L"file:///C:/workspace/already-saved.txt";
	});
	ASSERT_NE(snapshot.documents.end(), target);
	EXPECT_EQ(2U, target->inputReferenceCount);
	ASSERT_EQ(3U, snapshot.group.inputs.size());
	EXPECT_EQ(snapshot.group.inputs[0].documentKey, snapshot.group.inputs[2].documentKey);
	EXPECT_EQ("target", *snapshot.group.activeInputId);
}

TEST(EditorCoreService, RejectsReplaceConflictsAndNoOpsWithoutPublishingAPartialModel)
{
	EditorCoreService service;
	const auto sourceIdentity = ResourceIdentity(L"file:///C:/workspace/replace-source.txt");
	const auto targetIdentity = ResourceIdentity(L"file:///C:/workspace/replace-target.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open-source", "source", sourceIdentity)).status);
	const auto same = service.ReplaceInputDocument(ReplaceRequest("same", "source", sourceIdentity, 0, false, 1));
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, same.status);
	EXPECT_EQ(EEditorOperationReason::NoDocumentStateChange, same.reason);
	EXPECT_EQ(1U, service.Snapshot().revision);
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.OpenResolvedInput(OpenRequest("open-target", "target", targetIdentity)).status);
	const auto beforeConflict = service.Snapshot();

	const auto authoritativeConflict = service.ReplaceInputDocument(ReplaceRequest("state-conflict", "source", targetIdentity, 4, true, 2));
	EXPECT_EQ(EEditorOperationStatus::Failed, authoritativeConflict.status);
	EXPECT_EQ(EEditorOperationReason::DocumentStateConflict, authoritativeConflict.reason);
	const auto afterConflict = service.Snapshot();
	EXPECT_EQ(beforeConflict.revision, afterConflict.revision);
	EXPECT_EQ(beforeConflict.group.inputs[0].documentKey, afterConflict.group.inputs[0].documentKey);
	EXPECT_EQ(beforeConflict.documents.size(), afterConflict.documents.size());

	const auto stale = service.ReplaceInputDocument(ReplaceRequest("stale", "source", targetIdentity, 0, false, 1));
	EXPECT_EQ(EEditorOperationStatus::Failed, stale.status);
	EXPECT_EQ(EEditorOperationReason::RevisionConflict, stale.reason);
	const auto missing = service.ReplaceInputDocument(ReplaceRequest("missing", "absent", targetIdentity));
	EXPECT_EQ(EEditorOperationStatus::NotApplicable, missing.status);
	EXPECT_EQ(EEditorOperationReason::InputNotFound, missing.reason);
	const auto invalid = service.ReplaceInputDocument({ .operation = { .operationId = "invalid" }, .inputId = "source", .resolvedDocument = std::nullopt });
	EXPECT_EQ(EEditorOperationStatus::Failed, invalid.status);
	EXPECT_EQ(EEditorOperationReason::InvalidInput, invalid.reason);
}

TEST(EditorCoreService, PublishesReplaceAsOneDeterministicallyOrderedBatch)
{
	EditorCoreService service;
	const auto sourceIdentity = ResourceIdentity(L"file:///C:/workspace/replace-events-source.txt");
	const auto targetIdentity = ResourceIdentity(L"file:///C:/workspace/replace-events-target.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open", "source", sourceIdentity)).status);
	std::vector<EditorCoreChangeBatch> batches;
	auto subscription = service.Subscribe([&batches](const EditorCoreChangeBatch& batch) { batches.push_back(batch); });
	ASSERT_TRUE(subscription);

	const auto replaced = service.ReplaceInputDocument(ReplaceRequest("replace", "source", targetIdentity, 2, true));
	ASSERT_EQ(EEditorOperationStatus::Succeeded, replaced.status);
	ASSERT_EQ(1U, batches.size());
	EXPECT_EQ(replaced.revision, batches[0].revision);
	ASSERT_EQ(3U, batches[0].changes.size());
	EXPECT_EQ(EEditorCoreChangeKind::DocumentAdded, batches[0].changes[0].kind);
	EXPECT_EQ(EEditorCoreChangeKind::InputDocumentReplaced, batches[0].changes[1].kind);
	EXPECT_EQ(EEditorCoreChangeKind::DocumentReleased, batches[0].changes[2].kind);
}

TEST(EditorCoreService, DeliversOrderedPostUnlockEventsDespiteThrowingAndReentrantListenersAndUnsubscribesByRaii)
{
	EditorCoreService service;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/listeners.txt");
	std::vector<std::uint64_t> received;

	auto throwing = service.Subscribe([](const EditorCoreChangeBatch&) { throw 1; });
	auto reentrant = service.Subscribe([&service](const EditorCoreChangeBatch& batch) {
		if (batch.revision == 1) {
			const auto changed = service.SetDirty({ .operation = { .operationId = "reentrant-state" }, .inputId = "one",
				.dirty = true, .documentRevision = 7 });
			EXPECT_EQ(EEditorOperationStatus::Succeeded, changed.status);
		}
	});
	auto recorder = service.Subscribe([&received](const EditorCoreChangeBatch& batch) { received.push_back(batch.revision); });
	ASSERT_TRUE(throwing && reentrant && recorder);

	ASSERT_EQ(EEditorOperationStatus::Succeeded, service.OpenResolvedInput(OpenRequest("open", "one", identity)).status);
	ASSERT_EQ((std::vector<std::uint64_t>{ 1, 2 }), received);
	EXPECT_TRUE(std::is_sorted(received.begin(), received.end()));
	EXPECT_EQ(2U, service.Snapshot().revision);

	std::size_t shortLivedCalls = 0;
	{
		auto shortLived = service.Subscribe([&shortLivedCalls](const EditorCoreChangeBatch&) { ++shortLivedCalls; });
		ASSERT_TRUE(shortLived->IsSubscribed());
	}
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.SetDirty({ .operation = { .operationId = "later-state" }, .inputId = "one", .dirty = false, .documentRevision = 8 }).status);
	EXPECT_EQ(0U, shortLivedCalls);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2, 3 }), received);
	EXPECT_TRUE(std::is_sorted(received.begin(), received.end()));
}

TEST(EditorCoreService, CompletesEveryEarlierRevisionForAllListenersBeforeDeliveringAConcurrentCommit)
{
	EditorCoreService service;
	const auto firstIdentity = ResourceIdentity(L"file:///C:/workspace/notification-one.txt");
	const auto secondIdentity = ResourceIdentity(L"file:///C:/workspace/notification-two.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.OpenResolvedInput(OpenRequest("setup-one", "one", firstIdentity)).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		service.OpenResolvedInput(OpenRequest("setup-two", "two", secondIdentity)).status);

	std::mutex listenerMutex;
	std::condition_variable listenerCondition;
	bool firstRevisionEntered = false;
	bool releaseFirstRevision = false;
	std::vector<std::pair<std::uint64_t, int>> deliveries;

	auto blockingListener = service.Subscribe([&](const EditorCoreChangeBatch& batch) {
		std::unique_lock lock(listenerMutex);
		deliveries.emplace_back(batch.revision, 1);
		if (batch.revision == 3) {
			firstRevisionEntered = true;
			listenerCondition.notify_all();
			listenerCondition.wait(lock, [&] { return releaseFirstRevision; });
		}
	});
	auto followingListener = service.Subscribe([&](const EditorCoreChangeBatch& batch) {
		std::scoped_lock lock(listenerMutex);
		deliveries.emplace_back(batch.revision, 2);
	});
	ASSERT_TRUE(blockingListener && followingListener);

	std::optional<EditorOperationResult> firstResult;
	std::thread firstMutation([&] {
		firstResult = service.ShowInput({ .operation = { .operationId = "concurrent-show" }, .inputId = "one" });
	});

	bool listenerEntered = false;
	{
		std::unique_lock lock(listenerMutex);
		listenerEntered = listenerCondition.wait_for(lock, std::chrono::seconds(5), [&] { return firstRevisionEntered; });
		if (!listenerEntered) releaseFirstRevision = true;
	}
	if (!listenerEntered) {
		listenerCondition.notify_all();
		firstMutation.join();
		FAIL() << "the first committed revision did not reach the blocking listener";
	}

	std::optional<EditorOperationResult> secondResult;
	std::thread secondMutation([&] {
		secondResult = service.SetDirty({ .operation = { .operationId = "concurrent-state" }, .inputId = "two",
			.dirty = true, .documentRevision = 1 });
	});
	secondMutation.join();
	EXPECT_TRUE(secondResult);
	if (secondResult) {
		EXPECT_EQ(EEditorOperationStatus::Succeeded, secondResult->status);
		EXPECT_EQ(4U, secondResult->revision);
	}
	{
		std::scoped_lock lock(listenerMutex);
		// Revision 4 is committed and queued, but revision 3 has not completed registration order yet.
		EXPECT_EQ((std::vector<std::pair<std::uint64_t, int>>{ { 3, 1 } }), deliveries);
		releaseFirstRevision = true;
	}
	listenerCondition.notify_all();
	firstMutation.join();

	ASSERT_TRUE(firstResult);
	EXPECT_EQ(EEditorOperationStatus::Succeeded, firstResult->status);
	EXPECT_EQ(3U, firstResult->revision);
	EXPECT_EQ((std::vector<std::pair<std::uint64_t, int>>{
		{ 3, 1 }, { 3, 2 }, { 4, 1 }, { 4, 2 },
	}), deliveries);
}

} // namespace
} // namespace workbench::editor
