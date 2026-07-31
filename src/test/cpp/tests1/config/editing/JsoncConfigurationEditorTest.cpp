/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/editing/CJsoncConfigurationEditor.h"
#include "config/CConfigurationService.h"
#include "config/ConfigurationFileSourceController.h"
#include "config/SettingsWritebackCoordinator.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::ConfigurationSource;
using config::CConfigurationFileSourceController;
using config::CConfigurationService;
using config::CSettingsWritebackCoordinator;
using config::EConfigurationScope;
using config::ESettingsWritebackStatus;
using config::SettingsWritebackRequest;
using config::editing::CJsoncConfigurationEditor;
using config::editing::ConfigurationDocumentEditRequest;
using config::editing::EConfigurationDocumentEditStatus;
using config::editing::EConfigurationDocumentScope;
using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileConditionalReplaceExpectation;
using platform::filesystem::EFileConditionalReplaceStatus;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::FileBytes;
using platform::filesystem::FileConditionalReplaceOptions;
using platform::filesystem::FileConditionalReplaceResult;
using platform::filesystem::FileContentSnapshot;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::FileVersionToken;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
using platform::uri::Uri;

Uri Resource()
{
	auto parsed = Uri::Parse(L"file:///C:/Sensitive/profile/settings.json");
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

FileVersionToken Token(std::uint8_t value)
{
	const std::uint8_t bytes[] { value };
	auto token = FileVersionToken::FromOpaqueBytes(bytes);
	EXPECT_TRUE(token.has_value());
	return *token;
}

FileResult<FileContentSnapshot> Snapshot(std::string document, std::uint8_t version = 7)
{
	return FileResult<FileContentSnapshot>::Success({ FileBytes(document.begin(), document.end()), Token(version) });
}

std::string Text(const FileBytes& bytes)
{
	return std::string(bytes.begin(), bytes.end());
}

class FakeFileService final : public IFileService {
public:
	FileResult<FileStat> Stat(const Uri&) override { return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported); }
	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override { return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported); }
	FileResult<FileBytes> Read(const Uri&, const FileReadOptions&) override
	{
		return sourceReadEnabled ? FileResult<FileBytes>::Success(current) : FileResult<FileBytes>::Failure(EFileResultStatus::Unsupported);
	}
	FileResult<FileContentSnapshot> ReadVersioned(const Uri&, const FileReadOptions& options) override
	{
		++readCalls; lastMaximumBytes = options.maximumBytes; return nextRead;
	}
	FileConditionalReplaceResult ConditionalAtomicReplace(const Uri&, const FileBytes& bytes, const FileConditionalReplaceOptions& options) override
	{
		++replaceCalls; lastBytes = bytes; lastOptions = options;
		auto result = nextReplace;
		if (replaceResultIndex < replaceResults.size()) result = replaceResults[replaceResultIndex++];
		if (result.status == EFileConditionalReplaceStatus::Conflict && nextReadAfterConflict) nextRead = *nextReadAfterConflict;
		if (result.Succeeded()) current = bytes;
		return result;
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const Uri&, const platform::filesystem::FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
	}

	FileResult<FileContentSnapshot> nextRead = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound);
	FileConditionalReplaceResult nextReplace = FileConditionalReplaceResult::Success(Token(8));
	std::vector<FileConditionalReplaceResult> replaceResults;
	std::size_t replaceResultIndex = 0;
	std::optional<FileResult<FileContentSnapshot>> nextReadAfterConflict;
	FileBytes current;
	bool sourceReadEnabled = false;
	int readCalls = 0;
	int replaceCalls = 0;
	std::size_t lastMaximumBytes = 0;
	FileBytes lastBytes;
	FileConditionalReplaceOptions lastOptions;
};

