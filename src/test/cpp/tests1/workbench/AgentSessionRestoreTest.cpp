/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/projects/AgentSessionRestore.h"
#include "workbench/projects/AgentSessionRestoreService.h"
#include "terminal/window/TerminalAgentPresence.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace workbench::projects;
using terminal::TerminalAgentPresenceTracker;
using terminal::TerminalBackendJobProcess;
using terminal::TerminalBackendJobProcesses;

constexpr wchar_t kClaude[] = L"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110";
constexpr wchar_t kCodex[] = L"codex:019a0000-0000-7000-8000-000000000001";
constexpr wchar_t kDevin[] = L"devin:ninth-tamarind";
constexpr wchar_t kPi[] = L"pi:019a1111-2222-7333-8444-555566667777";
constexpr wchar_t kFolder[] = L"C:\\work\\project";

AgentSessionRecord Session(const EAgentSessionProvider provider, const wchar_t* id,
	const std::int64_t activity)
{
	return { .provider = provider, .id = id, .title = {}, .lastActivityUnixSeconds = activity };
}

LiveAgentSessionSource Agent(const std::wstring_view name, const std::int64_t started,
	std::optional<std::wstring> known = std::nullopt)
{
	return { .name = std::wstring(name), .knownSessionIdentity = std::move(known),
		.startedUnixSeconds = started };
}

std::vector<AgentSessionRecord> Sessions(std::initializer_list<AgentSessionRecord> list)
{
	return { list };
}

struct StoreScript final {
	std::deque<ProjectCatalogStoreLoadResult> loads;
	std::deque<EProjectCatalogStoreSaveStatus> saves;
	std::vector<std::string> saved;
	int loadCalls = 0;
};

class ScriptedStore final : public IProjectCatalogStore {
public:
	explicit ScriptedStore(std::shared_ptr<StoreScript> script) : m_script(std::move(script)) {}

	ProjectCatalogStoreLoadResult Load() override
	{
		++m_script->loadCalls;
		if (m_script->loads.empty()) return { EProjectCatalogStoreLoadStatus::NotFound, std::nullopt, {} };
		auto result = std::move(m_script->loads.front());
		m_script->loads.pop_front();
		return result;
	}

	ProjectCatalogStoreSaveResult Save(std::string payload) override
	{
		m_script->saved.push_back(std::move(payload));
		if (m_script->saves.empty()) return { EProjectCatalogStoreSaveStatus::Succeeded, {} };
		const auto status = m_script->saves.front();
		m_script->saves.pop_front();
		return { status, {} };
	}

private:
	std::shared_ptr<StoreScript> m_script;
};

ProjectCatalogStoreLoadResult Stored(std::string payload)
{
	return { EProjectCatalogStoreLoadStatus::Succeeded, std::move(payload), {} };
}

} // namespace

TEST(AgentSessionRestore, IdentityProviderParsesOnlyValidIdentities)
{
	EXPECT_EQ(EAgentSessionProvider::ClaudeCode, AgentSessionIdentityProvider(kClaude));
	EXPECT_EQ(EAgentSessionProvider::Codex, AgentSessionIdentityProvider(kCodex));
	EXPECT_EQ(EAgentSessionProvider::Devin, AgentSessionIdentityProvider(kDevin));
	EXPECT_EQ(EAgentSessionProvider::Pi, AgentSessionIdentityProvider(kPi));
	EXPECT_EQ(std::nullopt, AgentSessionIdentityProvider(L""));
	EXPECT_EQ(std::nullopt, AgentSessionIdentityProvider(L"claude-code:"));
	EXPECT_EQ(std::nullopt, AgentSessionIdentityProvider(L"gemini:abc"));
	EXPECT_EQ(std::nullopt, AgentSessionIdentityProvider(L"codex:a b"));
	EXPECT_EQ(std::nullopt, AgentSessionIdentityProvider(L"Codex:abc"));
}

TEST(AgentSessionRestore, RecordKeepsKnownIdentitiesFirstInInputOrder)
{
	const std::vector<LiveAgentSessionSource> agents{
		Agent(L"Codex", 100, std::wstring(kCodex)),
		Agent(L"Claude Code", 200, std::wstring(kClaude)),
	};
	EXPECT_EQ((std::vector<std::wstring>{ kCodex, kClaude }),
		RecordLiveAgentSessions(agents, {}));
}

