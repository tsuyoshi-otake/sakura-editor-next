/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/projects/AgentSessionDiscovery.h"
#include "workbench/projects/AgentSessionHistory.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace workbench::projects;

constexpr wchar_t kClaudeId[] = L"0f1e2d3c-4b5a-4968-8776-655443322110";
constexpr wchar_t kCodexId[] = L"019a0000-0000-7000-8000-000000000001";
constexpr wchar_t kPiId[] = L"019a1111-2222-7333-8444-555566667777";

std::string Utf8(const std::wstring_view text)
{
	if (text.empty()) return {};
	const int length = static_cast<int>(text.size());
	const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<std::size_t>(required), '\0');
	(void)::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), required, nullptr, nullptr);
	return result;
}

//! A Windows path as the body of a JSON string literal.
std::string JsonPath(const std::wstring_view path)
{
	std::string result;
	for (const char character : Utf8(path)) {
		if (character == '\\') result += "\\\\";
		else result.push_back(character);
	}
	return result;
}

void WriteFile(const std::filesystem::path& path, const std::string_view content, const std::int64_t unixSeconds)
{
	std::filesystem::create_directories(path.parent_path());
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(stream.good()) << path.string();
		stream.write(content.data(), static_cast<std::streamsize>(content.size()));
	}
	const HANDLE file = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(INVALID_HANDLE_VALUE, file);
	ULARGE_INTEGER value{};
	value.QuadPart = static_cast<std::uint64_t>(unixSeconds) * 10000000ULL + 116444736000000000ULL;
	FILETIME time{};
	time.dwLowDateTime = value.LowPart;
	time.dwHighDateTime = value.HighPart;
	EXPECT_NE(FALSE, ::SetFileTime(file, nullptr, nullptr, &time));
	::CloseHandle(file);
}

class TemporaryAgentHomes final {
public:
	TemporaryAgentHomes()
	{
		base = std::filesystem::temp_directory_path()
			/ (L"sakura-agent-sessions-" + std::to_wstring(::GetCurrentProcessId())
				+ L"-" + std::to_wstring(::GetTickCount64()));
		folder = base / L"repo";
		std::filesystem::create_directories(folder);
		roots.claudeConfigDirectory = (base / L"claude").wstring();
		roots.codexHome = (base / L"codex").wstring();
		roots.piAgentDirectory = (base / L"pi").wstring();
	}

	~TemporaryAgentHomes()
	{
		std::error_code error;
		std::filesystem::remove_all(base, error);
	}

	[[nodiscard]] std::filesystem::path ClaudeProject() const
	{
		return std::filesystem::path(roots.claudeConfigDirectory) / L"projects"
			/ ClaudeCodeProjectDirectoryName(folder.wstring());
	}

	[[nodiscard]] std::filesystem::path PiSessions() const
	{
		return std::filesystem::path(roots.piAgentDirectory) / L"sessions"
			/ PiSessionDirectoryName(folder.wstring());
	}

	[[nodiscard]] std::filesystem::path CodexDay() const
	{
		return std::filesystem::path(roots.codexHome) / L"sessions" / L"2026" / L"11" / L"09";
	}

	std::filesystem::path base;
	std::filesystem::path folder;
	AgentSessionDiscoveryRoots roots;
};

std::string ClaudeTranscript(const std::wstring_view cwd, const std::string_view title)
{
	const auto path = JsonPath(cwd);
	return R"({"type":"user","isSidechain":false,"cwd":")" + path
		+ R"(","message":{"role":"user","content":"Explain the parser"}})" "\n"
		+ R"({"type":"ai-title","aiTitle":")" + std::string(title) + R"("})" "\n";
}

std::string CodexMeta(const std::wstring_view id, const std::wstring_view cwd,
	const std::string_view threadSource)
{
	return R"({"timestamp":"2026-11-09T10:00:00Z","type":"session_meta","payload":{"id":")"
		+ Utf8(id) + R"(","cwd":")" + JsonPath(cwd) + R"(","thread_source":")"
		+ std::string(threadSource) + R"(","source":"cli"}})" "\n";
}

std::string CodexRollout(const std::wstring_view id, const std::wstring_view cwd,
	const std::string_view threadSource)
{
	return CodexMeta(id, cwd, threadSource)
		+ R"({"type":"event_msg","payload":{"type":"user_message","message":"Add a sessions node"}})" "\n";
}

std::string PiSession(const std::wstring_view id, const std::wstring_view cwd, const std::string_view name)
{
	return R"({"type":"session","version":3,"id":")" + Utf8(id) + R"(","cwd":")" + JsonPath(cwd)
		+ R"("})" "\n"
		+ R"({"type":"message","message":{"role":"user","content":[{"type":"text","text":"Port it"}]}})" "\n"
		+ R"({"type":"session_info","name":")" + std::string(name) + R"("})" "\n";
}

class ManualResetEvent final {
public:
	explicit ManualResetEvent(const bool signalled)
		: handle(::CreateEventW(nullptr, TRUE, signalled ? TRUE : FALSE, nullptr))
	{
	}
	~ManualResetEvent() { if (handle != nullptr) ::CloseHandle(handle); }
	ManualResetEvent(const ManualResetEvent&) = delete;
	ManualResetEvent& operator=(const ManualResetEvent&) = delete;

	HANDLE handle;
};

} // namespace