ConfigurationDocumentEditRequest Request(std::optional<ConfigurationValue> value = ConfigurationValue(3))
{
	ConfigurationDocumentEditRequest request;
	request.target.scope = EConfigurationDocumentScope::Profile;
	request.target.target.profileId = L"profile-1";
	request.target.resource = Resource();
	request.key = "editor.tabSize";
	request.value = std::move(value);
	return request;
}

ConfigurationDocumentEditRequest LanguageRequest(std::optional<ConfigurationValue> value = ConfigurationValue(3))
{
	auto request = Request(std::move(value));
	request.target.scope = EConfigurationDocumentScope::LanguageOverride;
	request.target.target.languageId = L"cpp";
	return request;
}

std::vector<config::ConfigurationDescriptor> Descriptors()
{
	return {
		{ "editor.tabSize", ConfigurationValue(4), { EConfigurationScope::Profile, EConfigurationScope::LanguageOverride } },
	};
}

SettingsWritebackRequest WritebackRequest(ConfigurationDocumentEditRequest edit)
{
	ConfigurationSource source;
	source.scope = EConfigurationScope::Profile;
	source.target.profileId = L"profile-1";
	source.sourceId = "profile.settings";
	return { std::move(edit), "profile.settings", std::move(source) };
}

} // namespace

TEST(JsoncConfigurationEditor, ReplacesOnlyTheValueAndRetainsBomCrLfCommentsAndUnknownFields)
{
	FakeFileService files;
	const std::string document = std::string("\xef\xbb\xbf{")
		+ "\r\n  // keep this comment\r\n  \"unknown\": { \"nested\": true },\r\n  \"editor.tabSize\": 2, // keep this too\r\n}\r\n";
	files.nextRead = Snapshot(document);
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request(ConfigurationValue(4)));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, result.status);
	EXPECT_EQ(CJsoncConfigurationEditor::kMaximumInputBytes, files.lastMaximumBytes);
	EXPECT_EQ(1, files.replaceCalls);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, files.lastOptions.expectation);
	EXPECT_EQ(std::string("\xef\xbb\xbf{")
		+ "\r\n  // keep this comment\r\n  \"unknown\": { \"nested\": true },\r\n  \"editor.tabSize\": 4, // keep this too\r\n}\r\n", Text(files.lastBytes));
}

TEST(JsoncConfigurationEditor, InsertsAndRemovesWithoutReserializingNeighboringJsonc)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);
	files.nextRead = Snapshot("{\r\n  // preserve\r\n  \"unknown\": true\r\n}\r\n");
	auto inserted = editor.Edit(Request(ConfigurationValue(3)));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, inserted.status);
	const auto insertedText = Text(files.lastBytes);
	EXPECT_NE(std::string::npos, insertedText.find("// preserve\r\n  \"unknown\": true,\r\n  \"editor.tabSize\": 3"));

	files.nextRead = Snapshot("{\r\n  \"keep\": true,\r\n  \"editor.tabSize\": 2,\r\n  // neighboring comment\r\n  \"later\": false\r\n}\r\n", 8);
	auto removed = editor.Edit(Request(std::nullopt));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, removed.status);
	const auto removedText = Text(files.lastBytes);
	EXPECT_EQ(std::string::npos, removedText.find("editor.tabSize"));
	EXPECT_NE(std::string::npos, removedText.find("\"keep\": true,"));
	EXPECT_NE(std::string::npos, removedText.find("// neighboring comment\r\n  \"later\": false"));
	EXPECT_TRUE(platform::serialization::CJsoncDocument::Parse(removedText).Succeeded());
}

TEST(JsoncConfigurationEditor, DoesNotWriteMalformedExistingDocument)
{
	FakeFileService files;
	files.nextRead = Snapshot("{ \"editor.tabSize\": 2, \"editor.tabSize\": 4 }");
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request());
	EXPECT_EQ(EConfigurationDocumentEditStatus::ParseFailed, result.status);
	EXPECT_EQ(0, files.replaceCalls);
}

