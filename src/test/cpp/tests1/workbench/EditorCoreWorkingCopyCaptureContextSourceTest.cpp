/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/editor/persistence/EditorCoreWorkingCopyCaptureContextSource.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <string>
#include <utility>

namespace workbench::editor::persistence {
namespace {

EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
{
	auto uri = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(uri);
	return uri ? EditorDocumentIdentity{ .resource = std::move(*uri.value) }
		: EditorDocumentIdentity{};
}

EditorOperationResult Open(EditorCoreService& core, std::string operationId,
	std::string inputId, const EditorDocumentIdentity& identity,
	std::uint64_t documentRevision, bool activate = true)
{
	return core.OpenResolvedInput({
		.operation = { .operationId = std::move(operationId) },
		.input = { .inputId = std::move(inputId), .documentIdentity = identity },
		.resolvedDocument = ResolvedEditorDocument{
			.identity = identity,
			.documentRevision = documentRevision,
		},
		.activate = activate,
	});
}

TEST(EditorCoreWorkingCopyCaptureContextSource, ReturnsTheActiveInputExactIdentityAndDocumentRevisionFromOneSnapshot)
{
	EditorCoreService core;
	const auto identity = ResourceIdentity(L"file:///C:/workspace/current.txt");
	ASSERT_EQ(EEditorOperationStatus::Succeeded, Open(core, "open", "input.current", identity, 7).status);

	EditorCoreWorkingCopyCaptureContextSource source(core);
	const auto context = source.CurrentCaptureContext();

	ASSERT_TRUE(context);
	EXPECT_EQ("input.current", context->inputId);
	EXPECT_EQ(CWorkingCopyPersistenceCodec::kTextInputTypeId, context->inputTypeId);
	EXPECT_EQ(7U, context->documentRevision);
	ASSERT_TRUE(context->documentIdentity.resource);
	EXPECT_EQ(identity.resource->ToString(), context->documentIdentity.resource->ToString());
	EXPECT_FALSE(context->documentIdentity.opaqueId);
}

TEST(EditorCoreWorkingCopyCaptureContextSource, RefusesEmptyInactiveAndRevisionZeroCurrentState)
{
	EditorCoreService core;
	EditorCoreWorkingCopyCaptureContextSource source(core);
	EXPECT_FALSE(source.CurrentCaptureContext());

	const auto inactiveIdentity = EditorDocumentIdentity{ .opaqueId = "untitled.inactive" };
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		Open(core, "open-inactive", "input.inactive", inactiveIdentity, 4, false).status);
	EXPECT_FALSE(source.CurrentCaptureContext());

	const auto zeroRevisionIdentity = EditorDocumentIdentity{ .opaqueId = "untitled.zero" };
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		Open(core, "open-zero", "input.zero", zeroRevisionIdentity, 0).status);
	EXPECT_FALSE(source.CurrentCaptureContext());
}

TEST(EditorCoreWorkingCopyCaptureContextSource, FollowsTheActiveInputWithoutReconstructingOpaqueIdentity)
{
	EditorCoreService core;
	const auto inactiveIdentity = EditorDocumentIdentity{ .opaqueId = "untitled.inactive" };
	const auto activeIdentity = EditorDocumentIdentity{ .opaqueId = "untitled.active.exact" };
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		Open(core, "open-inactive", "input.inactive", inactiveIdentity, 3, false).status);
	ASSERT_EQ(EEditorOperationStatus::Succeeded,
		Open(core, "open-active", "input.active", activeIdentity, 11).status);

	EditorCoreWorkingCopyCaptureContextSource source(core);
	const auto context = source.CurrentCaptureContext();

	ASSERT_TRUE(context);
	EXPECT_EQ("input.active", context->inputId);
	EXPECT_EQ(11U, context->documentRevision);
	EXPECT_EQ(std::optional<std::string>("untitled.active.exact"), context->documentIdentity.opaqueId);
	EXPECT_FALSE(context->documentIdentity.resource);
}

} // namespace
} // namespace workbench::editor::persistence
