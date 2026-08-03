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
#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"
#include "workbench/workspace/WorkspaceEditingService.h"

#include <cstdint>
#include <memory>
#include <map>
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
using workbench::workspace::CWorkspaceEditingService;
using workbench::workspace::EWorkspaceEditingOutcome;
using workbench::workspace::WorkspaceFolderEdit;
using workbench::workspace::WorkspaceFoldersEditRequest;

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

class WorkspaceFakeFileService final : public IFileService {
public:
	FileResult<FileStat> Stat(const Uri&) override { return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported); }
	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override { return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported); }
	FileResult<FileBytes> Read(const Uri&, const FileReadOptions&) override { return FileResult<FileBytes>::Failure(EFileResultStatus::Unsupported); }
	FileResult<FileContentSnapshot> ReadVersioned(const Uri& resource, const FileReadOptions& options) override
	{
		++readCalls;
		lastMaximumBytes = options.maximumBytes;
		readResources.push_back(resource.ToString());
		const auto found = reads.find(resource.ToString());
		return found == reads.end() ? FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound) : found->second;
	}
	FileConditionalReplaceResult ConditionalAtomicReplace(const Uri& resource, const FileBytes& bytes,
		const FileConditionalReplaceOptions& options) override
	{
		++replaceCalls;
		lastTarget = resource.ToString();
		lastBytes = bytes;
		lastOptions = options;
		auto result = nextReplace;
		if (result.Succeeded()) writes[resource.ToString()] = bytes;
		return result;
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const Uri&, const platform::filesystem::FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
	}

	std::map<std::wstring, FileResult<FileContentSnapshot>, std::less<>> reads;
	std::map<std::wstring, FileBytes, std::less<>> writes;
	FileConditionalReplaceResult nextReplace = FileConditionalReplaceResult::Success(Token(9));
	int readCalls = 0;
	int replaceCalls = 0;
	std::size_t lastMaximumBytes = 0;
	std::vector<std::wstring> readResources;
	std::wstring lastTarget;
	FileBytes lastBytes;
	FileConditionalReplaceOptions lastOptions;
};

Uri WorkspaceUri(std::wstring_view value)
{
	auto parsed = Uri::Parse(value);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

WorkspaceFoldersEditRequest WorkspaceRequest(const Uri& source, const Uri& target,
	std::initializer_list<WorkspaceFolderEdit> folders)
{
	return { source, target, folders };
}

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

TEST(JsoncConfigurationEditor, GenericTopLevelReplacementRetainsWorkspaceTriviaOutsideFolders)
{
	const std::string document = std::string("\xef\xbb\xbf{\r\n")
		+ "  // retain top-level comment\r\n"
		+ "  \"folders\": [ { \"path\": \"old\" } ], // replace only this value\r\n"
		+ "  \"settings\": { \"editor.tabSize\": 4 },\r\n"
		+ "  \"tasks\": { \"version\": \"2.0.0\" },\r\n"
		+ "}\r\n";
	const auto edited = CJsoncConfigurationEditor::ReplaceTopLevelObjectMember(document,
		"folders", "[ { \"path\": \"new\" } ]");
	ASSERT_TRUE(edited.has_value());
	EXPECT_EQ(std::string("\xef\xbb\xbf{\r\n")
		+ "  // retain top-level comment\r\n"
		+ "  \"folders\": [ { \"path\": \"new\" } ], // replace only this value\r\n"
		+ "  \"settings\": { \"editor.tabSize\": 4 },\r\n"
		+ "  \"tasks\": { \"version\": \"2.0.0\" },\r\n"
		+ "}\r\n", *edited);
}

TEST(JsoncConfigurationEditor, WorkspaceFolderEditingUsesTheSharedJsoncPreservationPrimitive)
{
	FakeFileService files;
	const std::string document = std::string("\xef\xbb\xbf{\r\n")
		+ "  // retain\r\n"
		+ "  \"folders\": [],\r\n"
		+ "  \"settings\": { \"editor.tabSize\": 4 },\r\n"
		+ "}\r\n";
	files.nextRead = Snapshot(document);
	auto source = Uri::Parse(L"file:///C:/Workspace/project.code-workspace");
	auto folder = Uri::Parse(L"file:///C:/Workspace/App");
	ASSERT_TRUE(source.value.has_value());
	ASSERT_TRUE(folder.value.has_value());
	CWorkspaceEditingService editor(files);
	const auto result = editor.ReplaceFolders({ *source.value, *source.value,
		{ WorkspaceFolderEdit{ *folder.value, L"App" } } });
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, files.lastOptions.expectation);
	const auto rewritten = Text(files.lastBytes);
	EXPECT_NE(std::string::npos, rewritten.find("\xef\xbb\xbf{\r\n  // retain\r\n"));
	EXPECT_NE(std::string::npos, rewritten.find("\"settings\": { \"editor.tabSize\": 4 },\r\n"));
	EXPECT_NE(std::string::npos, rewritten.find("\"folders\": [\r\n    { \"path\": \"App\", \"name\": \"App\" }\r\n  ]"));
}

