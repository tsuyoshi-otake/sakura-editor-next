/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/projects/AgentSessionStoreOperations.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace workbench::projects;
using platform::process::BoundedProcessRequest;
using platform::process::BoundedProcessResult;
using platform::process::EBoundedProcessStatus;
using platform::process::EBoundedProcessStream;
using Status = EAgentSessionOperationStatus;

constexpr wchar_t kClaudeId[] = L"0f1e2d3c-4b5a-4968-8776-655443322110";
constexpr wchar_t kCodexId[] = L"019a0000-0000-7000-8000-000000000001";
constexpr wchar_t kPiId[] = L"019a1111-2222-7333-8444-555566667777";

void WriteText(const std::filesystem::path& path, const std::string_view content)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(stream.good()) << path.string();
	stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadText(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

//! What the fake `run` saw of one request.
struct SeenRun final {
	std::wstring executable;
	std::wstring workingDirectory;
	std::vector<std::wstring> arguments;
	std::string standardInput;
	std::uint32_t timeoutMilliseconds{};
	bool hadObserver = false;
	bool hadCloseEvent = false;
	bool closeEventSignalledAfterReply = false;
};

class StoreFixture : public ::testing::Test {
protected:
	void SetUp() override
	{
		base = std::filesystem::temp_directory_path()
			/ (L"sakura-agent-store-" + std::to_wstring(::GetCurrentProcessId())
				+ L"-" + std::to_wstring(::GetTickCount64()));
		folder = base / L"repo";
		std::filesystem::create_directories(folder);
		context.roots.claudeConfigDirectory = (base / L"claude").wstring();
		context.roots.codexHome = (base / L"codex").wstring();
		context.roots.piAgentDirectory = (base / L"pi").wstring();
		// Nothing in these tests may reach a real CLI, the shell, or a real process table.
		context.services.run = [this](const BoundedProcessRequest& request, HANDLE) {
			SeenRun seen{ request.ExecutablePath(), request.WorkingDirectory(), request.Arguments(),
				request.StandardInput(), request.TimeoutMilliseconds(), request.OutputObserver() != nullptr,
				request.StandardInputCloseEvent() != nullptr };
			if (request.OutputObserver() && !reply.empty()) {
				const std::span<const std::uint8_t> bytes(
					reinterpret_cast<const std::uint8_t*>(reply.data()), reply.size());
				(void)request.OutputObserver()->OnOutput(EBoundedProcessStream::StandardError, bytes);
				(void)request.OutputObserver()->OnOutput(EBoundedProcessStream::StandardOutput, bytes);
				seen.closeEventSignalledAfterReply = request.StandardInputCloseEvent() != nullptr
					&& ::WaitForSingleObject(request.StandardInputCloseEvent(), 0) == WAIT_OBJECT_0;
			}
			runs.push_back(std::move(seen));
			return BoundedProcessResult(runStatus);
		};
		context.services.moveToRecycleBin = [this](const std::vector<std::wstring>& paths) {
			recycled.push_back(paths);
			return recycleSucceeds;
		};
		context.services.isProcessRunning = [this](const std::uint32_t processId) {
			return processId == runningProcessId;
		};
	}

	void TearDown() override
	{
		std::error_code error;
		std::filesystem::remove_all(base, error);
	}

	[[nodiscard]] std::filesystem::path ClaudeProject() const
	{
		return std::filesystem::path(context.roots.claudeConfigDirectory) / L"projects"
			/ ClaudeCodeProjectDirectoryName(folder.wstring());
	}
	[[nodiscard]] std::filesystem::path ClaudeTranscript() const
	{
		return ClaudeProject() / (std::wstring(kClaudeId) + L".jsonl");
	}
	[[nodiscard]] std::filesystem::path PiFile() const
	{
		return std::filesystem::path(context.roots.piAgentDirectory) / L"sessions"
			/ PiSessionDirectoryName(folder.wstring()) / (L"2026-11-09T10-00-00-000Z_" + std::wstring(kPiId) + L".jsonl");
	}
	void WriteLiveSession(const std::wstring_view name, const std::string_view sessionId, const std::uint32_t pid) const
	{
		WriteText(std::filesystem::path(context.roots.claudeConfigDirectory) / L"sessions" / name,
			"{\"pid\":" + std::to_string(pid) + ",\"sessionId\":\"" + std::string(sessionId) + "\"}");
	}

	[[nodiscard]] Status Rename(const EAgentSessionProvider provider, const std::wstring_view id,
		const std::wstring_view title, const HANDLE stop = nullptr) const
	{
		return RenameAgentSession({ provider, std::wstring(id), L"", 0 }, folder.wstring(), title, context, stop);
	}
	[[nodiscard]] Status Delete(const EAgentSessionProvider provider, const std::wstring_view id,
		const HANDLE stop = nullptr) const
	{
		return DeleteAgentSession({ provider, std::wstring(id), L"", 0 }, folder.wstring(), context, stop);
	}

	std::filesystem::path base;
	std::filesystem::path folder;
	AgentSessionStoreContext context;
	std::vector<SeenRun> runs;
	std::vector<std::vector<std::wstring>> recycled;
	std::string reply;
	EBoundedProcessStatus runStatus{ EBoundedProcessStatus::Succeeded };
	bool recycleSucceeds = true;
	std::uint32_t runningProcessId = 4242;
};

using AgentSessionStoreRename = StoreFixture;
using AgentSessionStoreDelete = StoreFixture;

constexpr char kClaudeIdUtf8[] = "0f1e2d3c-4b5a-4968-8776-655443322110";
constexpr char kTranscript[] = "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"Hi\"}}\n";

} // namespace