TEST(AgentSessionHistory, ResumeCommandsAreExactForEveryProviderAndRejectUnsafeIds)
{
	EXPECT_EQ(std::optional<std::wstring>(std::wstring(L"claude --resume ") + kClaudeId),
		BuildAgentSessionResumeCommand(EAgentSessionProvider::ClaudeCode, kClaudeId));
	EXPECT_EQ(std::optional<std::wstring>(std::wstring(L"codex resume ") + kCodexId),
		BuildAgentSessionResumeCommand(EAgentSessionProvider::Codex, kCodexId));
	EXPECT_EQ(std::optional<std::wstring>(L"devin -r ninth-tamarind"),
		BuildAgentSessionResumeCommand(EAgentSessionProvider::Devin, L"ninth-tamarind"));
	EXPECT_EQ(std::optional<std::wstring>(std::wstring(L"pi --session ") + kPiId),
		BuildAgentSessionResumeCommand(EAgentSessionProvider::Pi, kPiId));

	// Nothing a shell could interpret reaches the command line.
	for (const std::wstring_view id : { std::wstring_view(L""), std::wstring_view(L"a;b"),
		std::wstring_view(L"a b"), std::wstring_view(L"-flag"), std::wstring_view(L"_x"),
		std::wstring_view(L"a&b"), std::wstring_view(L"a\"b"), std::wstring_view(L"a|b"),
		std::wstring_view(L"a\rb"), std::wstring_view(L"a\u00e9") }) {
		EXPECT_FALSE(IsValidAgentSessionId(id));
		EXPECT_FALSE(BuildAgentSessionResumeCommand(EAgentSessionProvider::ClaudeCode, id).has_value());
	}
	EXPECT_TRUE(IsValidAgentSessionId(std::wstring(kMaximumAgentSessionIdLength, L'a')));
	EXPECT_FALSE(IsValidAgentSessionId(std::wstring(kMaximumAgentSessionIdLength + 1U, L'a')));

	EXPECT_EQ(std::wstring(L"claude-code:") + kClaudeId,
		AgentSessionIdentity({ EAgentSessionProvider::ClaudeCode, kClaudeId }));
	EXPECT_EQ(L"codex:x", AgentSessionIdentity({ EAgentSessionProvider::Codex, L"x" }));
	EXPECT_EQ(L"devin:x", AgentSessionIdentity({ EAgentSessionProvider::Devin, L"x" }));
	EXPECT_EQ(L"pi:x", AgentSessionIdentity({ EAgentSessionProvider::Pi, L"x" }));
	EXPECT_EQ(L"Claude Code", AgentSessionProviderDisplayName(EAgentSessionProvider::ClaudeCode));
	EXPECT_EQ(L"Pi", AgentSessionProviderDisplayName(EAgentSessionProvider::Pi));
}

TEST(AgentSessionHistory, FolderIdentityIgnoresCaseSeparatorsAndTrailingSeparators)
{
	EXPECT_TRUE(IsSameAgentSessionFolder(L"C:\\Repo\\", L"c:/repo"));
	EXPECT_TRUE(IsSameAgentSessionFolder(L"C:\\Repo", L"C:\\Repo\\\\"));
	EXPECT_TRUE(IsSameAgentSessionFolder(L"C:\\", L"c:/"));
	EXPECT_FALSE(IsSameAgentSessionFolder(L"C:\\", L"C:"));
	EXPECT_FALSE(IsSameAgentSessionFolder(L"C:\\repo", L"C:\\repo2"));
	EXPECT_FALSE(IsSameAgentSessionFolder(L"C:\\repo", L"D:\\repo"));
	EXPECT_FALSE(IsSameAgentSessionFolder(L"", L""));
}

TEST(AgentSessionHistory, DirectoryNamesMatchTheClaudeCodeAndPiLayouts)
{
	EXPECT_EQ(L"C--Work-example-org-sample-project",
		ClaudeCodeProjectDirectoryName(L"C:\\Work\\example-org\\sample-project"));
	EXPECT_EQ(L"C--Work-example-org-sample-project",
		ClaudeCodeProjectDirectoryName(L"C:\\Work\\example-org\\sample-project\\"));
	EXPECT_EQ(L"C--a-b-c", ClaudeCodeProjectDirectoryName(L"C:\\a.b_c"));
	EXPECT_EQ(L"--C--Users-someone--", PiSessionDirectoryName(L"C:\\Users\\someone"));
	EXPECT_EQ(L"--C--Users-someone--", PiSessionDirectoryName(L"C:/Users/someone/"));
}

TEST(AgentSessionHistory, TitlesCollapseWhitespaceAndStayWithinTheBound)
{
	EXPECT_EQ(L"fix login bug", NormalizeAgentSessionTitle(L"  fix\t\tlogin\r\nbug  "));
	EXPECT_TRUE(NormalizeAgentSessionTitle(L" \t\r\n").empty());

	const std::wstring exact(kMaximumAgentSessionTitleLength, L'a');
	EXPECT_EQ(exact, NormalizeAgentSessionTitle(exact));

	const auto longTitle = NormalizeAgentSessionTitle(std::wstring(200, L'a'));
	ASSERT_EQ(kMaximumAgentSessionTitleLength, longTitle.size());
	EXPECT_EQ(L'\u2026', longTitle.back());
	EXPECT_EQ(std::wstring(kMaximumAgentSessionTitleLength - 1U, L'a'),
		longTitle.substr(0, kMaximumAgentSessionTitleLength - 1U));

	// A cut inside a surrogate pair drops the orphaned high surrogate.
	const auto pair = NormalizeAgentSessionTitle(std::wstring(158, L'a') + L"\xD83D\xDE00" + L"bbb");
	ASSERT_EQ(159U, pair.size());
	EXPECT_EQ(L'a', pair[157]);
	EXPECT_EQ(L'\u2026', pair.back());

	// A cut at a word boundary leaves no space before the ellipsis.
	const auto word = NormalizeAgentSessionTitle(std::wstring(158, L'a') + L" bbbb");
	EXPECT_EQ(std::wstring(158, L'a') + L"\u2026", word);
}