TEST(WorkspaceEditingService, PreservesAllNonFolderBytesAndSerializesTargetRelativePaths)
{
	WorkspaceFakeFileService files;
	const auto workspace = WorkspaceUri(L"file:///C:/Root/config/project.code-workspace");
	const std::string source = std::string("\xef\xbb\xbf{\r\n")
		+ "  // retain this exact comment\r\n"
		+ "  \"folders\": [ { \"path\": \"old\" } ], // folders trivia stays\r\n"
		+ "  \"settings\": { \"editor.tabSize\": 4 },\r\n"
		+ "  \"tasks\": { \"version\": \"2.0.0\" },\r\n"
		+ "  \"launch\": { \"configurations\": [] },\r\n"
		+ "  \"extensions\": { \"recommendations\": [] },\r\n"
		+ "  \"emoji\": \"\xf0\x9f\x98\x80\",\r\n"
		+ "  \"unknown\": [ 1, ],\r\n}\r\n";
	files.reads.emplace(workspace.ToString(), Snapshot(source));
	CWorkspaceEditingService service(files);

	const auto result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {
		{ WorkspaceUri(L"file:///C:/Root/App"), L"App \"quoted\" \\ \U0001F600" },
		{ WorkspaceUri(L"file:///C:/Root/config/Child"), std::nullopt },
		{ WorkspaceUri(L"file:///C:/Root/config/EmptyName"), std::wstring{} },
		{ WorkspaceUri(L"file:///C:/Root/config/Controls"), L"line\n\t\x0001" },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, files.lastOptions.expectation);
	const std::string expected = std::string("\xef\xbb\xbf{\r\n")
		+ "  // retain this exact comment\r\n"
		+ "  \"folders\": [\r\n    { \"path\": \"../App\", \"name\": \"App \\\"quoted\\\" \\\\ \xf0\x9f\x98\x80\" },\r\n    { \"path\": \"Child\" },\r\n    { \"path\": \"EmptyName\", \"name\": \"\" },\r\n    { \"path\": \"Controls\", \"name\": \"line\\n\\t\\u0001\" }\r\n  ], // folders trivia stays\r\n"
		+ "  \"settings\": { \"editor.tabSize\": 4 },\r\n"
		+ "  \"tasks\": { \"version\": \"2.0.0\" },\r\n"
		+ "  \"launch\": { \"configurations\": [] },\r\n"
		+ "  \"extensions\": { \"recommendations\": [] },\r\n"
		+ "  \"emoji\": \"\xf0\x9f\x98\x80\",\r\n"
		+ "  \"unknown\": [ 1, ],\r\n}\r\n";
	EXPECT_EQ(expected, Text(files.lastBytes));
}