TEST_F(AgentSessionStoreRename, ClaudeCodeAppendsTheTitleRecordAndRewritesTheSidecar)
{
	std::string cwd;
	for (const wchar_t character : folder.wstring()) {
		if (character == L'\\') cwd += "\\\\";
		else cwd.push_back(static_cast<char>(character));
	}
	const std::string kTranscript = "{\"type\":\"user\",\"isSidechain\":false,\"cwd\":\"" + cwd
		+ "\",\"message\":{\"role\":\"user\",\"content\":\"Explain the parser\"}}\n";
	WriteText(ClaudeTranscript(), kTranscript);
	const auto sidecar = ClaudeProject() / kClaudeId / L"custom-title.json";

	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"  Parser \"work\"\t\u65e5\u672c  "));
	const std::string first = std::string("{\"type\":\"custom-title\",\"customTitle\":\"Parser \\\"work\\\" \xE6\x97\xA5\xE6\x9C\xAC\",")
		+ "\"sessionId\":\"" + kClaudeIdUtf8 + "\"}\n";
	EXPECT_EQ(kTranscript + first, ReadText(ClaudeTranscript()));
	EXPECT_EQ("{\"customTitle\":\"Parser \\\"work\\\" \xE6\x97\xA5\xE6\x9C\xAC\"}", ReadText(sidecar));

	// A second rename appends again (the latest record wins) and replaces the sidecar.
	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Second"));
	const std::string second = std::string("{\"type\":\"custom-title\",\"customTitle\":\"Second\",\"sessionId\":\"")
		+ kClaudeIdUtf8 + "\"}\n";
	EXPECT_EQ(kTranscript + first + second, ReadText(ClaudeTranscript()));
	EXPECT_EQ("{\"customTitle\":\"Second\"}", ReadText(sidecar));
	EXPECT_FALSE(std::filesystem::exists(sidecar.wstring() + L".sakura-tmp"));

	// What was written is what discovery reads back.
	const auto sessions = DiscoverAgentSessions(folder.wstring(), context.roots, {}, nullptr);
	ASSERT_TRUE(sessions.has_value());
	ASSERT_EQ(1U, sessions->size());
	EXPECT_EQ(L"Second", (*sessions)[0].title);
	EXPECT_TRUE(runs.empty());
}

