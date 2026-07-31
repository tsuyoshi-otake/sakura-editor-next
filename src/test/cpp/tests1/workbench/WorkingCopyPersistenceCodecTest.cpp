/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/storage/StorageTypes.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <string>

namespace {

using workbench::editor::persistence::CWorkingCopyPersistenceCodec;
using workbench::editor::persistence::EditorSessionInputDescriptor;
using workbench::editor::persistence::EditorSessionManifest;
using workbench::editor::persistence::EWorkingCopyEol;
using workbench::editor::persistence::EWorkingCopyPersistenceCodecStatus;
using workbench::editor::persistence::EWorkingCopyTextEncoding;
using workbench::editor::persistence::WorkingCopyBackup;
using workbench::editor::persistence::WorkingCopyPersistenceIdentity;
using workbench::editor::persistence::WorkingCopyPersistenceScope;

WorkingCopyPersistenceScope SampleScope()
{
	return { "profile.primary", "workspace.7f4ce4" };
}

WorkingCopyPersistenceIdentity SampleIdentity()
{
	return {
		std::string(CWorkingCopyPersistenceCodec::kTextInputTypeId),
		"file:///C%3A/workspace/example.txt",
		std::nullopt,
	};
}

WorkingCopyBackup SampleBackup()
{
	WorkingCopyBackup backup;
	backup.scope = SampleScope();
	backup.identity = SampleIdentity();
	backup.generation = 17;
	backup.contentVersion = 42;
	backup.encoding = EWorkingCopyTextEncoding::Utf8;
	backup.eol = EWorkingCopyEol::CrLf;
	backup.dirty = true;
	backup.content = "first line\r\nsecond line\r\n";
	backup.checksum = CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content);
	return backup;
}

EditorSessionManifest SampleSession()
{
	EditorSessionManifest manifest;
	manifest.scope = SampleScope();
	manifest.generation = 18;
	manifest.logicalGroupId = "workbench.editorGroup.primary";
	manifest.activeInputId = "input.example";
	manifest.inputs = {
		{ "input.example", "workbench.editor.text", SampleIdentity(), 1, "{\"viewState\":\"text\"}", 17 },
	};
	return manifest;
}

} // namespace

TEST(WorkingCopyPersistenceCodec, BackupRoundTripIsDeterministicAndPreservesLogicalIdentity)
{
	const auto backup = SampleBackup();
	const auto first = CWorkingCopyPersistenceCodec::EncodeBackup(backup);
	const auto second = CWorkingCopyPersistenceCodec::EncodeBackup(backup);
	ASSERT_TRUE(first.Succeeded()) << first.diagnostic;
	ASSERT_TRUE(second.Succeeded()) << second.diagnostic;
	EXPECT_EQ(first.payload, second.payload);
	EXPECT_EQ(std::string::npos, first.payload.find("hwnd"));
	EXPECT_EQ(std::string::npos, first.payload.find("displayTitle"));

	const auto decoded = CWorkingCopyPersistenceCodec::DecodeBackup(first.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_EQ("profile.primary", decoded.backup->scope.profileId);
	ASSERT_TRUE(decoded.backup->scope.workspaceId.has_value());
	EXPECT_EQ("workspace.7f4ce4", *decoded.backup->scope.workspaceId);
	EXPECT_EQ(SampleIdentity().typeId, decoded.backup->identity.typeId);
	EXPECT_EQ(SampleIdentity().canonicalResource, decoded.backup->identity.canonicalResource);
	EXPECT_EQ(SampleIdentity().opaqueId, decoded.backup->identity.opaqueId);
	EXPECT_EQ(17U, decoded.backup->generation);
	EXPECT_EQ(42U, decoded.backup->contentVersion);
	EXPECT_EQ(backup.checksum, decoded.backup->checksum);
	EXPECT_EQ(backup.content, decoded.backup->content);
	EXPECT_EQ(first.payload, CWorkingCopyPersistenceCodec::EncodeBackup(*decoded.backup).payload);
}

TEST(WorkingCopyPersistenceCodec, BackupRoundTripSupportsContentBeyondStorageEntryLimit)
{
	auto backup = SampleBackup();
	backup.content.assign(platform::storage::kMaximumStorageStringBytes + 32 * 1024, 'x');
	backup.checksum = CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content);

	const auto encoded = CWorkingCopyPersistenceCodec::EncodeBackup(backup);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	const auto decoded = CWorkingCopyPersistenceCodec::DecodeBackup(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_EQ(backup.content, decoded.backup->content);
}

TEST(WorkingCopyPersistenceCodec, BackupChecksumMismatchIsTerminalAndDoesNotExposePartialRecord)
{
	auto payload = CWorkingCopyPersistenceCodec::EncodeBackup(SampleBackup()).payload;
	const auto content = payload.find("first line");
	ASSERT_NE(std::string::npos, content);
	payload[content] = 'F';

	const auto decoded = CWorkingCopyPersistenceCodec::DecodeBackup(payload);
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::ChecksumMismatch, decoded.status);
	EXPECT_FALSE(decoded.backup.has_value());
}