TEST(WorkspaceEditingService, UsesPathOnlyForSameLocalFileAuthorityAndVolume)
{
	WorkspaceFakeFileService files;
	const auto workspace = WorkspaceUri(L"file:///C:/Root/project.code-workspace");
	files.reads.emplace(workspace.ToString(), Snapshot("{}"));
	CWorkspaceEditingService service(files);

	const auto result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {
		{ WorkspaceUri(L"file:///D:/Other"), std::nullopt },
		{ WorkspaceUri(L"file://server/share/folder"), std::nullopt },
		{ WorkspaceUri(L"file://localhost/C:/Root/authority-different"), std::nullopt },
		{ WorkspaceUri(L"vscode-remote://ssh-remote+host/home/test"), std::nullopt },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	const auto text = Text(files.lastBytes);
	EXPECT_NE(std::string::npos, text.find("\"path\": \"D:/Other\""));
	EXPECT_NE(std::string::npos, text.find("\"uri\": \"file://server/share/folder\""));
	EXPECT_NE(std::string::npos, text.find("\"uri\": \"file://localhost/C:/Root/authority-different\""));
	EXPECT_NE(std::string::npos, text.find("\"uri\": \"vscode-remote://ssh-remote+host/home/test\""));
}

TEST(WorkspaceEditingService, CopiesSourceWithTargetCasAndNeverMutatesSource)
{
	WorkspaceFakeFileService files;
	const auto source = WorkspaceUri(L"file:///C:/Source/project.code-workspace");
	const auto target = WorkspaceUri(L"file:///C:/Target/project.code-workspace");
	const std::string original = "{\n  // source only\n  \"settings\": { \"keep\": true }\n}\n";
	files.reads.emplace(source.ToString(), Snapshot(original, 7));
	CWorkspaceEditingService service(files);

	auto result = service.ReplaceFolders(WorkspaceRequest(source, target, {
		{ WorkspaceUri(L"file:///C:/Source/App"), std::nullopt },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(2, files.readCalls);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Missing, files.lastOptions.expectation);
	EXPECT_EQ(original, Text(files.reads.at(source.ToString()).value->bytes));
	EXPECT_EQ(target.ToString(), files.lastTarget);
	EXPECT_NE(std::string::npos, Text(files.lastBytes).find("\"path\": \"../Source/App\""));

	files.reads[target.ToString()] = Snapshot("{ \"old\": true }", 8);
	result = service.ReplaceFolders(WorkspaceRequest(source, target, {
		{ WorkspaceUri(L"file:///C:/Source/App"), std::nullopt },
	}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, files.lastOptions.expectation);
}

TEST(WorkspaceEditingService, CreatesNoopsAndReportsReadWriteAndConflictTerminals)
{
	WorkspaceFakeFileService files;
	const auto workspace = WorkspaceUri(L"file:///C:/Root/project.code-workspace");
	CWorkspaceEditingService service(files);

	// Same source/target missing is the only missing-source creation case.
	auto result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Missing, files.lastOptions.expectation);
	EXPECT_EQ("{\"folders\": []}", Text(files.lastBytes));

	files.reads[workspace.ToString()] = Snapshot("{ \"folders\": [] }");
	const auto replacesBeforeNoop = files.replaceCalls;
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(replacesBeforeNoop, files.replaceCalls);

	files.nextReplace = FileConditionalReplaceResult::Conflict();
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {
		{ WorkspaceUri(L"file:///C:/Root/App"), std::nullopt },
	}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(std::string::npos, result.diagnostic.find("C:/Root"));

	files.reads[workspace.ToString()] = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::PermissionDenied, L"C:/private");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(EFileResultStatus::PermissionDenied, result.fileStatus);
	EXPECT_EQ(std::string::npos, result.diagnostic.find("private"));

	files.nextReplace = FileConditionalReplaceResult::Failure();
	files.reads[workspace.ToString()] = Snapshot("{}");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
}

TEST(WorkspaceEditingService, AcceptsExactlySixtyFourFoldersAndRejectsOversizedOrUnreadableDocuments)
{
	WorkspaceFakeFileService files;
	const auto workspace = WorkspaceUri(L"file:///C:/Root/project.code-workspace");
	files.reads[workspace.ToString()] = Snapshot("{}");
	CWorkspaceEditingService service(files);

	std::vector<WorkspaceFolderEdit> sixtyFour;
	for (int index = 0; index != 64; ++index) {
		sixtyFour.push_back({ WorkspaceUri(L"file:///C:/Root/Folder" + std::to_wstring(index)), std::nullopt });
	}
	auto result = service.ReplaceFolders({ workspace, workspace, sixtyFour });
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, files.lastOptions.expectation);
	EXPECT_NE(std::string::npos, Text(files.lastBytes).find("Folder63"));

	const auto replacesBeforeOversize = files.replaceCalls;
	files.reads[workspace.ToString()] = Snapshot(std::string(CJsoncConfigurationEditor::kMaximumInputBytes + 1U, ' '));
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(replacesBeforeOversize, files.replaceCalls);
	EXPECT_NE(std::string::npos, result.diagnostic.find("byte limit"));

	const auto target = WorkspaceUri(L"file:///C:/Target/project.code-workspace");
	files.reads.erase(workspace.ToString());
	result = service.ReplaceFolders(WorkspaceRequest(workspace, target, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(EFileResultStatus::NotFound, result.fileStatus);
	EXPECT_EQ(replacesBeforeOversize, files.replaceCalls);

	files.reads[workspace.ToString()] = Snapshot("{}");
	files.reads[target.ToString()] = FileResult<FileContentSnapshot>::Failure(EFileResultStatus::PermissionDenied, L"C:/private/target");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, target, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(EFileResultStatus::PermissionDenied, result.fileStatus);
	EXPECT_EQ(std::string::npos, result.diagnostic.find("private"));
	EXPECT_EQ(replacesBeforeOversize, files.replaceCalls);
}

TEST(WorkspaceEditingService, RejectsInvalidJsoncUtf8SurrogatesAndBoundsButStablyDeduplicatesBeforePublish)
{
	WorkspaceFakeFileService files;
	const auto workspace = WorkspaceUri(L"file:///C:/Root/project.code-workspace");
	CWorkspaceEditingService service(files);
	files.reads[workspace.ToString()] = Snapshot("{ \"folders\": [ }");
	auto result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_TRUE(result.jsoncDiagnostic.has_value());
	EXPECT_EQ(0, files.replaceCalls);

	files.reads[workspace.ToString()] = Snapshot(std::string("{ \"note\": \"") + char(0xff) + "\" }");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(platform::serialization::EJsoncDiagnosticCode::InvalidUtf8, result.jsoncDiagnostic);
	EXPECT_EQ(0, files.replaceCalls);

	files.reads[workspace.ToString()] = Snapshot("{}");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, {
		{ WorkspaceUri(L"file:///C:/Root/App"), std::wstring(1, static_cast<wchar_t>(0xd800)) },
	}));
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);

	const auto folder = WorkspaceUri(L"file:///C:/Root/App");
	result = service.ReplaceFolders(WorkspaceRequest(workspace, workspace, { { folder, std::nullopt }, { folder, std::nullopt } }));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	const auto deduplicated = Text(files.lastBytes);
	EXPECT_EQ(deduplicated.find("App"), deduplicated.rfind("App"));
	std::vector<WorkspaceFolderEdit> duplicateOverflow(65, WorkspaceFolderEdit{ folder, std::nullopt });
	result = service.ReplaceFolders({ workspace, workspace, std::move(duplicateOverflow) });
	EXPECT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);

	std::vector<WorkspaceFolderEdit> sixtyFive;
	for (int index = 0; index != 65; ++index) sixtyFive.push_back({ WorkspaceUri(L"file:///C:/Root/" + std::to_wstring(index)), std::nullopt });
	result = service.ReplaceFolders({ workspace, workspace, std::move(sixtyFive) });
	EXPECT_EQ(EWorkspaceEditingOutcome::Failed, result.outcome);
	EXPECT_EQ(2, files.replaceCalls);
}