TEST(JsoncConfigurationEditor, ReturnsConflictWithoutRetryOrOverwrite)
{
	FakeFileService files;
	files.nextRead = Snapshot("{ \"editor.tabSize\": 2 }");
	files.nextReplace = FileConditionalReplaceResult::Conflict(L"contains an implementation path that must not escape");
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request(ConfigurationValue(4)));
	EXPECT_EQ(EConfigurationDocumentEditStatus::Conflict, result.status);
	EXPECT_EQ(1, files.readCalls);
	EXPECT_EQ(1, files.replaceCalls);
}

TEST(JsoncConfigurationEditor, CreatesMissingDocumentWithExpectedMissingCas)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request(ConfigurationValue(true)));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, result.status);
	ASSERT_EQ(1, files.replaceCalls);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Missing, files.lastOptions.expectation);
	EXPECT_EQ("{\n  \"editor.tabSize\": true\n}\n", Text(files.lastBytes));
}

TEST(JsoncConfigurationEditor, EnforcesOutputBoundBeforePublishing)
{
	FakeFileService files;
	files.nextRead = Snapshot(std::string(CJsoncConfigurationEditor::kMaximumInputBytes - 2, ' ') + "{}");
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request(ConfigurationValue(3)));
	EXPECT_EQ(EConfigurationDocumentEditStatus::OutputTooLarge, result.status);
	EXPECT_EQ(0, files.replaceCalls);
}

TEST(JsoncConfigurationEditor, RejectsOversizedInputAndInvalidRequestsBeforePublishing)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);
	files.nextRead = Snapshot(std::string(CJsoncConfigurationEditor::kMaximumInputBytes + 1, ' '));
	EXPECT_EQ(EConfigurationDocumentEditStatus::InputTooLarge, editor.Edit(Request()).status);
	EXPECT_EQ(0, files.replaceCalls);

	auto invalid = Request();
	invalid.key = "invalid key";
	EXPECT_EQ(EConfigurationDocumentEditStatus::InvalidRequest, editor.Edit(invalid).status);
	EXPECT_EQ(0, files.replaceCalls);

	auto languageOverride = Request();
	languageOverride.target.target.languageId = L"cpp";
	EXPECT_EQ(EConfigurationDocumentEditStatus::InvalidRequest, editor.Edit(languageOverride).status);
	EXPECT_EQ(0, files.replaceCalls);

	auto mismatchedWorkspace = Request();
	mismatchedWorkspace.target.scope = EConfigurationDocumentScope::Workspace;
	EXPECT_EQ(EConfigurationDocumentEditStatus::InvalidRequest, editor.Edit(mismatchedWorkspace).status);
	EXPECT_EQ(0, files.replaceCalls);

	auto invalidUnicode = Request(ConfigurationValue(std::wstring(1, static_cast<wchar_t>(0xd800))));
	EXPECT_EQ(EConfigurationDocumentEditStatus::InvalidRequest, editor.Edit(invalidUnicode).status);
	EXPECT_EQ(0, files.replaceCalls);
}

TEST(JsoncConfigurationEditor, SerializesValuesDeterministicallyWithJsonEscaping)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(Request(ConfigurationValue(L"\"\\\n\u65e5")));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, result.status);
	EXPECT_EQ("{\n  \"editor.tabSize\": \"\\\"\\\\\\n\\u65E5\"\n}\n", Text(files.lastBytes));
}