TEST_F(AgentSessionStoreRename, ClaudeCodeStartsTheRecordOnItsOwnLineAfterACutLastLine)
{
	const std::string cut = std::string(kTranscript) + "{\"type\":\"assist";
	WriteText(ClaudeTranscript(), cut);
	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Named"));
	EXPECT_EQ(cut + "\n{\"type\":\"custom-title\",\"customTitle\":\"Named\",\"sessionId\":\""
		+ kClaudeIdUtf8 + "\"}\n", ReadText(ClaudeTranscript()));
}

TEST_F(AgentSessionStoreRename, ClaudeCodeRefusesARunningSessionAndAMissingTranscript)
{
	EXPECT_EQ(Status::NotFound, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Named"));

	WriteText(ClaudeTranscript(), kTranscript);
	// Another session's live marker, a dead owner, and an unreadable marker do not block.
	WriteLiveSession(L"100.json", "0f1e2d3c-4b5a-4968-8776-000000000000", runningProcessId);
	WriteLiveSession(L"200.json", kClaudeIdUtf8, 777);
	WriteText(std::filesystem::path(context.roots.claudeConfigDirectory) / L"sessions" / L"300.json", "not json");
	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Named"));

	WriteLiveSession(L"4242.json", kClaudeIdUtf8, runningProcessId);
	const auto before = ReadText(ClaudeTranscript());
	EXPECT_EQ(Status::SessionRunning, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Other"));
	EXPECT_EQ(Status::SessionRunning, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId));
	EXPECT_EQ(before, ReadText(ClaudeTranscript()));
	EXPECT_TRUE(recycled.empty());
}

TEST_F(AgentSessionStoreRename, PiAppendsASessionInfoEntryLinkedToTheLastEntry)
{
	const std::string header = std::string("{\"type\":\"session\",\"version\":3,\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n");
	const std::string content = header
		+ "{\"type\":\"message\",\"id\":\"aaaaaaaa\",\"parentId\":null,\"message\":{\"role\":\"user\",\"content\":\"Port it\"}}\n"
		+ "{\"type\":\"message\",\"id\":\"bbbbbbbb\",\"parentId\":\"aaaaaaaa\"}\n";
	WriteText(PiFile(), content);

	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::Pi, kPiId, L"Port the parser"));
	const auto written = ReadText(PiFile());
	ASSERT_GT(written.size(), content.size());
	EXPECT_EQ(content, written.substr(0, content.size()));
	const auto record = written.substr(content.size());
	EXPECT_EQ('\n', record.back());
	EXPECT_EQ(record.size() - 1, record.find('\n'));
	EXPECT_EQ(0U, record.find("{\"type\":\"session_info\",\"id\":\""));
	EXPECT_NE(std::string::npos, record.find("\",\"parentId\":\"bbbbbbbb\",\"timestamp\":\"20"));
	EXPECT_NE(std::string::npos, record.find("Z\",\"name\":\"Port the parser\"}\n"));

	// The file is still one chain, now ending at the new 8-hex entry.
	const auto chain = ReadPiSessionChain(written, kPiId);
	ASSERT_TRUE(chain.has_value());
	ASSERT_EQ(3U, chain->entryIds.size());
	const auto& entryId = chain->entryIds.back();
	EXPECT_EQ(8U, entryId.size());
	EXPECT_EQ(std::string::npos, entryId.find_first_not_of("0123456789abcdef"));
	EXPECT_NE("aaaaaaaa", entryId);
	EXPECT_NE("bbbbbbbb", entryId);

	// A header-only session gets a root entry.
	WriteText(PiFile(), header);
	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::Pi, kPiId, L"Empty"));
	EXPECT_NE(std::string::npos, ReadText(PiFile()).find("\",\"parentId\":null,\"timestamp\":\""));
}