TEST(AgentSessionHistory, ClaudeTranscriptTitlePrecedenceAndPromptFiltering)
{
	const std::string prompts =
		R"({"type":"user","isMeta":true,"isSidechain":false,"cwd":"C:\\repo","message":{"role":"user","content":"Caveat: local commands"}})" "\n"
		R"({"type":"user","cwd":"C:\\repo","message":{"role":"user","content":"<command-name>/clear</command-name>"}})" "\n"
		R"({"type":"user","cwd":"C:\\repo","message":{"role":"user","content":[{"type":"tool_result","content":"x"}]}})" "\n"
		R"({"type":"user","cwd":"C:\\repo","message":{"role":"user","content":[{"type":"text","text":"  Fix the\nlogin bug"}]}})" "\n"
		R"({"type":"assistant","cwd":"C:\\repo","message":{"role":"assistant","content":[{"type":"text","text":"ok"}]}})" "\n";

	const auto prompt = ParseClaudeCodeTranscript(prompts, {}, kClaudeId, L"c:/repo/", 1234);
	ASSERT_TRUE(prompt.has_value());
	EXPECT_EQ(EAgentSessionProvider::ClaudeCode, prompt->provider);
	EXPECT_EQ(kClaudeId, prompt->id);
	EXPECT_EQ(L"Fix the login bug", prompt->title);
	EXPECT_EQ(1234, prompt->lastActivityUnixSeconds);

	// A line cut by the tail slice's edge is ignored, not an error.
	const std::string summary = "ary\":\"cut\"}\n" R"({"type":"summary","summary":"Summarized"})" "\n";
	EXPECT_EQ(L"Summarized", ParseClaudeCodeTranscript(prompts, summary, kClaudeId, L"C:\\repo", 0)->title);

	const std::string aiTitles = summary
		+ R"({"type":"ai-title","aiTitle":"First AI title"})" "\n"
		+ R"({"type":"ai-title","aiTitle":"Latest AI title"})" "\n";
	EXPECT_EQ(L"Latest AI title", ParseClaudeCodeTranscript(prompts, aiTitles, kClaudeId, L"C:\\repo", 0)->title);

	const std::string custom = aiTitles
		+ R"({"type":"custom-title","customTitle":"Renamed by user"})" "\n"
		+ R"({"type":"custom-title","customTitle":""})" "\n";
	EXPECT_EQ(L"Renamed by user", ParseClaudeCodeTranscript(prompts, custom, kClaudeId, L"C:\\repo", 0)->title);

	// Another folder, no cwd at all, an unsafe stem, or a sub-agent transcript.
	EXPECT_FALSE(ParseClaudeCodeTranscript(prompts, {}, kClaudeId, L"C:\\other", 0).has_value());
	EXPECT_FALSE(ParseClaudeCodeTranscript(R"({"type":"summary","summary":"x"})", {}, kClaudeId,
		L"C:\\repo", 0).has_value());
	EXPECT_FALSE(ParseClaudeCodeTranscript(prompts, {}, L"agent;rm", L"C:\\repo", 0).has_value());
	const std::string sidechain =
		R"({"type":"user","isSidechain":true,"cwd":"C:\\repo","message":{"role":"user","content":"Search the tree"}})" "\n";
	EXPECT_FALSE(ParseClaudeCodeTranscript(sidechain, {}, L"agent-a13e19439a8862a6a", L"C:\\repo", 0).has_value());

	// A sidechain prompt inside a main transcript is not its title.
	const std::string mixed =
		R"({"type":"user","isSidechain":false,"cwd":"C:\\repo","message":{"role":"user","content":"<system-reminder>x</system-reminder>"}})" "\n"
		R"({"type":"user","isSidechain":true,"cwd":"C:\\repo","message":{"role":"user","content":"Sub-agent task"}})" "\n"
		R"({"type":"user","isSidechain":false,"cwd":"C:\\repo","message":{"role":"user","content":"Real prompt"}})" "\n";
	EXPECT_EQ(L"Real prompt", ParseClaudeCodeTranscript(mixed, {}, kClaudeId, L"C:\\repo", 0)->title);
}

TEST(AgentSessionHistory, CodexIndexAndRolloutAcceptOnlyUserThreadsForTheFolder)
{
	const auto id = Utf8(kCodexId);
	const std::string index =
		R"({"id":")" + id + R"(","thread_name":"Old name","updated_at":"2026-11-09T10:00:00Z"})" "\n"
		+ R"({"id":")" + id + R"(","thread_name":"Renamed  thread"})" "\n"
		+ R"({"id":"019a0000-0000-7000-8000-000000000002","thread_name":"Gone"})" "\n"
		+ R"({"id":"019a0000-0000-7000-8000-000000000002","thread_name":""})" "\n"
		+ R"({"id":"bad id","thread_name":"x"})" "\n"
		+ "not json\n";
	const auto names = ParseCodexSessionIndex(index);
	ASSERT_EQ(1U, names.size());
	EXPECT_EQ(L"Renamed thread", names.at(kCodexId));

	const auto rollout = CodexRollout(kCodexId, L"C:\\repo", "user");
	const auto named = ParseCodexRollout(rollout, L"C:\\repo\\", 77, names);
	ASSERT_TRUE(named.has_value());
	EXPECT_EQ(EAgentSessionProvider::Codex, named->provider);
	EXPECT_EQ(kCodexId, named->id);
	EXPECT_EQ(L"Renamed thread", named->title);
	EXPECT_EQ(77, named->lastActivityUnixSeconds);

	// Without an index name, the first real user message is the title.
	const std::string withContext = CodexMeta(kCodexId, L"C:\\repo", "user")
		+ R"({"type":"event_msg","payload":{"type":"user_message","message":"<environment_context>x</environment_context>"}})" "\n"
		+ R"({"type":"event_msg","payload":{"type":"user_message","message":"Add a sessions node"}})" "\n";
	EXPECT_EQ(L"Add a sessions node", ParseCodexRollout(withContext, L"C:\\repo", 0, {})->title);

	// Sub-agent threads, another folder, and a non-meta first line are rejected.
	EXPECT_FALSE(ParseCodexRollout(CodexRollout(kCodexId, L"C:\\repo", "subagent"), L"C:\\repo", 0, {}).has_value());
	const std::string subagentSource =
		R"({"type":"session_meta","payload":{"id":")" + id
		+ R"(","cwd":"C:\\repo","thread_source":"user","source":{"subagent":{"parent_thread_id":"x"}}}})" "\n";
	EXPECT_FALSE(ParseCodexRollout(subagentSource, L"C:\\repo", 0, {}).has_value());
	EXPECT_FALSE(ParseCodexRollout(rollout, L"C:\\other", 0, names).has_value());
	EXPECT_FALSE(ParseCodexRollout(R"({"type":"event_msg","payload":{"type":"user_message","message":"x"}})" "\n"
		+ rollout, L"C:\\repo", 0, names).has_value());
}

