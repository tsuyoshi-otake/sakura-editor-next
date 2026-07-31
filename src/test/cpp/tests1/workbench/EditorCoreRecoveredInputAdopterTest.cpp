/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/persistence/EditorCoreRecoveredInputAdopter.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <optional>
#include <string>

namespace workbench::editor::persistence {
namespace {

EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
{
	auto uri = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(uri);
	return { .resource = std::move(*uri.value) };
}

EditorSessionInputDescriptor RecoveredInput(std::string inputId = "recovered.input",
	WorkingCopyPersistenceIdentity identity = { "workbench.editor.text", std::nullopt, "untitled.recovered" })
{
	return {
		.inputId = std::move(inputId),
		.inputTypeId = std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		.workingCopyIdentity = std::move(identity),
		.stateVersion = 1,
		.state = "{}",
		.backupGeneration = 7,
	};
}

OpenResolvedInputRequest ActiveOpen(std::string operationId, std::string inputId, const EditorDocumentIdentity& identity)
{
	return {
		.operation = { .operationId = std::move(operationId) },
		.input = { .inputId = std::move(inputId), .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{ .identity = identity },
	};
}

TEST(EditorCoreRecoveredInputAdopter, OpensRecoveredInputInactiveIntoAnEmptyPrimaryGroupAndPreservesIdentityAndVersion)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	const auto identity = ResourceIdentity(L"file:///C:/workspace/recovered-empty.txt");
	const auto input = RecoveredInput("recovered.input",
		{ "workbench.editor.text", "file:///C:/workspace/recovered-empty.txt", std::nullopt });

	ASSERT_TRUE(adopter.AdoptInactive(input, identity, 42));
	const auto snapshot = core.Snapshot();
	ASSERT_EQ(1U, snapshot.group.inputs.size());
	EXPECT_EQ(input.inputId, snapshot.group.inputs[0].descriptor.inputId);
	ASSERT_TRUE(snapshot.group.inputs[0].descriptor.documentIdentity.resource);
	ASSERT_TRUE(identity.resource);
	EXPECT_EQ(identity.resource->ToString(), snapshot.group.inputs[0].descriptor.documentIdentity.resource->ToString());
	EXPECT_FALSE(snapshot.group.activeInputId);
	ASSERT_EQ(1U, snapshot.documents.size());
	ASSERT_TRUE(snapshot.documents[0].identity.resource);
	EXPECT_EQ(identity.resource->ToString(), snapshot.documents[0].identity.resource->ToString());
	EXPECT_EQ(42U, snapshot.documents[0].documentRevision);
	EXPECT_TRUE(snapshot.documents[0].dirty);
}

TEST(EditorCoreRecoveredInputAdopter, OpensInactiveIntoNonemptyPrimaryGroupWithoutChangingSelection)
{
	EditorCoreService core;
	const auto activeIdentity = ResourceIdentity(L"file:///C:/workspace/active.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, core.OpenResolvedInput(ActiveOpen("existing-active", "active.input", activeIdentity)).status);
	EditorCoreRecoveredInputAdopter adopter(core);

	ASSERT_TRUE(adopter.AdoptInactive(RecoveredInput("recovered.input",
		{ "workbench.editor.text", "file:///C:/workspace/recovered-nonempty.txt", std::nullopt }),
		ResourceIdentity(L"file:///C:/workspace/recovered-nonempty.txt"), 9));
	const auto snapshot = core.Snapshot();
	ASSERT_EQ(2U, snapshot.group.inputs.size());
	ASSERT_TRUE(snapshot.group.activeInputId);
	EXPECT_EQ("active.input", *snapshot.group.activeInputId);
	EXPECT_EQ("recovered.input", snapshot.group.inputs[1].descriptor.inputId);
}

TEST(EditorCoreRecoveredInputAdopter, RejectsInvalidRecoveryRequestWithoutMutatingCore)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	auto input = RecoveredInput("recovered.input",
		{ "workbench.editor.text", "file:///C:/workspace/invalid.txt", std::nullopt });
	input.inputTypeId.clear();

	EXPECT_FALSE(adopter.AdoptInactive(input, ResourceIdentity(L"file:///C:/workspace/invalid.txt"), 1));
	EXPECT_TRUE(core.Snapshot().group.inputs.empty());
	EXPECT_EQ(0U, core.Snapshot().revision);
}

TEST(EditorCoreRecoveredInputAdopter, MapsNonSuccessCoreResultToFalse)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	const auto firstIdentity = ResourceIdentity(L"file:///C:/workspace/first.txt");
	const auto input = RecoveredInput("duplicate.input",
		{ "workbench.editor.text", "file:///C:/workspace/first.txt", std::nullopt });
	ASSERT_TRUE(adopter.AdoptInactive(input, firstIdentity, 1));