TEST_F(AgentSessionStoreRename, PiLeavesAFileItCannotChainUntouched)
{
	EXPECT_EQ(Status::NotFound, Rename(EAgentSessionProvider::Pi, kPiId, L"Named"));

	// An entry without an id (an older Pi format) and a cut last line are not a chain.
	for (const std::string& content : {
		std::string("{\"type\":\"session\",\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n{\"type\":\"message\"}\n"),
		std::string("{\"type\":\"session\",\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n{\"type\":\"mess"),
		std::string("{\"type\":\"session\",\"id\":\"another-session\"}\n") }) {
		WriteText(PiFile(), content);
		EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::Pi, kPiId, L"Named"));
		EXPECT_EQ(content, ReadText(PiFile()));
	}
}

TEST_F(AgentSessionStoreRename, CodexSendsThreadNameSetThroughTheAppServerAndTrustsItsReply)
{
	context.tools.codexCommand = L"C:\\Tools\\codex.exe";
	context.commandTimeoutMilliseconds = 1234;
	reply = "{\"id\":1,\"result\":{}}\n{\"method\":\"note\"}\n{\"id\":2,\"result\":{}}\n";
	// The reply decides, even when the server is then killed at the deadline.
	runStatus = EBoundedProcessStatus::TimedOut;

	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));
	ASSERT_EQ(1U, runs.size());
	EXPECT_EQ(L"C:\\Tools\\codex.exe", runs[0].executable);
	EXPECT_EQ(folder.wstring(), runs[0].workingDirectory);
	EXPECT_EQ((std::vector<std::wstring>{ L"app-server" }), runs[0].arguments);
	EXPECT_EQ(BuildCodexRenameRequests(kCodexId, L"Add a node"), std::optional<std::string>(runs[0].standardInput));
	EXPECT_EQ(1234U, runs[0].timeoutMilliseconds);
	EXPECT_TRUE(runs[0].hadObserver);
	EXPECT_TRUE(runs[0].hadCloseEvent);
	// Standard input stays open until thread/name/set has answered.
	EXPECT_TRUE(runs[0].closeEventSignalledAfterReply);

	reply = "{\"id\":1,\"result\":{}}\n{\"id\":2,\"error\":{\"code\":-32600,\"message\":\"no thread\"}}\n";
	runStatus = EBoundedProcessStatus::Succeeded;
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));

	// No reply: a clean exit is still a failure, and a cancel stays a cancel.
	reply = "{\"id\":1,\"result\":{}}\n";
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));
	EXPECT_FALSE(runs.back().closeEventSignalledAfterReply);
	runStatus = EBoundedProcessStatus::Cancelled;
	EXPECT_EQ(Status::Cancelled, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));
	runStatus = EBoundedProcessStatus::TimedOut;
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));
	runStatus = EBoundedProcessStatus::LaunchFailed;
	EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Codex, kCodexId, L"Add a node"));
}

TEST_F(AgentSessionStoreRename, CodexShimRunsThroughTheCommandProcessorOnlyWithASafePath)
{
	context.tools.codexCommand = L"C:\\Users\\dev\\AppData\\Roaming\\npm\\codex.CMD";
	context.tools.commandProcessor = L"C:\\Windows\\System32\\cmd.exe";
	reply = "{\"id\":2,\"result\":{}}\n";
	EXPECT_EQ(Status::Succeeded, Rename(EAgentSessionProvider::Codex, kCodexId, L"Named"));
	ASSERT_EQ(1U, runs.size());
	EXPECT_EQ(L"C:\\Windows\\System32\\cmd.exe", runs[0].executable);
	EXPECT_EQ((std::vector<std::wstring>{ L"/d", L"/c", context.tools.codexCommand, L"app-server" }),
		runs[0].arguments);

	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::Codex, kCodexId));
	ASSERT_EQ(2U, runs.size());
	EXPECT_EQ((std::vector<std::wstring>{ L"/d", L"/c", context.tools.codexCommand, L"delete", L"--force", kCodexId }),
		runs[1].arguments);
	runs.clear();

	// cmd.exe re-parses its command line, so a path it could interpret never reaches it.
	for (const wchar_t* unsafe : { L"C:\\Tools & Co\\codex.cmd", L"C:\\Tools\\%X%\\codex.cmd",
		L"C:\\Tools (x86)\\codex.bat", L"C:\\Tools\\a^b\\codex.cmd", L"C:\\Tools\\a!b\\codex.cmd" }) {
		context.tools.codexCommand = unsafe;
		EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Codex, kCodexId, L"Named")) << unsafe;
		EXPECT_EQ(Status::Unsupported, Delete(EAgentSessionProvider::Codex, kCodexId));
	}
	// Neither a missing or relative command processor nor a relative or missing codex is resolved.
	context.tools.codexCommand = L"C:\\Tools\\codex.cmd";
	context.tools.commandProcessor = L"cmd.exe";
	EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Codex, kCodexId, L"Named"));
	context.tools.commandProcessor.clear();
	EXPECT_EQ(Status::Unsupported, Delete(EAgentSessionProvider::Codex, kCodexId));
	context.tools.codexCommand = L"codex.exe";
	EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Codex, kCodexId, L"Named"));
	context.tools.codexCommand.clear();
	EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Codex, kCodexId, L"Named"));
	EXPECT_EQ(Status::Unsupported, Delete(EAgentSessionProvider::Codex, kCodexId));
	EXPECT_TRUE(runs.empty());
}