TEST(AgentSessionRestore, RecordIgnoresKnownIdentitiesThatDoNotFitTheLiveAgent)
{
	// A valid identity for another provider than the live agent, an invalid
	// one, and one on a non-session-capable agent all record nothing.
	const std::vector<LiveAgentSessionSource> agents{
		Agent(L"Codex", 100, std::wstring(kClaude)),
		Agent(L"Claude Code", 100, std::wstring(L"claude-code:")),
		Agent(L"Copilot", 100, std::wstring(kCodex)),
	};
	EXPECT_TRUE(RecordLiveAgentSessions(agents, {}).empty());
}

TEST(AgentSessionRestore, RecordInfersTheNewestSessionAtOrAfterTheStartAnchor)
{
	const auto sessions = Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"older", 50),
		Session(EAgentSessionProvider::ClaudeCode, L"newest", 300),
		Session(EAgentSessionProvider::ClaudeCode, L"newer", 200),
	});
	const std::vector<LiveAgentSessionSource> agents{ Agent(L"Claude Code", 100) };
	EXPECT_EQ((std::vector<std::wstring>{ L"claude-code:newest" }),
		RecordLiveAgentSessions(agents, sessions));
}

TEST(AgentSessionRestore, RecordNeverInfersWithoutAnAnchorOrWithoutActivityAfterIt)
{
	const auto sessions = Sessions({
		Session(EAgentSessionProvider::Codex, L"s1", 100),
		Session(EAgentSessionProvider::Codex, L"s2", 200),
	});
	// No start anchor: nothing can be proven, so nothing is recorded.
	EXPECT_TRUE(RecordLiveAgentSessions(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 0) }, sessions).empty());
	EXPECT_TRUE(RecordLiveAgentSessions(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", -5) }, sessions).empty());
	// Every session ended before this agent started: none is its.
	EXPECT_TRUE(RecordLiveAgentSessions(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 300) }, sessions).empty());
	// No sessions at all: nothing to infer from.
	EXPECT_TRUE(RecordLiveAgentSessions(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 100) }, {}).empty());
	// A session of another provider or without activity helps no one.
	EXPECT_TRUE(RecordLiveAgentSessions(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 100) }, Sessions({
			Session(EAgentSessionProvider::Pi, L"s1", 500),
			Session(EAgentSessionProvider::Codex, L"s2", 0) })).empty());
}

TEST(AgentSessionRestore, RecordLetsTheLatestAgentClaimFirstAndKeepsTheOldCandidateForAnEarlierOne)
{
	const auto sessions = Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"old", 100),
		Session(EAgentSessionProvider::ClaudeCode, L"new", 400),
	});
	// The newest agent cannot take "old", but the earlier one still does;
	// claims are recorded in claim order, latest-started agent first.
	const std::vector<LiveAgentSessionSource> agents{
		Agent(L"Claude Code", 50),
		Agent(L"Claude Code", 300),
	};
	EXPECT_EQ((std::vector<std::wstring>{ L"claude-code:new", L"claude-code:old" }),
		RecordLiveAgentSessions(agents, sessions));
}

TEST(AgentSessionRestore, RecordKeepsASessionForAtMostOneAgent)
{
	const auto sessions = Sessions({ Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 500) });
	const std::vector<LiveAgentSessionSource> agents{
		Agent(L"Devin", 100),
		Agent(L"Devin", 200),
	};
	EXPECT_EQ((std::vector<std::wstring>{ kDevin }),
		RecordLiveAgentSessions(agents, sessions));
}

TEST(AgentSessionRestore, RecordBoundsTheResultAndSkipsNothingWorthwhile)
{
	std::vector<AgentSessionRecord> sessions;
	std::vector<LiveAgentSessionSource> agents;
	for (std::size_t index = 0; index < kMaximumAgentSessionRestoreSessions + 3; ++index) {
		sessions.push_back(Session(EAgentSessionProvider::Codex,
			(std::wstring(L"s") + std::to_wstring(index)).c_str(), static_cast<std::int64_t>(100 + index)));
		agents.push_back(Agent(L"Codex", 1 + static_cast<std::int64_t>(index)));
	}
	const auto recorded = RecordLiveAgentSessions(agents, sessions);
	EXPECT_EQ(kMaximumAgentSessionRestoreSessions, recorded.size());
	// Every agent claims the newest unclaimed session newer than its start;
	// the newest session of all goes to the agent that started latest.
	EXPECT_EQ(std::wstring(L"codex:s") + std::to_wstring(kMaximumAgentSessionRestoreSessions + 2),
		recorded.front());
}