TEST(AgentSessionHistory, PiSessionUsesTheLatestNameAndAnEmptyNameFallsBackToThePrompt)
{
	const auto head = PiSession(kPiId, L"C:\\repo", "Parser port");
	const auto named = ParsePiSession(head, {}, L"c:\\REPO", 55);
	ASSERT_TRUE(named.has_value());
	EXPECT_EQ(EAgentSessionProvider::Pi, named->provider);
	EXPECT_EQ(kPiId, named->id);
	EXPECT_EQ(L"Parser port", named->title);
	EXPECT_EQ(55, named->lastActivityUnixSeconds);

	EXPECT_EQ(L"Port it", ParsePiSession(head, R"({"type":"session_info","name":""})" "\n",
		L"C:\\repo", 0)->title);
	EXPECT_EQ(L"Final", ParsePiSession(head, R"({"type":"session_info","name":""})" "\n"
		R"({"type":"session_info","name":"Final"})" "\n", L"C:\\repo", 0)->title);

	EXPECT_FALSE(ParsePiSession(head, {}, L"C:\\other", 0).has_value());
	EXPECT_FALSE(ParsePiSession(R"({"type":"message","id":"x"})" "\n" + head, {}, L"C:\\repo", 0).has_value());
	EXPECT_FALSE(ParsePiSession(PiSession(L"bad id", L"C:\\repo", "x"), {}, L"C:\\repo", 0).has_value());
}

TEST(AgentSessionHistory, DevinListKeepsValidSessionsForTheFolder)
{
	const std::string json = R"([
		{"id":"ninth-tamarind","title":"Refactor  the\nparser","working_directory":"C:\\repo","last_activity_at":"1762650000"},
		{"id":"other-folder","title":"x","working_directory":"D:\\other","last_activity_at":"1"},
		{"id":"bad id","title":"x","working_directory":"C:\\repo"},
		{"id":"numeric-time","title":"","working_directory":"c:/repo/","last_activity_at":1762640000},
		{"id":"garbage-time","working_directory":"C:\\repo","last_activity_at":"12abc"},
		{"id":"word-time","working_directory":"C:\\repo","last_activity_at":"soon"},
		42
	])";
	const auto sessions = ParseDevinSessionList(json, L"C:\\repo");
	ASSERT_EQ(4U, sessions.size());
	EXPECT_EQ(EAgentSessionProvider::Devin, sessions[0].provider);
	EXPECT_EQ(L"ninth-tamarind", sessions[0].id);
	EXPECT_EQ(L"Refactor the parser", sessions[0].title);
	EXPECT_EQ(1762650000, sessions[0].lastActivityUnixSeconds);
	EXPECT_EQ(L"numeric-time", sessions[1].id);
	EXPECT_TRUE(sessions[1].title.empty());
	EXPECT_EQ(1762640000, sessions[1].lastActivityUnixSeconds);
	EXPECT_EQ(0, sessions[2].lastActivityUnixSeconds);
	EXPECT_EQ(0, sessions[3].lastActivityUnixSeconds);

	EXPECT_TRUE(ParseDevinSessionList(R"({"id":"x"})", L"C:\\repo").empty());
	EXPECT_TRUE(ParseDevinSessionList("[", L"C:\\repo").empty());
	EXPECT_TRUE(ParseDevinSessionList("", L"C:\\repo").empty());
}

// #410: `devin list --format json` (3000.12.17) pretty-prints the whole array
// and ends it with a newline. That newline is JSON whitespace, not trailing
// content, so the list must still parse; any other trailing byte rejects it.
TEST(AgentSessionHistory, DevinListAcceptsTheCliTrailingNewline)
{
	const std::string body = "[\n  {\n    \"id\": \"sweltering-grade\",\n"
		"    \"working_directory\": \"C:\\\\repo\",\n"
		"    \"last_activity_at\": 1794275330,\n"
		"    \"title\": \"Hello\"\n  }\n]";
	for (const std::string_view ending : { "\n", "\r\n", " \n\t\r\n" }) {
		const auto sessions = ParseDevinSessionList(body + std::string(ending), L"C:\\repo");
		ASSERT_EQ(1U, sessions.size());
		EXPECT_EQ(L"sweltering-grade", sessions[0].id);
		EXPECT_EQ(L"Hello", sessions[0].title);
		EXPECT_EQ(1794275330, sessions[0].lastActivityUnixSeconds);
	}
	EXPECT_TRUE(ParseDevinSessionList(body + "\nx", L"C:\\repo").empty());
	EXPECT_TRUE(ParseDevinSessionList(body + "\n[]", L"C:\\repo").empty());
}

TEST(AgentSessionHistory, MergeOrdersNewestFirstWithUnknownTimesLastAndRemovesDuplicates)
{
	std::vector<AgentSessionRecord> sessions{
		{ EAgentSessionProvider::Codex, L"b", L"", 100 },
		{ EAgentSessionProvider::ClaudeCode, L"a", L"", 0 },
		{ EAgentSessionProvider::ClaudeCode, L"c", L"", 200 },
		{ EAgentSessionProvider::Codex, L"b", L"older copy", 50 },
		{ EAgentSessionProvider::Pi, L"d", L"", 100 },
		{ EAgentSessionProvider::ClaudeCode, L"e", L"", 100 },
	};
	const auto merged = MergeAgentSessions(sessions, 10);
	ASSERT_EQ(5U, merged.size());
	EXPECT_EQ(L"c", merged[0].id);
	EXPECT_EQ(L"e", merged[1].id);
	EXPECT_EQ(L"b", merged[2].id);
	EXPECT_EQ(100, merged[2].lastActivityUnixSeconds);
	EXPECT_EQ(L"d", merged[3].id);
	EXPECT_EQ(L"a", merged[4].id);

	const auto bounded = MergeAgentSessions(sessions, 3);
	ASSERT_EQ(3U, bounded.size());
	EXPECT_EQ(L"b", bounded[2].id);
	EXPECT_TRUE(MergeAgentSessions(sessions, 0).empty());
}