TEST_F(AgentSessionStoreRename, DevinIsUnsupportedAndInvalidInputNeverReachesAStore)
{
	context.roots.devinExecutable = L"C:\\Tools\\devin.exe";
	context.tools.codexCommand = L"C:\\Tools\\codex.exe";
	EXPECT_EQ(Status::Unsupported, Rename(EAgentSessionProvider::Devin, L"ninth-tamarind", L"Named"));

	WriteText(ClaudeTranscript(), kTranscript);
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L" \t\r\n "));
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId,
		std::wstring(kMaximumAgentSessionTitleLength + 1U, L'x')));
	EXPECT_EQ(Status::Failed, Rename(EAgentSessionProvider::Codex, L"a b", L"Named"));
	EXPECT_EQ(Status::Failed, Delete(EAgentSessionProvider::Codex, L"--all"));
	EXPECT_EQ(Status::Failed, RenameAgentSession({ EAgentSessionProvider::Codex, kCodexId, L"", 0 },
		L"repo", L"Named", context, nullptr));
	EXPECT_EQ(Status::Failed, DeleteAgentSession({ EAgentSessionProvider::Codex, kCodexId, L"", 0 },
		L"repo", context, nullptr));

	const HANDLE stopped = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
	ASSERT_NE(nullptr, stopped);
	EXPECT_EQ(Status::Cancelled, Rename(EAgentSessionProvider::ClaudeCode, kClaudeId, L"Named", stopped));
	EXPECT_EQ(Status::Cancelled, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId, stopped));
	::CloseHandle(stopped);

	EXPECT_EQ(kTranscript, ReadText(ClaudeTranscript()));
	EXPECT_TRUE(runs.empty());
	EXPECT_TRUE(recycled.empty());
}

TEST_F(AgentSessionStoreDelete, ClaudeCodeRecyclesTheTranscriptAndEveryDirectoryKeyedByTheSession)
{
	EXPECT_EQ(Status::NotFound, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId));
	EXPECT_TRUE(recycled.empty());

	WriteText(ClaudeTranscript(), kTranscript);
	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId));
	ASSERT_EQ(1U, recycled.size());
	EXPECT_EQ((std::vector<std::wstring>{ ClaudeTranscript().wstring() }), recycled[0]);

	const auto claude = std::filesystem::path(context.roots.claudeConfigDirectory);
	const auto sessionDirectory = ClaudeProject() / kClaudeId;
	const auto fileHistory = claude / L"file-history" / kClaudeId;
	const auto sessionEnv = claude / L"session-env" / kClaudeId;
	std::filesystem::create_directories(sessionDirectory / L"subagents");
	std::filesystem::create_directories(fileHistory);
	std::filesystem::create_directories(sessionEnv);
	// Another session's directories and a same-named file are not this session's.
	std::filesystem::create_directories(claude / L"file-history" / L"another-session");
	WriteText(claude / L"todos" / kClaudeId, "x");
	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId));
	ASSERT_EQ(2U, recycled.size());
	EXPECT_EQ((std::vector<std::wstring>{ ClaudeTranscript().wstring(), sessionDirectory.wstring(),
		fileHistory.wstring(), sessionEnv.wstring() }), recycled[1]);

	// A refused Recycle Bin move is a failure; nothing is deleted another way.
	recycleSucceeds = false;
	EXPECT_EQ(Status::Failed, Delete(EAgentSessionProvider::ClaudeCode, kClaudeId));
	EXPECT_TRUE(std::filesystem::exists(ClaudeTranscript()));
	EXPECT_TRUE(std::filesystem::exists(sessionDirectory / L"subagents"));
	EXPECT_TRUE(runs.empty());
}