TEST(AgentSessionRestore, RestoreEntryLookupReplacesAndRemovesPerFolder)
{
	AgentSessionRestoreState state;
	EXPECT_TRUE(FindAgentSessionRestoreEntry(state, kFolder).empty());
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, kFolder, { kClaude }));
	EXPECT_EQ((std::vector<std::wstring>{ kClaude }),
		std::vector<std::wstring>(FindAgentSessionRestoreEntry(state, kFolder).begin(),
			FindAgentSessionRestoreEntry(state, kFolder).end()));
	// Same folder spelled differently replaces the entry, keeping one row.
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, L"c:/work/project/", { kCodex }));
	EXPECT_EQ(1U, state.entries.size());
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }),
		std::vector<std::wstring>(FindAgentSessionRestoreEntry(state, kFolder).begin(),
			FindAgentSessionRestoreEntry(state, kFolder).end()));
	// Recording nothing removes the row entirely.
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, kFolder, {}));
	EXPECT_TRUE(state.entries.empty());
	// Invalid input changes nothing.
	EXPECT_FALSE(SetAgentSessionRestoreEntry(state, L"", { kClaude }));
	EXPECT_FALSE(SetAgentSessionRestoreEntry(state, kFolder, { L"gemini:abc" }));
	EXPECT_FALSE(SetAgentSessionRestoreEntry(state, kFolder, { kClaude, kClaude }));
	EXPECT_TRUE(state.entries.empty());
}

TEST(AgentSessionRestore, RestoreEntryCapDropsTheLeastRecentlyWritten)
{
	AgentSessionRestoreState state;
	for (std::size_t index = 0; index < kMaximumAgentSessionRestoreProjects + 1; ++index) {
		ASSERT_TRUE(SetAgentSessionRestoreEntry(state,
			L"C:\\p" + std::to_wstring(index), { kCodex }));
	}
	EXPECT_EQ(kMaximumAgentSessionRestoreProjects, state.entries.size());
	EXPECT_TRUE(FindAgentSessionRestoreEntry(state, L"C:\\p0").empty());
	EXPECT_FALSE(FindAgentSessionRestoreEntry(state, L"C:\\p" +
		std::to_wstring(kMaximumAgentSessionRestoreProjects)).empty());
}

TEST(AgentSessionRestore, CodecRoundTripsAndRejectsMalformedState)
{
	EXPECT_EQ(std::optional<std::string>(R"({"version":1,"projects":[]})"),
		EncodeAgentSessionRestoreState({}));

	AgentSessionRestoreState state{ .entries = {
		{ .folder = kFolder, .sessions = { kClaude, kDevin } },
		{ .folder = L"D:\\other", .sessions = { kCodex } },
	} };
	const auto encoded = EncodeAgentSessionRestoreState(state);
	ASSERT_TRUE(encoded.has_value());
	EXPECT_EQ(std::optional<AgentSessionRestoreState>(state),
		DecodeAgentSessionRestoreState(*encoded));

	for (const std::string_view payload : {
		std::string_view(""),
		std::string_view("[]"),
		std::string_view("{"),
		std::string_view(R"({"version":2,"projects":[]})"),
		std::string_view(R"({"version":"1","projects":[]})"),
		std::string_view(R"({"version":1,"projects":{}})"),
		std::string_view(R"({"version":1,"projects":[{"sessions":[]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\\a"}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"","sessions":[]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\\a","sessions":[]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\a","sessions":["gemini:x"]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\\a","sessions":["codex:x","codex:x"]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\\a","sessions":[]},{"folder":"c:\\A","sessions":[]}]})"),
		std::string_view(R"({"version":1,"projects":[{"folder":"C:\\a","sessions":[7]}]})"),
	}) {
		EXPECT_FALSE(DecodeAgentSessionRestoreState(payload).has_value()) << payload;
	}

	// Encode refuses what Decode refuses.
	EXPECT_FALSE(EncodeAgentSessionRestoreState({ .entries = {
		{ .folder = L"", .sessions = {} } } }).has_value());
	EXPECT_FALSE(EncodeAgentSessionRestoreState({ .entries = {
		{ .folder = kFolder, .sessions = { L"nope" } } } }).has_value());
	EXPECT_FALSE(EncodeAgentSessionRestoreState({ .entries = {
		{ .folder = kFolder, .sessions = { kClaude, kClaude } } } }).has_value());

	// A payload over the byte cap never decodes.
	std::string oversized = R"({"version":1,"projects":[{"folder":"C:\\)";
	for (std::size_t index = 0; index < 70000; ++index) oversized += 'a';
	oversized += R"(","sessions":[]}]})";
	EXPECT_FALSE(DecodeAgentSessionRestoreState(oversized).has_value());
}