TEST(AgentSessionHistory, AgeTextsAreRelativeAndEmptyWhenUnknown)
{
	constexpr std::int64_t now = 1'762'650'000;
	EXPECT_TRUE(FormatAgentSessionAge(0, now).empty());
	EXPECT_EQ(L"now", FormatAgentSessionAge(now + 30, now));
	EXPECT_EQ(L"now", FormatAgentSessionAge(now - 59, now));
	EXPECT_EQ(L"1m", FormatAgentSessionAge(now - 60, now));
	EXPECT_EQ(L"59m", FormatAgentSessionAge(now - 3599, now));
	EXPECT_EQ(L"1h", FormatAgentSessionAge(now - 3600, now));
	EXPECT_EQ(L"23h", FormatAgentSessionAge(now - 86399, now));
	EXPECT_EQ(L"1d", FormatAgentSessionAge(now - 86400, now));
	EXPECT_EQ(L"30d", FormatAgentSessionAge(now - 30 * 86400, now));

	const AgentSessionAgeTexts texts{ L"just now", L"{0} min ago", L"{0} hr ago", L"{0} days ago" };
	EXPECT_EQ(L"just now", FormatAgentSessionAge(now, now, texts));
	EXPECT_EQ(L"5 min ago", FormatAgentSessionAge(now - 300, now, texts));
	EXPECT_EQ(L"2 hr ago", FormatAgentSessionAge(now - 7200, now, texts));
	EXPECT_EQ(L"3 days ago", FormatAgentSessionAge(now - 3 * 86400, now, texts));
}