TEST_F(AgentSessionStoreDelete, PiRecyclesTheSessionFileOfThisFolder)
{
	EXPECT_EQ(Status::NotFound, Delete(EAgentSessionProvider::Pi, kPiId));
	WriteText(PiFile(), "{\"type\":\"session\",\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n");
	WriteText(PiFile().parent_path() / L"2026-11-09T09-00-00-000Z_another-session.jsonl", "{}\n");
	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::Pi, kPiId));
	ASSERT_EQ(1U, recycled.size());
	EXPECT_EQ((std::vector<std::wstring>{ PiFile().wstring() }), recycled[0]);
	EXPECT_TRUE(std::filesystem::exists(PiFile()));
}

TEST_F(AgentSessionStoreDelete, CodexAndDevinDeleteThroughTheirOwnCli)
{
	context.tools.codexCommand = L"C:\\Tools\\codex.exe";
	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::Codex, kCodexId));
	ASSERT_EQ(1U, runs.size());
	EXPECT_EQ(L"C:\\Tools\\codex.exe", runs[0].executable);
	EXPECT_EQ(folder.wstring(), runs[0].workingDirectory);
	EXPECT_EQ((std::vector<std::wstring>{ L"delete", L"--force", kCodexId }), runs[0].arguments);
	EXPECT_TRUE(runs[0].standardInput.empty());
	EXPECT_FALSE(runs[0].hadCloseEvent);
	EXPECT_EQ(30000U, runs[0].timeoutMilliseconds);

	// Devin is unsupported until discovery resolved an absolute executable.
	EXPECT_EQ(Status::Unsupported, Delete(EAgentSessionProvider::Devin, L"ninth-tamarind"));
	context.roots.devinExecutable = L"devin.exe";
	EXPECT_EQ(Status::Unsupported, Delete(EAgentSessionProvider::Devin, L"ninth-tamarind"));
	ASSERT_EQ(1U, runs.size());
	context.roots.devinExecutable = L"C:\\Tools\\devin.exe";
	EXPECT_EQ(Status::Succeeded, Delete(EAgentSessionProvider::Devin, L"ninth-tamarind"));
	ASSERT_EQ(2U, runs.size());
	EXPECT_EQ(L"C:\\Tools\\devin.exe", runs[1].executable);
	EXPECT_EQ((std::vector<std::wstring>{ L"rm", L"ninth-tamarind", L"--force" }), runs[1].arguments);

	for (const auto& [process, expected] : {
		std::pair{ EBoundedProcessStatus::Failed, Status::Failed },
		std::pair{ EBoundedProcessStatus::TimedOut, Status::Failed },
		std::pair{ EBoundedProcessStatus::OutputLimitExceeded, Status::Failed },
		std::pair{ EBoundedProcessStatus::Cancelled, Status::Cancelled },
		std::pair{ EBoundedProcessStatus::LaunchFailed, Status::Unsupported },
		std::pair{ EBoundedProcessStatus::InvalidRequest, Status::Unsupported } }) {
		runStatus = process;
		EXPECT_EQ(expected, Delete(EAgentSessionProvider::Codex, kCodexId));
		EXPECT_EQ(expected, Delete(EAgentSessionProvider::Devin, L"ninth-tamarind"));
	}
	EXPECT_TRUE(recycled.empty());
}