TEST(WorkspaceEditingService, PreservesExistingLocationAndNameIntentWhileRebasingRelativePaths)
{
	WorkspaceFakeFileService files;
	const auto source = WorkspaceUri(L"file:///C:/Source/config/project.code-workspace");
	const auto target = WorkspaceUri(L"file:///C:/Target/project.code-workspace");
	files.reads[source.ToString()] = Snapshot(R"({
  "folders": [
    { "path": "../Relative" },
    { "path": "D:/Absolute", "name": "" },
    { "uri": "vscode-remote://ssh-remote+host/home/project", "name": "Remote" }
  ],
  "settings": { "keep": true }
})");
	CWorkspaceEditingService service(files);

	const auto result = service.ReplaceFolders(WorkspaceRequest(source, target, {
		{ WorkspaceUri(L"file:///C:/Source/Relative"), L"derived-must-not-persist" },
		{ WorkspaceUri(L"file:///D:/Absolute"), L"derived-must-not-replace-empty" },
		{ WorkspaceUri(L"vscode-remote://ssh-remote+host/home/project"), L"derived-must-not-replace-explicit" },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	const auto text = Text(files.lastBytes);
	EXPECT_NE(std::string::npos, text.find("{ \"path\": \"../Source/Relative\" }"));
	EXPECT_NE(std::string::npos, text.find("{ \"path\": \"D:/Absolute\", \"name\": \"\" }"));
	EXPECT_NE(std::string::npos, text.find("{ \"uri\": \"vscode-remote://ssh-remote+host/home/project\", \"name\": \"Remote\" }"));
	EXPECT_EQ(std::string::npos, text.find("derived-must"));
	EXPECT_NE(std::string::npos, text.find("\"settings\": { \"keep\": true }"));
	const auto reparsed = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(text, target);
	ASSERT_TRUE(reparsed.Succeeded());
	ASSERT_EQ(3U, reparsed.document->folders.size());
	EXPECT_EQ(L"Absolute", reparsed.document->folders[1].displayName);
}

TEST(WorkspaceEditingService, PreservesAndReparsesUncFoldersWhenSavingAcrossShares)
{
	WorkspaceFakeFileService files;
	const auto source = WorkspaceUri(L"file://server/share/config/source.code-workspace");
	const auto target = WorkspaceUri(L"file://server/share/other/target.code-workspace");
	files.reads[source.ToString()] = Snapshot(R"({
  "folders": [
    { "path": "../Project", "name": "" },
    { "path": "\\\\other\\share\\Repo" }
  ],
  "settings": { "keep": true }
})");
	CWorkspaceEditingService service(files);

	const auto result = service.ReplaceFolders(WorkspaceRequest(source, target, {
		{ WorkspaceUri(L"file://server/share/Project"), L"must-not-replace-empty" },
		{ WorkspaceUri(L"file://other/share/Repo"), L"must-not-persist" },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	const auto text = Text(files.lastBytes);
	EXPECT_NE(std::string::npos, text.find("{ \"path\": \"//server/share/Project\", \"name\": \"\" }")) << text;
	EXPECT_NE(std::string::npos, text.find("{ \"path\": \"//other/share/Repo\" }")) << text;
	EXPECT_EQ(std::string::npos, text.find("must-not"));
	EXPECT_NE(std::string::npos, text.find("\"settings\": { \"keep\": true }"));

	const auto reparsed = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(text, target);
	ASSERT_TRUE(reparsed.Succeeded()) << text;
	ASSERT_EQ(2U, reparsed.document->folders.size());
	EXPECT_EQ(L"file://server/share/Project", reparsed.document->folders[0].uri.ToString());
	EXPECT_EQ(L"Project", reparsed.document->folders[0].displayName);
	EXPECT_EQ(L"file://other/share/Repo", reparsed.document->folders[1].uri.ToString());
	EXPECT_EQ(L"Repo", reparsed.document->folders[1].displayName);
}

TEST(WorkspaceEditingService, KeepsPathSemanticsForSameRemoteSchemeAndAuthority)
{
	WorkspaceFakeFileService files;
	const auto source = WorkspaceUri(L"vscode-remote://ssh-remote+host/home/config/source.code-workspace");
	const auto target = WorkspaceUri(L"vscode-remote://ssh-remote+host/home/other/target.code-workspace");
	files.reads[source.ToString()] = Snapshot(R"({ "folders": [ { "path": "../project" } ] })");
	CWorkspaceEditingService service(files);

	const auto result = service.ReplaceFolders(WorkspaceRequest(source, target, {
		{ WorkspaceUri(L"vscode-remote://ssh-remote+host/home/project"), std::nullopt },
	}));
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, result.outcome);
	EXPECT_NE(std::string::npos, Text(files.lastBytes).find("\"path\": \"../project\""));
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