TEST(WorkingCopyPersistenceCodec, UnsupportedSchemaAndPartialCorruptionAreDistinctTerminalFailures)
{
	const auto unsupported = CWorkingCopyPersistenceCodec::DecodeBackup(R"json({
		"formatVersion":2,"scope":{"profileId":"profile.primary"},
		"identity":{"typeId":"workbench.editor.text","opaqueId":"untitled.1"},
		"generation":1,"contentVersion":1,"checksum":"cbf29ce484222325",
		"encoding":"utf8","eol":"lf","dirty":true,"content":""
	})json");
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::UnsupportedSchema, unsupported.status);
	EXPECT_FALSE(unsupported.backup.has_value());

	const auto partial = CWorkingCopyPersistenceCodec::DecodeBackup(
		R"json({"formatVersion":1,"scope":{"profileId":"profile.primary"})json");
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::CorruptPayload, partial.status);
	EXPECT_FALSE(partial.backup.has_value());
}

TEST(WorkingCopyPersistenceCodec, BackupPayloadLimitIsTerminal)
{
	const std::string oversized(platform::storage::kMaximumStorageSnapshotPayloadBytes + 1, 'x');
	const auto decoded = CWorkingCopyPersistenceCodec::DecodeBackup(oversized);
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::PayloadTooLarge, decoded.status);
	EXPECT_FALSE(decoded.backup.has_value());
}

TEST(WorkingCopyPersistenceCodec, BackupRequiresDirtyContentAndBoundsCanonicalResourceIdentity)
{
	auto clean = SampleBackup();
	clean.dirty = false;
	const auto cleanResult = CWorkingCopyPersistenceCodec::EncodeBackup(clean);
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::InvalidRecord, cleanResult.status);

	auto oversizedResource = SampleBackup();
	oversizedResource.identity.canonicalResource = std::string(
		workbench::editor::persistence::kMaximumWorkingCopyPersistenceResourceBytes + 1, 'r');
	const auto resourceResult = CWorkingCopyPersistenceCodec::EncodeBackup(oversizedResource);
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::InvalidRecord, resourceResult.status);
}

TEST(WorkingCopyPersistenceCodec, SessionSupportsZeroInputsAndOptionalActiveInputWithoutDuplicatingBackupContent)
{
	EditorSessionManifest empty;
	empty.scope = SampleScope();
	empty.generation = 3;
	empty.logicalGroupId = "workbench.editorGroup.primary";
	const auto encoded = CWorkingCopyPersistenceCodec::EncodeSession(empty);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	const auto decoded = CWorkingCopyPersistenceCodec::DecodeSession(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.manifest->inputs.empty());
	EXPECT_FALSE(decoded.manifest->activeInputId.has_value());
}

TEST(WorkingCopyPersistenceCodec, SessionInputReferencesBackupGenerationAndRejectsUnknownType)
{
	const auto manifest = SampleSession();
	const auto encoded = CWorkingCopyPersistenceCodec::EncodeSession(manifest);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;
	EXPECT_EQ(std::string::npos, encoded.payload.find("first line"));
	const auto decoded = CWorkingCopyPersistenceCodec::DecodeSession(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(1U, decoded.manifest->inputs.size());
	ASSERT_TRUE(decoded.manifest->inputs.front().backupGeneration.has_value());
	EXPECT_EQ(17U, *decoded.manifest->inputs.front().backupGeneration);
	EXPECT_EQ(SampleIdentity().typeId, decoded.manifest->inputs.front().workingCopyIdentity.typeId);
	EXPECT_EQ(SampleIdentity().canonicalResource, decoded.manifest->inputs.front().workingCopyIdentity.canonicalResource);
	EXPECT_EQ(SampleIdentity().opaqueId, decoded.manifest->inputs.front().workingCopyIdentity.opaqueId);

	const auto unknown = CWorkingCopyPersistenceCodec::DecodeSession(R"json({
		"formatVersion":1,"scope":{"profileId":"profile.primary"},"generation":1,
		"logicalGroupId":"workbench.editorGroup.primary","inputs":[{
			"inputId":"publisher.input","inputTypeId":"publisher.future",
			"identity":{"typeId":"workbench.editor.text","opaqueId":"opaque.1"},
			"stateVersion":1,"state":""
		}]
	})json");
	EXPECT_EQ(EWorkingCopyPersistenceCodecStatus::UnknownInputType, unknown.status);
	EXPECT_FALSE(unknown.manifest.has_value());
}