TEST(JsoncConfigurationEditor, ReportsDistinctTerminalFailuresAndNeverLeaksRequestDetails)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);
	const auto request = Request(ConfigurationValue(L"private-value"));

	files.nextRead = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::PermissionDenied, L"C:/Sensitive/profile/settings.json");
	auto readFailed = editor.Edit(request);
	EXPECT_EQ(EConfigurationDocumentEditStatus::ReadFailed, readFailed.status);
	files.nextRead = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::Unsupported);
	EXPECT_EQ(EConfigurationDocumentEditStatus::Unsupported, editor.Edit(request).status);
	files.nextRead = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::Failed);
	EXPECT_EQ(EConfigurationDocumentEditStatus::Failed, editor.Edit(request).status);
	files.nextRead = Snapshot("{ \"editor.tabSize\": 2 }");
	files.nextReplace = FileConditionalReplaceResult::Unsupported();
	EXPECT_EQ(EConfigurationDocumentEditStatus::Unsupported, editor.Edit(request).status);
	files.nextReplace = FileConditionalReplaceResult::Failure();
	EXPECT_EQ(EConfigurationDocumentEditStatus::Failed, editor.Edit(request).status);
	files.nextReplace = FileConditionalReplaceResult::Success(Token(9));
	auto removeMissing = Request(std::nullopt);
	files.nextRead = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound);
	EXPECT_EQ(EConfigurationDocumentEditStatus::NoChange, editor.Edit(removeMissing).status);

	for (const auto& result : { readFailed, editor.Edit(request) }) {
		EXPECT_EQ(std::string::npos, result.diagnostic.find("C:/Sensitive"));
		EXPECT_EQ(std::string::npos, result.diagnostic.find("editor.tabSize"));
		EXPECT_EQ(std::string::npos, result.diagnostic.find("private-value"));
	}
}

TEST(JsoncConfigurationEditor, EditsOnlyTheRequestedVsCodeLanguageOverrideMember)
{
	FakeFileService files;
	files.nextRead = Snapshot("{\r\n  // keep root trivia\r\n  \"[cpp]\": {\r\n    // keep override trivia\r\n    \"editor.tabSize\": 2,\r\n    \"unknown\": true,\r\n  },\r\n  \"editor.wordWrap\": \"off\",\r\n}\r\n");
	CJsoncConfigurationEditor editor(files);

	auto result = editor.Edit(LanguageRequest(ConfigurationValue(4)));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, result.status);
	EXPECT_EQ("{\r\n  // keep root trivia\r\n  \"[cpp]\": {\r\n    // keep override trivia\r\n    \"editor.tabSize\": 4,\r\n    \"unknown\": true,\r\n  },\r\n  \"editor.wordWrap\": \"off\",\r\n}\r\n", Text(files.lastBytes));
}

TEST(JsoncConfigurationEditor, CreatesMissingVsCodeLanguageOverrideAndRejectsCombinedSelectorEdits)
{
	FakeFileService files;
	CJsoncConfigurationEditor editor(files);

	auto created = editor.Edit(LanguageRequest(ConfigurationValue(4)));
	ASSERT_EQ(EConfigurationDocumentEditStatus::Applied, created.status);
	EXPECT_EQ("{\n  \"[cpp]\": {\n    \"editor.tabSize\": 4\n  }\n}\n", Text(files.lastBytes));
	EXPECT_EQ(EFileConditionalReplaceExpectation::Missing, files.lastOptions.expectation);

	files.nextRead = Snapshot("{ \"[cpp][c]\": { \"editor.tabSize\": 2 } }");
	files.replaceCalls = 0;
	auto combined = editor.Edit(LanguageRequest(ConfigurationValue(4)));
	EXPECT_EQ(EConfigurationDocumentEditStatus::Unsupported, combined.status);
	EXPECT_EQ(0, files.replaceCalls);
}