TEST(AgentSessionRestore, PlanCoversEveryExplicitBranch)
{
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"0f1e2d3c-4b5a-4968-8776-655443322110", 10),
		Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 20),
	}));
	const std::vector<std::wstring> one{ kClaude };
	const std::vector<std::wstring> four{ kClaude, kCodex, kDevin, kPi };

	auto plan = PlanAgentSessionRestore(false, one, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::Disabled, plan.status);
	EXPECT_TRUE(plan.resume.empty());

	plan = PlanAgentSessionRestore(true, {}, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::NothingRecorded, plan.status);

	plan = PlanAgentSessionRestore(true, one, std::nullopt);
	EXPECT_EQ(EAgentSessionRestoreStatus::DiscoveryFailed, plan.status);
	EXPECT_TRUE(plan.resume.empty());

	// kCodex is gone: skipped, never retried; kPi beyond the cap: dropped.
	plan = PlanAgentSessionRestore(true, four, sessions, 3);
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, plan.status);
	ASSERT_EQ(2U, plan.resume.size());
	EXPECT_EQ(L"0f1e2d3c-4b5a-4968-8776-655443322110", plan.resume[0].id);
	EXPECT_EQ(L"ninth-tamarind", plan.resume[1].id);
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }), plan.skipped);
	EXPECT_EQ(1U, plan.dropped);
}

TEST(AgentSessionRestore, DispatchRunsTheResumeSeamAndCountsEveryOutcome)
{
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"0f1e2d3c-4b5a-4968-8776-655443322110", 10),
		Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 20),
	}));
	const std::vector<std::wstring> recorded{ kClaude, kCodex, kDevin };
	const auto plan = PlanAgentSessionRestore(true, recorded, sessions);

	std::vector<std::wstring> resumed;
	const auto outcome = DispatchAgentSessionRestore(plan,
		[&resumed](const AgentSessionRecord& session) {
			resumed.push_back(session.id);
			return session.provider == EAgentSessionProvider::ClaudeCode;
		});
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, outcome.status);
	EXPECT_EQ(1U, outcome.resumed);
	EXPECT_EQ(1U, outcome.failed);
	EXPECT_EQ(1U, outcome.skipped);
	EXPECT_EQ((std::vector<std::wstring>{
		L"0f1e2d3c-4b5a-4968-8776-655443322110", L"ninth-tamarind" }), resumed);

	// A non-Ready plan never touches the seam, and a missing seam resumes none.
	bool called = false;
	const std::vector<std::wstring> one{ kClaude };
	const auto disabled = DispatchAgentSessionRestore(
		PlanAgentSessionRestore(false, one, sessions),
		[&called](const AgentSessionRecord&) { return called = true; });
	EXPECT_EQ(EAgentSessionRestoreStatus::Disabled, disabled.status);
	EXPECT_FALSE(called);
	EXPECT_EQ(0U, DispatchAgentSessionRestore(plan, {}).resumed);
}