	EXPECT_FALSE(adopter.AdoptInactive(input, ResourceIdentity(L"file:///C:/workspace/second.txt"), 2));
	const auto snapshot = core.Snapshot();
	ASSERT_EQ(1U, snapshot.group.inputs.size());
	ASSERT_TRUE(snapshot.group.inputs[0].descriptor.documentIdentity.resource);
	ASSERT_TRUE(firstIdentity.resource);
	EXPECT_EQ(firstIdentity.resource->ToString(), snapshot.group.inputs[0].descriptor.documentIdentity.resource->ToString());
	EXPECT_EQ(1U, snapshot.documents[0].documentRevision);
}

TEST(EditorCoreRecoveredInputAdopter, RejectsPersistenceAndCoreIdentityMismatchOrInvalidVersion)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	const auto input = RecoveredInput("mismatch.input",
		{ "workbench.editor.text", "file:///C:/workspace/persisted.txt", std::nullopt });
	const auto other = ResourceIdentity(L"file:///C:/workspace/other.txt");

	EXPECT_FALSE(adopter.AdoptInactive(input, other, 1));
	EXPECT_FALSE(adopter.AdoptInactive(input, ResourceIdentity(L"file:///C:/workspace/persisted.txt"), 0));
	EXPECT_TRUE(core.Snapshot().group.inputs.empty());
}

TEST(EditorCoreRecoveredInputAdopter, RollsBackOnlyTheExactStillInactiveRecovery)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	const auto identity = ResourceIdentity(L"file:///C:/workspace/recovered-rollback.txt");
	const auto input = RecoveredInput("rollback.input",
		{ "workbench.editor.text", "file:///C:/workspace/recovered-rollback.txt", std::nullopt });
	ASSERT_TRUE(adopter.AdoptInactive(input, identity, 12));

	EXPECT_FALSE(adopter.RollbackInactive(input, identity, 11));
	ASSERT_EQ(1U, core.Snapshot().group.inputs.size());
	EXPECT_TRUE(adopter.RollbackInactive(input, identity, 12));
	EXPECT_TRUE(core.Snapshot().group.inputs.empty());
	EXPECT_TRUE(core.Snapshot().documents.empty());
}

TEST(EditorCoreRecoveredInputAdopter, NeverRollsBackAnActivatedRecovery)
{
	EditorCoreService core;
	EditorCoreRecoveredInputAdopter adopter(core);
	const EditorDocumentIdentity identity{ .opaqueId = "untitled.recovered" };
	const auto input = RecoveredInput();
	ASSERT_TRUE(adopter.AdoptInactive(input, identity, 4));
	const auto beforeShow = core.Snapshot();
	ASSERT_EQ(EEditorOperationStatus::Succeeded, core.ShowInput({
		.operation = { .operationId = "activate-recovery", .expectedModelRevision = beforeShow.revision },
		.inputId = input.inputId,
	}).status);

	EXPECT_FALSE(adopter.RollbackInactive(input, identity, 4));
	ASSERT_TRUE(core.Snapshot().group.activeInputId);
	EXPECT_EQ(input.inputId, *core.Snapshot().group.activeInputId);
}

} // namespace
} // namespace workbench::editor::persistence