TEST(SettingsWritebackCoordinator, ReplaysOneCasConflictThenResnapshotsTheSameSourceOwner)
{
	FakeFileService files;
	files.nextRead = Snapshot("{ \"[cpp]\": { \"editor.tabSize\": 2 } }");
	files.sourceReadEnabled = true;
	files.replaceResults = {
		FileConditionalReplaceResult::Conflict(),
		FileConditionalReplaceResult::Success(Token(9)),
	};
	files.nextReadAfterConflict = Snapshot("{ // concurrent edit\n  \"[cpp]\": { \"editor.tabSize\": 3, \"external.setting\": true }\n}", 8);
	CConfigurationService configuration(Descriptors());
	CConfigurationFileSourceController sources(files, configuration);
	CSettingsWritebackCoordinator coordinator(files, sources);

	auto result = coordinator.Write(WritebackRequest(LanguageRequest(ConfigurationValue(4))));
	ASSERT_EQ(ESettingsWritebackStatus::Replayed, result.status);
	ASSERT_EQ(2U, result.attempts);
	ASSERT_EQ(2, files.readCalls);
	ASSERT_EQ(2, files.replaceCalls);
	EXPECT_NE(std::string::npos, Text(files.lastBytes).find("\"external.setting\": true"));
	ASSERT_TRUE(result.resnapshot.has_value());
	EXPECT_TRUE(result.resnapshot->Succeeded());

	ConfigurationTarget target;
	target.profileId = L"profile-1";
	target.languageId = L"cpp";
	auto effective = configuration.ReadSnapshot({ "editor.tabSize" }, target);
	ASSERT_TRUE(effective.snapshot.has_value());
	EXPECT_EQ(4, std::get<std::int64_t>(effective.snapshot->values.front().Value()));
}

TEST(SettingsWritebackCoordinator, BoundsConflictsAndRejectsPostStopWritesWithoutIo)
{
	FakeFileService files;
	files.nextRead = Snapshot("{ \"editor.tabSize\": 2 }");
	files.replaceResults = {
		FileConditionalReplaceResult::Conflict(),
		FileConditionalReplaceResult::Conflict(),
	};
	CConfigurationService configuration(Descriptors());
	CConfigurationFileSourceController sources(files, configuration);
	CSettingsWritebackCoordinator coordinator(files, sources);

	auto conflicted = coordinator.Write(WritebackRequest(Request(ConfigurationValue(4))));
	EXPECT_EQ(ESettingsWritebackStatus::Conflict, conflicted.status);
	EXPECT_EQ(CSettingsWritebackCoordinator::kMaximumAttempts, conflicted.attempts);
	EXPECT_EQ(2, files.readCalls);
	EXPECT_EQ(2, files.replaceCalls);

	EXPECT_EQ(ESettingsWritebackStatus::Stopped, coordinator.Stop().status);
	const auto readsBeforeStop = files.readCalls;
	const auto replacesBeforeStop = files.replaceCalls;
	auto stopped = coordinator.Write(WritebackRequest(Request(ConfigurationValue(4))));
	EXPECT_EQ(ESettingsWritebackStatus::Stopped, stopped.status);
	EXPECT_EQ(readsBeforeStop, files.readCalls);
	EXPECT_EQ(replacesBeforeStop, files.replaceCalls);
}

TEST(SettingsWritebackCoordinator, ReportsARejectedResnapshotInsteadOfApplyingTheRequestedValue)
{
	FakeFileService files;
	files.nextRead = Snapshot("{ \"editor.tabSize\": 2 }");
	// Deliberately leave sourceReadEnabled false: publication succeeds but the
	// semantic owner cannot read the file back and must expose that terminal state.
	CConfigurationService configuration(Descriptors());
	CConfigurationFileSourceController sources(files, configuration);
	CSettingsWritebackCoordinator coordinator(files, sources);

	auto result = coordinator.Write(WritebackRequest(Request(ConfigurationValue(7))));
	ASSERT_EQ(ESettingsWritebackStatus::ResnapshotRejected, result.status);
	ASSERT_TRUE(result.resnapshot.has_value());
	EXPECT_FALSE(result.resnapshot->Succeeded());
	ConfigurationTarget target;
	target.profileId = L"profile-1";
	auto effective = configuration.ReadSnapshot({ "editor.tabSize" }, target);
	ASSERT_TRUE(effective.snapshot.has_value());
	EXPECT_EQ(4, std::get<std::int64_t>(effective.snapshot->values.front().Value()));
}