TEST(AgentSessionDiscovery, FindsEveryFileProviderSessionForTheFolderOnly)
{
	TemporaryAgentHomes homes;
	const auto folder = homes.folder.wstring();
	const auto otherFolder = (homes.base / L"other").wstring();

	// Claude Code: one session for the folder; a transcript of another folder
	// that happens to share the directory, a non-transcript file, and a
	// sub-agent directory are not sessions.
	WriteFile(homes.ClaudeProject() / (std::wstring(kClaudeId) + L".jsonl"),
		ClaudeTranscript(folder, "Claude title"), 1'000);
	WriteFile(homes.ClaudeProject() / L"1f1e2d3c-4b5a-4968-8776-655443322110.jsonl",
		ClaudeTranscript(otherFolder, "Elsewhere"), 2'000);
	WriteFile(homes.ClaudeProject() / L"notes.txt", "{}", 2'500);
	WriteFile(homes.ClaudeProject() / kClaudeId / L"subagents" / L"agent-a1.jsonl",
		ClaudeTranscript(folder, "Sub-agent"), 2'600);

	// Codex: a named user thread, a sub-agent thread, and another folder's thread.
	WriteFile(std::filesystem::path(homes.roots.codexHome) / L"session_index.jsonl",
		R"({"id":")" + Utf8(kCodexId) + R"(","thread_name":"Codex named"})" "\n", 3'000);
	WriteFile(homes.CodexDay() / (std::wstring(L"rollout-2026-11-09T10-00-00-") + kCodexId + L".jsonl"),
		CodexRollout(kCodexId, folder, "user"), 3'000);
	WriteFile(homes.CodexDay() / L"rollout-2026-11-09T11-00-00-019a0000-0000-7000-8000-000000000004.jsonl",
		CodexRollout(L"019a0000-0000-7000-8000-000000000004", folder, "subagent"), 4'000);
	WriteFile(homes.CodexDay() / L"rollout-2026-11-09T12-00-00-019a0000-0000-7000-8000-000000000005.jsonl",
		CodexRollout(L"019a0000-0000-7000-8000-000000000005", otherFolder, "user"), 5'000);

	// Pi: one named session for the folder.
	WriteFile(homes.PiSessions() / (std::wstring(L"2026-11-09T10-00-00-000Z_") + kPiId + L".jsonl"),
		PiSession(kPiId, folder, "Pi named"), 500);

	ManualResetEvent stop(false);
	ASSERT_NE(nullptr, stop.handle);
	const auto sessions = DiscoverAgentSessions(folder, homes.roots, {}, stop.handle);
	ASSERT_TRUE(sessions.has_value());
	ASSERT_EQ(3U, sessions->size());
	EXPECT_EQ(EAgentSessionProvider::Codex, (*sessions)[0].provider);
	EXPECT_EQ(kCodexId, (*sessions)[0].id);
	EXPECT_EQ(L"Codex named", (*sessions)[0].title);
	EXPECT_EQ(3'000, (*sessions)[0].lastActivityUnixSeconds);
	EXPECT_EQ(EAgentSessionProvider::ClaudeCode, (*sessions)[1].provider);
	EXPECT_EQ(kClaudeId, (*sessions)[1].id);
	EXPECT_EQ(L"Claude title", (*sessions)[1].title);
	EXPECT_EQ(1'000, (*sessions)[1].lastActivityUnixSeconds);
	EXPECT_EQ(EAgentSessionProvider::Pi, (*sessions)[2].provider);
	EXPECT_EQ(kPiId, (*sessions)[2].id);
	EXPECT_EQ(L"Pi named", (*sessions)[2].title);

	// The folder is matched by identity, not spelling.
	const auto respelled = DiscoverAgentSessions(folder + L"\\", homes.roots, {}, nullptr);
	ASSERT_TRUE(respelled.has_value());
	EXPECT_EQ(3U, respelled->size());
}

TEST(AgentSessionDiscovery, BoundsSessionsPerProviderAndOverallAndReadsLargeFilesBySlices)
{
	TemporaryAgentHomes homes;
	const auto folder = homes.folder.wstring();
	WriteFile(homes.ClaudeProject() / L"00000000-0000-4000-8000-000000000001.jsonl",
		ClaudeTranscript(folder, "Older"), 1'000);
	WriteFile(homes.ClaudeProject() / L"00000000-0000-4000-8000-000000000002.jsonl",
		ClaudeTranscript(folder, "Newer"), 2'000);
	// A transcript far larger than head + tail: its cwd is only in the head and
	// its user-assigned title only in the tail.
	std::string large = ClaudeTranscript(folder, "Head title");
	const std::string filler = R"({"type":"assistant","message":{"content":"filler filler filler filler"}})" "\n";
	while (large.size() < 64u * 1024u) large += filler;
	large += R"({"type":"custom-title","customTitle":"Tail title"})" "\n";
	WriteFile(homes.ClaudeProject() / L"00000000-0000-4000-8000-000000000003.jsonl", large, 3'000);
	WriteFile(homes.PiSessions() / (std::wstring(kPiId) + L".jsonl"), PiSession(kPiId, folder, "Pi"), 2'500);

	AgentSessionDiscoveryLimits limits;
	limits.headBytes = 1024;
	limits.tailBytes = 1024;
	const auto all = DiscoverAgentSessions(folder, homes.roots, limits, nullptr);
	ASSERT_TRUE(all.has_value());
	ASSERT_EQ(4U, all->size());
	EXPECT_EQ(L"Tail title", (*all)[0].title);
	EXPECT_EQ(L"Pi", (*all)[1].title);
	EXPECT_EQ(L"Newer", (*all)[2].title);
	EXPECT_EQ(L"Older", (*all)[3].title);

	limits.maximumSessionsPerProvider = 2;
	const auto perProvider = DiscoverAgentSessions(folder, homes.roots, limits, nullptr);
	ASSERT_TRUE(perProvider.has_value());
	ASSERT_EQ(3U, perProvider->size());
	EXPECT_EQ(L"Tail title", (*perProvider)[0].title);
	EXPECT_EQ(L"Pi", (*perProvider)[1].title);
	EXPECT_EQ(L"Newer", (*perProvider)[2].title);

	limits.maximumSessions = 1;
	const auto overall = DiscoverAgentSessions(folder, homes.roots, limits, nullptr);
	ASSERT_TRUE(overall.has_value());
	ASSERT_EQ(1U, overall->size());
	EXPECT_EQ(L"Tail title", (*overall)[0].title);

	// The read budget counts files actually opened, newest first.
	limits = {};
	limits.maximumFileReadsPerProvider = 1;
	limits.headBytes = 1024;
	limits.tailBytes = 1024;
	const auto reads = DiscoverAgentSessions(folder, homes.roots, limits, nullptr);
	ASSERT_TRUE(reads.has_value());
	ASSERT_EQ(2U, reads->size());
	EXPECT_EQ(L"Tail title", (*reads)[0].title);
	EXPECT_EQ(L"Pi", (*reads)[1].title);
}

TEST(AgentSessionDiscovery, StopDiscardsResultsAndMissingRootsContributeNothing)
{
	TemporaryAgentHomes homes;
	const auto folder = homes.folder.wstring();
	WriteFile(homes.ClaudeProject() / (std::wstring(kClaudeId) + L".jsonl"),
		ClaudeTranscript(folder, "Claude title"), 1'000);

	ManualResetEvent stopped(true);
	ASSERT_NE(nullptr, stopped.handle);
	EXPECT_FALSE(DiscoverAgentSessions(folder, homes.roots, {}, stopped.handle).has_value());

	// No roots at all, missing directories, and a relative folder are empty
	// results, never a failure.
	const auto none = DiscoverAgentSessions(folder, AgentSessionDiscoveryRoots{}, {}, nullptr);
	ASSERT_TRUE(none.has_value());
	EXPECT_TRUE(none->empty());
	auto missing = homes.roots;
	missing.claudeConfigDirectory = (homes.base / L"absent").wstring();
	const auto absent = DiscoverAgentSessions(folder, missing, {}, nullptr);
	ASSERT_TRUE(absent.has_value());
	EXPECT_TRUE(absent->empty());
	const auto relative = DiscoverAgentSessions(L"repo", homes.roots, {}, nullptr);
	ASSERT_TRUE(relative.has_value());
	EXPECT_TRUE(relative->empty());
}

TEST(AgentSessionHistory, RenameTitlesAreOneNormalisedLineWithinTheBound)
{
	EXPECT_EQ(std::optional<std::wstring>(L"Fix the parser"), PrepareAgentSessionRenameTitle(L"  Fix\tthe \r\n parser  "));
	EXPECT_EQ(std::optional<std::wstring>(L"a b"), PrepareAgentSessionRenameTitle(L"a\x01\x7f" L"b"));
	EXPECT_EQ(std::optional<std::wstring>(L"\u65e5\u672c\u8a9e"), PrepareAgentSessionRenameTitle(L"\u3000\u65e5\u672c\u8a9e\u3000"));
	EXPECT_FALSE(PrepareAgentSessionRenameTitle(L"").has_value());
	EXPECT_FALSE(PrepareAgentSessionRenameTitle(L" \t\r\n").has_value());
	// An unpaired surrogate has no UTF-8 form.
	EXPECT_FALSE(PrepareAgentSessionRenameTitle(std::wstring(1, static_cast<wchar_t>(0xD800))).has_value());
	EXPECT_TRUE(PrepareAgentSessionRenameTitle(std::wstring(kMaximumAgentSessionTitleLength, L'x')).has_value());
	EXPECT_FALSE(PrepareAgentSessionRenameTitle(std::wstring(kMaximumAgentSessionTitleLength + 1U, L'x')).has_value());
}

TEST(AgentSessionHistory, ClaudeCodeRenameRecordsAreExactAndReadBackAsTheTitle)
{
	const auto record = BuildClaudeCodeCustomTitleRecord(kClaudeId, L" Quote \" and \\ back ");
	EXPECT_EQ(std::optional<std::string>(
		"{\"type\":\"custom-title\",\"customTitle\":\"Quote \\\" and \\\\ back\","
		"\"sessionId\":\"0f1e2d3c-4b5a-4968-8776-655443322110\"}\n"), record);
	EXPECT_EQ(std::optional<std::string>("{\"customTitle\":\"Quote \\\" and \\\\ back\"}"),
		BuildClaudeCodeCustomTitleSidecar(L" Quote \" and \\ back "));
	EXPECT_EQ(std::optional<std::string>("{\"customTitle\":\"\xE6\x97\xA5\xE6\x9C\xAC\"}"),
		BuildClaudeCodeCustomTitleSidecar(L"\u65e5\u672c"));
	EXPECT_FALSE(BuildClaudeCodeCustomTitleRecord(L"a b", L"Title").has_value());
	EXPECT_FALSE(BuildClaudeCodeCustomTitleRecord(kClaudeId, L"  ").has_value());
	EXPECT_FALSE(BuildClaudeCodeCustomTitleSidecar(L"").has_value());

	// Appended to a transcript, the record is the title discovery reports.
	TemporaryAgentHomes homes;
	WriteFile(homes.ClaudeProject() / (std::wstring(kClaudeId) + L".jsonl"),
		ClaudeTranscript(homes.folder.wstring(), "Generated title") + *record, 1'700'000'000);
	const auto sessions = DiscoverAgentSessions(homes.folder.wstring(), homes.roots, {}, nullptr);
	ASSERT_TRUE(sessions.has_value());
	ASSERT_EQ(1U, sessions->size());
	EXPECT_EQ(L"Quote \" and \\ back", (*sessions)[0].title);
}

TEST(AgentSessionHistory, ClaudeCodeLiveSessionMarkersNeedAValidIdAndProcessId)
{
	const auto live = ParseClaudeCodeLiveSession(
		R"({"pid":4242,"sessionId":"0f1e2d3c-4b5a-4968-8776-655443322110","cwd":"C:\\repo","kind":"interactive"})");
	ASSERT_TRUE(live.has_value());
	EXPECT_EQ(kClaudeId, live->sessionId);
	EXPECT_EQ(4242U, live->processId);
	for (const std::string_view json : {
		std::string_view(""), std::string_view("[]"), std::string_view(R"({"pid":4242})"),
		std::string_view(R"({"sessionId":"abc"})"), std::string_view(R"({"pid":"4242","sessionId":"abc"})"),
		std::string_view(R"({"pid":0,"sessionId":"abc"})"), std::string_view(R"({"pid":-3,"sessionId":"abc"})"),
		std::string_view(R"({"pid":1.5,"sessionId":"abc"})"), std::string_view(R"({"pid":4294967296,"sessionId":"abc"})"),
		std::string_view(R"({"pid":4242,"sessionId":"a b"})") }) {
		EXPECT_FALSE(ParseClaudeCodeLiveSession(json).has_value()) << json;
	}
}

TEST(AgentSessionHistory, CodexRenameRequestsAndRepliesFollowTheAppServerProtocol)
{
	EXPECT_EQ(std::optional<std::string>(
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":"
		"{\"name\":\"sakura-editor-next\",\"version\":\"1\"}}}\n"
		"{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"thread/name/set\",\"params\":{\"threadId\":"
		"\"019a0000-0000-7000-8000-000000000001\",\"name\":\"Add \\\"sessions\\\"\"}}\n"),
		BuildCodexRenameRequests(kCodexId, L"Add \"sessions\""));
	EXPECT_FALSE(BuildCodexRenameRequests(L"-x", L"Name").has_value());
	EXPECT_FALSE(BuildCodexRenameRequests(kCodexId, L"").has_value());

	EXPECT_EQ(ECodexRenameReply::Pending, ParseCodexRenameReplies(""));
	EXPECT_EQ(ECodexRenameReply::Pending, ParseCodexRenameReplies("{\"id\":1,\"result\":{}}\n"));
	// A reply cut by the read boundary is not a reply yet.
	EXPECT_EQ(ECodexRenameReply::Pending, ParseCodexRenameReplies("{\"id\":1,\"result\":{}}\n{\"id\":2,\"result\":{}}"));
	EXPECT_EQ(ECodexRenameReply::Pending,
		ParseCodexRenameReplies("{\"method\":\"thread/name/updated\",\"params\":{}}\n{\"id\":7,\"result\":{}}\nnoise\n"));
	EXPECT_EQ(ECodexRenameReply::Succeeded,
		ParseCodexRenameReplies("{\"id\":1,\"result\":{}}\n{\"method\":\"x\"}\n{\"id\":2,\"result\":{}}\n"));
	EXPECT_EQ(ECodexRenameReply::Failed,
		ParseCodexRenameReplies("{\"id\":1,\"error\":{\"code\":-32600}}\n{\"id\":2,\"result\":{}}\n"));
	EXPECT_EQ(ECodexRenameReply::Failed,
		ParseCodexRenameReplies("{\"id\":1,\"result\":{}}\n{\"id\":2,\"error\":{\"message\":\"unknown thread\"}}\n"));
}

TEST(AgentSessionHistory, PiSessionInfoEntriesLinkToTheEndOfACompleteChain)
{
	const std::string header = "{\"type\":\"session\",\"version\":3,\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n";
	const auto empty = ReadPiSessionChain(header, kPiId);
	ASSERT_TRUE(empty.has_value());
	EXPECT_FALSE(empty->lastEntryId.has_value());
	EXPECT_TRUE(empty->entryIds.empty());
	EXPECT_EQ(std::optional<std::string>("{\"type\":\"session_info\",\"id\":\"0a1b2c3d\",\"parentId\":null,"
		"\"timestamp\":\"2026-11-10T01:02:03.004Z\",\"name\":\"Named\"}\n"),
		BuildPiSessionInfoRecord("0a1b2c3d", *empty, "2026-11-10T01:02:03.004Z", L" Named "));

	const std::string file = header
		+ "{\"type\":\"message\",\"id\":\"aaaaaaaa\",\"parentId\":null}\r\n"
		+ "{\"type\":\"message\",\"id\":\"bbbbbbbb\",\"parentId\":\"aaaaaaaa\"}\n";
	const auto chain = ReadPiSessionChain(file, kPiId);
	ASSERT_TRUE(chain.has_value());
	EXPECT_EQ(std::optional<std::string>("bbbbbbbb"), chain->lastEntryId);
	EXPECT_EQ((std::vector<std::string>{ "aaaaaaaa", "bbbbbbbb" }), chain->entryIds);
	const auto record = BuildPiSessionInfoRecord("0a1b2c3d", *chain, "2026-11-10T01:02:03.004Z", L"Port \"it\"");
	EXPECT_EQ(std::optional<std::string>("{\"type\":\"session_info\",\"id\":\"0a1b2c3d\",\"parentId\":\"bbbbbbbb\","
		"\"timestamp\":\"2026-11-10T01:02:03.004Z\",\"name\":\"Port \\\"it\\\"\"}\n"), record);
	// The appended entry extends the chain and is the name the parser reports.
	const auto extended = ReadPiSessionChain(file + *record, kPiId);
	ASSERT_TRUE(extended.has_value());
	EXPECT_EQ(std::optional<std::string>("0a1b2c3d"), extended->lastEntryId);

	// An id already in the file, a malformed id, and an empty timestamp or name are refused.
	EXPECT_FALSE(BuildPiSessionInfoRecord("aaaaaaaa", *chain, "2026-11-10T01:02:03.004Z", L"Named").has_value());
	EXPECT_FALSE(BuildPiSessionInfoRecord("0A1B2C3D", *chain, "2026-11-10T01:02:03.004Z", L"Named").has_value());
	EXPECT_FALSE(BuildPiSessionInfoRecord("0a1b2c3", *chain, "2026-11-10T01:02:03.004Z", L"Named").has_value());
	EXPECT_FALSE(BuildPiSessionInfoRecord("0a1b2c3d", *chain, "", L"Named").has_value());
	EXPECT_FALSE(BuildPiSessionInfoRecord("0a1b2c3d", *chain, "2026-11-10T01:02:03.004Z", L" ").has_value());

	// Only a complete file for this session is a chain.
	EXPECT_FALSE(ReadPiSessionChain("", kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header.substr(0, header.size() - 1), kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header, kCodexId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header + "{\"type\":\"message\"}\n", kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header + "{\"type\":\"message\",\"id\":7}\n", kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header + "not json\n", kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain(header + "{\"type\":\"mess", kPiId).has_value());
	EXPECT_FALSE(ReadPiSessionChain("{\"type\":\"message\",\"id\":\"019a1111-2222-7333-8444-555566667777\"}\n", kPiId)
		.has_value());
}

// #408: a pinned or archived session stays listed however many newer sessions exist.
TEST(AgentSessionDiscovery, RequiredIdentitiesAreReadFirstAndDoNotCountAgainstTheProviderLimit)
{
	TemporaryAgentHomes homes;
	const auto folder = homes.folder.wstring();
	constexpr std::int64_t base = 1'700'000'000;
	const auto claudeId = [](const int index) { return L"claude-" + std::to_wstring(index); };
	const auto piId = [](const int index) { return L"pi-" + std::to_wstring(index); };
	// Index 0 is the oldest session of each provider.
	for (int index = 0; index < 6; ++index) {
		WriteFile(homes.ClaudeProject() / (claudeId(index) + L".jsonl"),
			ClaudeTranscript(folder, "Claude " + std::to_string(index)), base + index);
		WriteFile(homes.PiSessions() / (L"2026-11-09T10-00-0" + std::to_wstring(index) + L"-000Z_" + piId(index) + L".jsonl"),
			PiSession(piId(index), folder, "Pi " + std::to_string(index)), base + 100 + index);
	}
	WriteFile(homes.CodexDay() / (L"rollout-2026-11-09T10-00-00-" + std::wstring(kCodexId) + L".jsonl"),
		CodexRollout(kCodexId, folder, "user"), base - 500);

	const auto ids = [](const std::optional<std::vector<AgentSessionRecord>>& sessions) {
		std::vector<std::wstring> result;
		if (sessions) for (const auto& session : *sessions) result.push_back(AgentSessionIdentity(session));
		return result;
	};
	AgentSessionDiscoveryLimits limits;
	limits.maximumSessionsPerProvider = 2;
	limits.maximumSessions = 20;
	EXPECT_EQ((std::vector<std::wstring>{ L"pi:pi-5", L"pi:pi-4", L"claude-code:claude-5", L"claude-code:claude-4",
		std::wstring(L"codex:") + kCodexId }), ids(DiscoverAgentSessions(folder, homes.roots, limits, nullptr)));

	// The old marked sessions come on top of the two newest of each provider.
	limits.requiredIdentities = { L"claude-code:claude-0", L"pi:pi-1", L"pi:missing", L"devin:not-a-file" };
	EXPECT_EQ((std::vector<std::wstring>{ L"pi:pi-5", L"pi:pi-4", L"pi:pi-1", L"claude-code:claude-5",
		L"claude-code:claude-4", L"claude-code:claude-0", std::wstring(L"codex:") + kCodexId }),
		ids(DiscoverAgentSessions(folder, homes.roots, limits, nullptr)));

	// A marked session that is among the newest is listed once, and it does
	// not use up one of its provider's two unmarked places.
	limits.requiredIdentities = { L"claude-code:claude-5" };
	EXPECT_EQ((std::vector<std::wstring>{ L"pi:pi-5", L"pi:pi-4", L"claude-code:claude-5", L"claude-code:claude-4",
		L"claude-code:claude-3", std::wstring(L"codex:") + kCodexId }),
		ids(DiscoverAgentSessions(folder, homes.roots, limits, nullptr)));

	// The overall bound still holds.
	limits.requiredIdentities = { L"claude-code:claude-0", L"pi:pi-1" };
	limits.maximumSessions = 3;
	EXPECT_EQ(3U, ids(DiscoverAgentSessions(folder, homes.roots, limits, nullptr)).size());
}

TEST(AgentSessionDiscovery, LimitsForTheSessionsViewLeaveRoomForEveryMarkedSession)
{
	const auto limits = AgentSessionDiscoveryLimitsFor(30, { L"codex:a", L"pi:b", L"devin:c" });
	EXPECT_EQ(30U, limits.maximumSessionsPerProvider);
	EXPECT_EQ(123U, limits.maximumSessions);
	EXPECT_EQ((std::vector<std::wstring>{ L"codex:a", L"pi:b", L"devin:c" }), limits.requiredIdentities);
	const AgentSessionDiscoveryLimits defaults;
	EXPECT_EQ(defaults.headBytes, limits.headBytes);
	EXPECT_EQ(defaults.maximumFileReadsPerProvider, limits.maximumFileReadsPerProvider);

	const auto plain = AgentSessionDiscoveryLimitsFor(10, {});
	EXPECT_EQ(10U, plain.maximumSessionsPerProvider);
	EXPECT_EQ(40U, plain.maximumSessions);
}