TEST(AgentSessionRestoreService, NothingStoredGivesAnEmptyRecordAndTheFirstChangeIsSaved)
{
	const auto script = std::make_shared<StoreScript>();
	CAgentSessionRestoreService service(std::make_unique<ScriptedStore>(script));
	EXPECT_TRUE(service.Snapshot().entries.empty());
	EXPECT_TRUE(service.RecordedSessions(kFolder).empty());

	service.Load();
	EXPECT_TRUE(script->saved.empty());

	EXPECT_TRUE(service.Update([](AgentSessionRestoreState& state) {
		return SetAgentSessionRestoreEntry(state, kFolder, { kClaude });
	}));
	ASSERT_EQ(1U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"projects":[{"folder":"C:\\work\\project","sessions":[)"
		R"("claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"]}]})", script->saved[0]);
	EXPECT_EQ((std::vector<std::wstring>{ kClaude }), service.RecordedSessions(kFolder));

	// A no-op change saves nothing, and a null change is refused.
	EXPECT_FALSE(service.Update([](AgentSessionRestoreState& state) {
		return SetAgentSessionRestoreEntry(state, kFolder, { kClaude });
	}));
	EXPECT_FALSE(service.Update({}));
	EXPECT_EQ(1U, script->saved.size());
}

TEST(AgentSessionRestoreService, AnUndecodableRecordGivesAnEmptyRecordAndIsNeverOverwritten)
{
	for (const char* stored : { "not json", R"({"version":9,"projects":[]})" }) {
		const auto script = std::make_shared<StoreScript>();
		for (int index = 0; index < 4; ++index) script->loads.push_back(Stored(stored));
		CAgentSessionRestoreService service(std::make_unique<ScriptedStore>(script));
		service.Load();
		EXPECT_TRUE(service.Snapshot().entries.empty());
		EXPECT_TRUE(service.Update([](AgentSessionRestoreState& state) {
			return SetAgentSessionRestoreEntry(state, kFolder, { kClaude });
		}));
		EXPECT_TRUE(script->saved.empty()) << stored;
	}
}

TEST(AgentSessionRestoreService, AConflictReloadsTheOtherWindowsRecordAndAppliesTheChangeToIt)
{
	const auto script = std::make_shared<StoreScript>();
	CAgentSessionRestoreService service(std::make_unique<ScriptedStore>(script));
	service.Load();

	// Another window recorded a different Project first; this window's save
	// lands on that state, keeping both entries.
	script->saves.push_back(EProjectCatalogStoreSaveStatus::Conflict);
	script->loads.push_back(Stored(
		R"({"version":1,"projects":[{"folder":"D:\\other","sessions":["devin:ninth-tamarind"]}]})"));
	EXPECT_TRUE(service.Update([](AgentSessionRestoreState& state) {
		return SetAgentSessionRestoreEntry(state, kFolder, { kClaude });
	}));
	ASSERT_EQ(2U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"projects":[{"folder":"D:\\other","sessions":["devin:ninth-tamarind"]},)"
		R"({"folder":"C:\\work\\project","sessions":["claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"]}]})",
		script->saved[1]);
	EXPECT_EQ(2U, service.Snapshot().entries.size());
}

TEST(AgentSessionRestoreService, WithoutAStoreTheRecordLivesInTheWindowOnly)
{
	CAgentSessionRestoreService service(nullptr);
	service.Load();
	EXPECT_TRUE(service.Update([](AgentSessionRestoreState& state) {
		return SetAgentSessionRestoreEntry(state, kFolder, { kCodex });
	}));
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }), service.RecordedSessions(kFolder));
}

TEST(TerminalAgentPresence, StartAnchorIsTheEarliestAgentProcessCreation)
{
	TerminalAgentPresenceTracker tracker;
	TerminalBackendJobProcesses processes;
	processes.processes = {
		TerminalBackendJobProcess{ 10, 134'000'000'000'000'000ULL, L"pwsh.exe" },
		TerminalBackendJobProcess{ 11, 134'000'100'000'000'000ULL, L"claude.exe" },
		TerminalBackendJobProcess{ 12, 134'000'050'000'000'000ULL, L"claude.exe" },
	};
	EXPECT_TRUE(tracker.ApplyJobObservation(7, processes));
	const auto started = tracker.StartedUnixSeconds(7);
	ASSERT_TRUE(started.has_value());
	// The earliest agent process's FILETIME, as unix seconds.
	EXPECT_EQ(134'000'050'000'000'000ULL / 10'000'000ULL - 11'644'473'600ULL,
		static_cast<std::uint64_t>(*started));
	EXPECT_FALSE(tracker.StartedUnixSeconds(8).has_value());
}

TEST(TerminalAgentPresence, TitleOnlyPresenceAnchorsAtFirstResolutionAndProcessesBeatIt)
{
	TerminalAgentPresenceTracker tracker;
	EXPECT_TRUE(tracker.ApplyTitle(7, L"Claude Code: topic"));
	const auto first = tracker.StartedUnixSeconds(7);
	ASSERT_TRUE(first.has_value());
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	EXPECT_GE(*first, now - 60);
	EXPECT_LE(*first, now + 60);

	// An agent process in the job moves the anchor to its real creation time.
	TerminalBackendJobProcesses processes;
	processes.processes = {
		TerminalBackendJobProcess{ 11, 134'000'100'000'000'000ULL, L"claude.exe" },
	};
	(void)tracker.ApplyJobObservation(7, processes);
	const auto processAnchored = tracker.StartedUnixSeconds(7);
	ASSERT_TRUE(processAnchored.has_value());
	EXPECT_EQ(134'000'100'000'000'000ULL / 10'000'000ULL - 11'644'473'600ULL,
		static_cast<std::uint64_t>(*processAnchored));

	// Losing the presence drops the anchor.
	(void)tracker.ApplyJobObservation(7, std::nullopt);
	(void)tracker.ApplyTitle(7, L"");
	EXPECT_FALSE(tracker.Presence(7).has_value());
	EXPECT_FALSE(tracker.StartedUnixSeconds(7).has_value());
}
