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

//! The identities RecordLiveAgentSessionsByTab claims, in claim order; the
//! record-model tests below assert the arbitration through them.
std::vector<std::wstring> RecordIdentities(const std::vector<LiveAgentSessionSource>& agents,
	const std::vector<AgentSessionRecord>& sessions)
{
	std::vector<std::wstring> identities;
	for (auto& claim : RecordLiveAgentSessionsByTab(agents, sessions)) {
		identities.push_back(std::move(claim.identity));
	}
	return identities;
}

//! The planned session leaves in panel order, groups first to last.
std::vector<AgentSessionRecord> PlannedSessions(const AgentSessionRestorePlan& plan)
{
	std::vector<AgentSessionRecord> planned;
	for (const auto& group : plan.groups) {
		for (const auto& leaf : group.leaves) {
			if (leaf.session) planned.push_back(*leaf.session);
		}
	}
	return planned;
}

template <class Service>
std::vector<std::wstring> RecordedSessionsOf(const Service& service, const std::wstring_view folder)
{
	const auto entry = service.RecordedEntry(folder);
	return entry ? entry->sessions : std::vector<std::wstring>{};
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
		RecordIdentities(agents, {}));
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
	EXPECT_TRUE(RecordIdentities(agents, {}).empty());
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
		RecordIdentities(agents, sessions));
}

TEST(AgentSessionRestore, RecordNeverInfersWithoutAnAnchorOrWithoutActivityAfterIt)
{
	const auto sessions = Sessions({
		Session(EAgentSessionProvider::Codex, L"s1", 100),
		Session(EAgentSessionProvider::Codex, L"s2", 200),
	});
	// No start anchor: nothing can be proven, so nothing is recorded.
	EXPECT_TRUE(RecordIdentities(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 0) }, sessions).empty());
	EXPECT_TRUE(RecordIdentities(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", -5) }, sessions).empty());
	// Every session ended before this agent started: none is its.
	EXPECT_TRUE(RecordIdentities(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 300) }, sessions).empty());
	// No sessions at all: nothing to infer from.
	EXPECT_TRUE(RecordIdentities(std::vector<LiveAgentSessionSource>{
		Agent(L"Codex", 100) }, {}).empty());
	// A session of another provider or without activity helps no one.
	EXPECT_TRUE(RecordIdentities(std::vector<LiveAgentSessionSource>{
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
		RecordIdentities(agents, sessions));
}

TEST(AgentSessionRestore, RecordKeepsASessionForAtMostOneAgent)
{
	const auto sessions = Sessions({ Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 500) });
	const std::vector<LiveAgentSessionSource> agents{
		Agent(L"Devin", 100),
		Agent(L"Devin", 200),
	};
	EXPECT_EQ((std::vector<std::wstring>{ kDevin }),
		RecordIdentities(agents, sessions));
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
	const auto recorded = RecordIdentities(agents, sessions);
	EXPECT_EQ(kMaximumAgentSessionRestoreSessions, recorded.size());
	// Every agent claims the newest unclaimed session newer than its start;
	// the newest session of all goes to the agent that started latest.
	EXPECT_EQ(std::wstring(L"codex:s") + std::to_wstring(kMaximumAgentSessionRestoreSessions + 2),
		recorded.front());
}

TEST(AgentSessionRestore, RecordLiveAgentSessionsByTabPairsEachClaimWithItsTab)
{
	const auto sessions = Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"inferred", 300),
		Session(EAgentSessionProvider::Codex, L"newest", 200),
	});
	std::vector<LiveAgentSessionSource> agents{
		Agent(L"Codex", 100, std::wstring(kCodex)),
		Agent(L"Claude Code", 50),
		Agent(L"Devin", 100),	// unclaimable: no Devin session at all
	};
	agents[0].tabId = 11;
	agents[1].tabId = 12;
	agents[2].tabId = 13;
	const auto claims = RecordLiveAgentSessionsByTab(agents, sessions);
	// Claim order: known identities first, then inferred.
	ASSERT_EQ(2U, claims.size());
	EXPECT_EQ(11U, claims[0].tabId);
	EXPECT_EQ(std::wstring(kCodex), claims[0].identity);
	EXPECT_EQ(12U, claims[1].tabId);
	EXPECT_EQ(std::wstring(L"claude-code:inferred"), claims[1].identity);
	// The unclaimed agent produces no entry; the caller records its tab as a
	// shell leaf.
	EXPECT_EQ(2U, claims.size());
}

TEST(AgentSessionRestore, RestoreEntryLookupReplacesAndRemovesPerFolder)
{
	AgentSessionRestoreState state;
	EXPECT_EQ(nullptr, FindAgentSessionRestoreEntry(state, kFolder));
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, kFolder, { kClaude }));
	EXPECT_EQ((std::vector<std::wstring>{ kClaude }),
		FindAgentSessionRestoreEntry(state, kFolder)->sessions);
	// Same folder spelled differently replaces the entry, keeping one row.
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, L"c:/work/project/", { kCodex }));
	EXPECT_EQ(1U, state.entries.size());
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }),
		FindAgentSessionRestoreEntry(state, kFolder)->sessions);
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
	EXPECT_EQ(nullptr, FindAgentSessionRestoreEntry(state, L"C:\\p0"));
	EXPECT_NE(nullptr, FindAgentSessionRestoreEntry(state, L"C:\\p" +
		std::to_wstring(kMaximumAgentSessionRestoreProjects)));
}

TEST(AgentSessionRestore, CodecRoundTripsAndRejectsMalformedState)
{
	EXPECT_EQ(std::optional<std::string>(R"({"version":2,"projects":[]})"),
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
		std::string_view(R"({"version":0,"projects":[]})"),
		std::string_view(R"({"version":3,"projects":[]})"),
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

	// A version-1 record carries no layout and decodes like a version-2 one
	// without one.
	const auto v1 = DecodeAgentSessionRestoreState(
		R"({"version":1,"projects":[{"folder":"C:\\work\\project","sessions":[)"
		R"("claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"]}]})");
	ASSERT_TRUE(v1.has_value());
	ASSERT_EQ(1U, v1->entries.size());
	EXPECT_FALSE(v1->entries[0].layout.has_value());
	EXPECT_EQ((std::vector<std::wstring>{ kClaude }), v1->entries[0].sessions);

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

TEST(AgentSessionRestore, CodecRoundTripsAVersionTwoLayout)
{
	AgentSessionRestoreLayout layout{
		.leaves = {
			{ .session = std::wstring(kClaude) },
			{ .session = std::wstring(kCodex) },
			{ .shell = std::wstring(L"C:\\work\\project\\sub") },
		},
		// Two agents side by side, a shell below the right one:
		//   root(horizontal): [leaf0, split1]; split1(vertical): [leaf1, leaf2]
		.groups = { { .nodes = {
			{ .leaf = 0 },
			{ .leaf = 1 },
			{ .leaf = 2 },
			{ .orientation = AgentSessionRestoreOrientation::Vertical,
				.children = { 1, 2 }, .weights = { 700, 300 } },
			{ .orientation = AgentSessionRestoreOrientation::Horizontal,
				.children = { 0, 3 }, .weights = { 600, 400 } },
		}, .root = 4 } },
		.active = 1,
	};
	AgentSessionRestoreState state{ .entries = {
		{ .folder = kFolder, .sessions = { kClaude, kCodex }, .layout = layout },
	} };
	const auto encoded = EncodeAgentSessionRestoreState(state);
	ASSERT_TRUE(encoded.has_value());
	EXPECT_EQ(std::optional<AgentSessionRestoreState>(state),
		DecodeAgentSessionRestoreState(*encoded));
}

TEST(AgentSessionRestore, CodecRejectsMalformedLayouts)
{
	const std::string prefix = R"({"version":2,"projects":[{"folder":"C:\\work\\project",)"
		R"("sessions":["claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"],"layout":)";
	auto decode = [&prefix](const std::string& layout) {
		return DecodeAgentSessionRestoreState(prefix + layout + "}]}");
	};

	// A child index out of range.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
		R"({"session":"claude-code:1f1e2d3c-4b5a-4968-8776-655443322111"}],"groups":[{"nodes":[)"
		R"({"leaf":0},{"split":"horizontal","children":[0,9],"weights":[600,400]}],"root":1}]})")
		.has_value());
	// A cycle: node 0 splits into itself and leaf 1.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
		R"({"session":"claude-code:1f1e2d3c-4b5a-4968-8776-655443322111"}],"groups":[{"nodes":[)"
		R"({"split":"horizontal","children":[0,1],"weights":[600,400]},{"leaf":0}],"root":0}]})")
		.has_value());
	// A split with fewer than two children (its one child is a valid leaf).
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0},{"split":"horizontal","children":[0],"weights":[600]}],)"
		R"("root":1}]})").has_value());
	// A non-positive weight.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
		R"({"shell":"C:\\work"}],"groups":[{"nodes":[{"leaf":0},{"leaf":1},)"
		R"({"split":"horizontal","children":[0,1],"weights":[600,0]}],"root":2}]})")
		.has_value());
	// A leaf used twice.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0},{"leaf":0},)"
		R"({"split":"horizontal","children":[0,1],"weights":[600,400]}],"root":2}]})")
		.has_value());
	// An unknown orientation.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
		R"({"shell":"C:\\work"}],"groups":[{"nodes":[{"leaf":0},{"leaf":1},)"
		R"({"split":"diagonal","children":[0,1],"weights":[600,400]}],"root":2}]})")
		.has_value());
	// A leaf index out of range, a root out of range, an unreferenced leaf, a
	// leaf with split data, an unreachable node, and a wrong-typed member
	// beside a valid one.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":1}],"root":0}]})").has_value());
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0}],"root":1}]})").has_value());
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
		R"({"shell":"C:\\work"}],"groups":[{"nodes":[{"leaf":0}],"root":0}]})").has_value());
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0,"children":[1]}],"root":0}]})").has_value());
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0},{"leaf":0}],"root":0}]})").has_value());
	EXPECT_FALSE(decode(R"({"leaves":[{"session":7,"shell":"C:\\work"},)"
		R"({"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],"groups":[{"nodes":[)"
		R"({"leaf":0},{"leaf":1},{"split":"horizontal","children":[0,1],"weights":[600,400]}],)"
		R"("root":2}]})").has_value());
	{
		// A session order that differs from the leaf order, against the same
		// two sessions in leaf order as the control.
		const std::string twoSessions = R"({"version":2,"projects":[{"folder":"C:\\work\\project",)"
			R"("sessions":["claude-code:0f1e2d3c-4b5a-4968-8776-655443322110",)"
			R"("codex:019a0000-0000-7000-8000-000000000001"],"layout":{"leaves":[)"
			R"({"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"},)"
			R"({"session":"codex:019a0000-0000-7000-8000-000000000001"}],"groups":[{"nodes":[)";
		EXPECT_TRUE(DecodeAgentSessionRestoreState(twoSessions
			+ R"({"leaf":0},{"leaf":1},{"split":"horizontal","children":[0,1],)"
			R"("weights":[600,400]}],"root":2}]}}]})").has_value());
		EXPECT_FALSE(DecodeAgentSessionRestoreState(twoSessions
			+ R"({"leaf":1},{"leaf":0},{"split":"horizontal","children":[0,1],)"
			R"("weights":[600,400]}],"root":2}]}}]})").has_value());
	}
	// An active leaf index out of range, a non-object layout, and the bounds:
	// a ninth shell leaf and a thirty-third node.
	EXPECT_FALSE(decode(R"({"leaves":[{"session":"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"}],)"
		R"("groups":[{"nodes":[{"leaf":0}],"root":0}],"active":1})").has_value());
	EXPECT_FALSE(decode("[]").has_value());
	{
		// Every leaf is referenced once through one split, so only the shell
		// bound can reject the record; eight shells are the accepted control.
		const auto layoutWith = [](const std::size_t shellCount) {
			std::string leaves = "{\"session\":\"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110\"}";
			std::string nodes = "{\"leaf\":0}";
			std::string children = "0";
			std::string weights = "100";
			for (std::size_t index = 1; index <= shellCount; ++index) {
				leaves += ",{\"shell\":\"C:\\\\work" + std::to_string(index) + "\"}";
				nodes += ",{\"leaf\":" + std::to_string(index) + "}";
				children += "," + std::to_string(index);
				weights += ",100";
			}
			nodes += ",{\"split\":\"horizontal\",\"children\":[" + children + "],\"weights\":["
				+ weights + "]}";
			return "{\"leaves\":[" + leaves + "],\"groups\":[{\"nodes\":[" + nodes + "],\"root\":"
				+ std::to_string(shellCount + 1) + "}]}";
		};
		EXPECT_TRUE(decode(layoutWith(kMaximumAgentSessionRestoreShellLeaves)).has_value());
		EXPECT_FALSE(decode(layoutWith(kMaximumAgentSessionRestoreShellLeaves + 1)).has_value());
	}
	{
		std::string nodes;
		for (std::size_t index = 0; index <= kMaximumAgentSessionRestoreLayoutNodes; ++index) {
			if (!nodes.empty()) nodes += ',';
			nodes += "{\"leaf\":0}";
		}
		EXPECT_FALSE(decode("{\"leaves\":[{\"session\":\"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110\"}],"
			"\"groups\":[{\"nodes\":[" + nodes + "],\"root\":0}]}").has_value());
	}
}

TEST(AgentSessionRestore, LayoutValidatorTellsTheRecorderWhenToDropTheLayout)
{
	// The recorder checks the live layout with the public validator and, when
	// it is past a bound, records the sessions without it (#415 Phase 3).
	const std::wstring session = L"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110";
	AgentSessionRestoreLayout layout;
	layout.leaves.push_back({ .session = session });
	AgentSessionRestoreLayoutGroup group;
	group.nodes.push_back({ .leaf = 0 });
	layout.groups.push_back(group);
	EXPECT_TRUE(IsValidAgentSessionRestoreLayout(layout, { session }));
	EXPECT_FALSE(IsValidAgentSessionRestoreLayout(layout, {}));

	// One session beside one shell more than the bound allows.
	AgentSessionRestoreLayoutGroup wide;
	AgentSessionRestoreLayoutNode root{ .orientation = AgentSessionRestoreOrientation::Horizontal };
	wide.nodes.push_back({ .leaf = 0 });
	root.children.push_back(0);
	root.weights.push_back(1000);
	for (std::size_t index = 0; index <= kMaximumAgentSessionRestoreShellLeaves; ++index) {
		layout.leaves.push_back({ .shell = L"C:\\work" + std::to_wstring(index) });
		wide.nodes.push_back({ .leaf = layout.leaves.size() - 1 });
		root.children.push_back(wide.nodes.size() - 1);
		root.weights.push_back(1000);
	}
	wide.nodes.push_back(root);
	wide.root = wide.nodes.size() - 1;
	layout.groups = { wide };
	EXPECT_FALSE(IsValidAgentSessionRestoreLayout(layout, { session }));

	AgentSessionRestoreState state;
	EXPECT_FALSE(SetAgentSessionRestoreEntry(state, L"C:\\a", { session }, layout));
	EXPECT_TRUE(SetAgentSessionRestoreEntry(state, L"C:\\a", { session }));
	ASSERT_EQ(1u, state.entries.size());
	EXPECT_FALSE(state.entries[0].layout.has_value());
}

TEST(AgentSessionRestore, PlanCoversEveryExplicitBranch)
{
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"0f1e2d3c-4b5a-4968-8776-655443322110", 10),
		Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 20),
	}));
	const AgentSessionRestoreEntry one{ .folder = kFolder, .sessions = { kClaude } };
	const AgentSessionRestoreEntry four{
		.folder = kFolder, .sessions = { kClaude, kCodex, kDevin, kPi } };

	auto plan = PlanAgentSessionRestore(false, one, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::Disabled, plan.status);
	EXPECT_TRUE(PlannedSessions(plan).empty());

	plan = PlanAgentSessionRestore(true, {}, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::NothingRecorded, plan.status);

	plan = PlanAgentSessionRestore(true, one, std::nullopt);
	EXPECT_EQ(EAgentSessionRestoreStatus::DiscoveryFailed, plan.status);
	EXPECT_TRUE(PlannedSessions(plan).empty());

	// kCodex is gone: skipped, never retried; kPi beyond the cap: dropped.
	plan = PlanAgentSessionRestore(true, four, sessions, 3);
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, plan.status);
	ASSERT_EQ(2U, PlannedSessions(plan).size());
	EXPECT_EQ(L"0f1e2d3c-4b5a-4968-8776-655443322110", PlannedSessions(plan)[0].id);
	EXPECT_EQ(L"ninth-tamarind", PlannedSessions(plan)[1].id);
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }), plan.skipped);
	EXPECT_EQ(1U, plan.dropped);
	// No layout: one single-leaf group per surviving session. Phase 2 selected
	// each resumed session in turn, so the last resumed one stays active.
	ASSERT_EQ(2U, plan.groups.size());
	EXPECT_EQ(1U, plan.groups[0].leaves.size());
	EXPECT_EQ(1U, plan.groups[1].leaves.size());
	EXPECT_EQ(std::optional<std::size_t>(1), plan.activeGroup);
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeLeaf);
}

TEST(AgentSessionRestore, SetEntryEvictsTheOldestFoldersToKeepTheRecordInsideItsPayloadBound)
{
	// Eight sessions and eight shells with long start directories make an
	// entry of a few KiB, so 64 of them exceed the 64 KiB record bound.
	const auto bigLayout = [](const std::size_t folderIndex) {
		AgentSessionRestoreEntry entry{ .folder = L"C:\\work\\project" + std::to_wstring(folderIndex) };
		AgentSessionRestoreLayout layout;
		AgentSessionRestoreLayoutNode split{ .orientation = AgentSessionRestoreOrientation::Horizontal };
		for (std::size_t index = 0; index < kMaximumAgentSessionRestoreSessions; ++index) {
			entry.sessions.push_back(L"codex:s" + std::to_wstring(folderIndex) + L"x" + std::to_wstring(index));
			layout.leaves.push_back({ .session = entry.sessions.back() });
		}
		for (std::size_t index = 0; index < kMaximumAgentSessionRestoreShellLeaves; ++index) {
			layout.leaves.push_back({ .shell = std::wstring(L"C:\\") + std::wstring(200, L'd')
				+ std::to_wstring(index) });
		}
		for (std::size_t index = 0; index < layout.leaves.size(); ++index) {
			split.children.push_back(index);
			split.weights.push_back(100);
		}
		AgentSessionRestoreLayoutGroup group;
		for (std::size_t index = 0; index < layout.leaves.size(); ++index) {
			group.nodes.push_back({ .leaf = index });
		}
		group.nodes.push_back(split);
		group.root = group.nodes.size() - 1;
		layout.groups.push_back(std::move(group));
		layout.active = 0;
		entry.layout = std::move(layout);
		return entry;
	};
	AgentSessionRestoreState state;
	for (std::size_t index = 0; index < kMaximumAgentSessionRestoreProjects; ++index) {
		auto entry = bigLayout(index);
		ASSERT_TRUE(SetAgentSessionRestoreEntry(state, entry.folder, entry.sessions, entry.layout));
		// Every accepted state stays encodable: it never silently stops saving.
		ASSERT_TRUE(EncodeAgentSessionRestoreState(state).has_value()) << index;
	}
	// The oldest folders were evicted by size, the newest is the last entry
	// with its layout intact, and the record round-trips.
	EXPECT_LT(state.entries.size(), kMaximumAgentSessionRestoreProjects);
	EXPECT_NE(nullptr, FindAgentSessionRestoreEntry(state, L"C:\\work\\project63"));
	EXPECT_EQ(nullptr, FindAgentSessionRestoreEntry(state, L"C:\\work\\project0"));
	EXPECT_TRUE(state.entries.back().layout.has_value());
	const auto encoded = EncodeAgentSessionRestoreState(state);
	ASSERT_TRUE(encoded.has_value());
	const auto decoded = DecodeAgentSessionRestoreState(*encoded);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(state.entries.size(), decoded->entries.size());
}

TEST(AgentSessionRestore, PlanCollapsesMissingLeavesAndKeepsSurvivingWeights)
{
	// leaf0 claude (survives) | leaf1 codex (missing) over leaf2 shell
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"0f1e2d3c-4b5a-4968-8776-655443322110", 10),
	}));
	AgentSessionRestoreEntry recorded{
		.folder = kFolder,
		.sessions = { kClaude, kCodex },
		.layout = AgentSessionRestoreLayout{
			.leaves = {
				{ .session = std::wstring(kClaude) },
				{ .session = std::wstring(kCodex) },
				{ .shell = std::wstring(L"C:\\work\\sub") },
			},
			.groups = { { .nodes = {
				{ .leaf = 0 },
				{ .leaf = 1 },
				{ .leaf = 2 },
				{ .orientation = AgentSessionRestoreOrientation::Vertical,
					.children = { 1, 2 }, .weights = { 700, 300 } },
				{ .orientation = AgentSessionRestoreOrientation::Horizontal,
					.children = { 0, 3 }, .weights = { 600, 400 } },
			}, .root = 4 } },
			.active = 2,
		},
	};
	const auto plan = PlanAgentSessionRestore(true, recorded, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, plan.status);
	ASSERT_EQ(1U, PlannedSessions(plan).size());
	EXPECT_EQ(L"0f1e2d3c-4b5a-4968-8776-655443322110", PlannedSessions(plan)[0].id);
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }), plan.skipped);
	ASSERT_EQ(1U, plan.groups.size());
	const auto& group = plan.groups[0];
	ASSERT_EQ(2U, group.leaves.size());
	EXPECT_TRUE(group.leaves[0].session.has_value());
	EXPECT_TRUE(group.leaves[1].shellDirectory.has_value());
	// The vertical split lost a child and collapsed: the root stays the
	// horizontal split of the two survivors, weights preserved.
	ASSERT_EQ(3U, group.nodes.size());
	const auto& rootNode = group.nodes[group.root];
	EXPECT_FALSE(rootNode.leaf.has_value());
	EXPECT_EQ(AgentSessionRestoreOrientation::Horizontal, rootNode.orientation);
	EXPECT_EQ(2U, rootNode.children.size());
	EXPECT_EQ((std::vector<int>{ 600, 400 }), rootNode.weights);
	// The recorded active leaf survived: it stays selected.
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeGroup);
	EXPECT_EQ(std::optional<std::size_t>(1), plan.activeLeaf);
}

TEST(AgentSessionRestore, PlanDropsAShellOnlyGroupAndFallsBackToTheFirstSessionLeaf)
{
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 20),
	}));
	AgentSessionRestoreEntry recorded{
		.folder = kFolder,
		.sessions = { kDevin },
		.layout = AgentSessionRestoreLayout{
			.leaves = {
				{ .shell = std::wstring(L"C:\\work\\a") },
				{ .shell = std::wstring(L"C:\\work\\b") },
				{ .session = std::wstring(kDevin) },
			},
			// Group 0 holds shells alone: it is never recreated (open
			// decision 1). Group 1 holds the surviving session leaf.
			.groups = {
				{ .nodes = {
					{ .leaf = 0 },
					{ .leaf = 1 },
					{ .orientation = AgentSessionRestoreOrientation::Horizontal,
						.children = { 0, 1 }, .weights = { 500, 500 } },
				}, .root = 2 },
				{ .nodes = { { .leaf = 2 } }, .root = 0 },
			},
			// The recorded active leaf was a dropped shell: the plan falls
			// back to the first session leaf of the first group.
			.active = 0,
		},
	};
	const auto plan = PlanAgentSessionRestore(true, recorded, sessions);
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, plan.status);
	ASSERT_EQ(1U, plan.groups.size());
	EXPECT_EQ(1U, plan.groups[0].leaves.size());
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeGroup);
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeLeaf);
}

TEST(AgentSessionRestore, PlanDropsSessionLeavesPastTheCapAndEmptyGroupsWithThem)
{
	std::vector<AgentSessionRecord> discovered;
	AgentSessionRestoreLayout layout;
	for (std::size_t index = 0; index < 4; ++index) {
		const auto id = L"s" + std::to_wstring(index);
		discovered.push_back(Session(EAgentSessionProvider::Codex, id.c_str(), 100));
		layout.leaves.push_back({ .session = L"codex:" + id });
		layout.groups.push_back({ .nodes = {
			{ .leaf = index } }, .root = 0 });
	}
	layout.active = 3;	// past the cap: the fallback takes over.
	AgentSessionRestoreEntry recorded{ .folder = kFolder,
		.sessions = { L"codex:s0", L"codex:s1", L"codex:s2", L"codex:s3" },
		.layout = std::move(layout) };
	const auto plan = PlanAgentSessionRestore(true, recorded, discovered, 2);
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, plan.status);
	ASSERT_EQ(2U, PlannedSessions(plan).size());
	EXPECT_EQ(2U, plan.dropped);
	ASSERT_EQ(2U, plan.groups.size());
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeGroup);
	EXPECT_EQ(std::optional<std::size_t>(0), plan.activeLeaf);
}

TEST(AgentSessionRestore, DispatchRunsTheInstallSeamAndCountsEveryOutcome)
{
	const auto sessions = std::optional<std::vector<AgentSessionRecord>>(Sessions({
		Session(EAgentSessionProvider::ClaudeCode, L"0f1e2d3c-4b5a-4968-8776-655443322110", 10),
		Session(EAgentSessionProvider::Devin, L"ninth-tamarind", 20),
	}));
	const AgentSessionRestoreEntry recorded{
		.folder = kFolder, .sessions = { kClaude, kCodex, kDevin } };
	const auto plan = PlanAgentSessionRestore(true, recorded, sessions);

	std::vector<std::wstring> resumed;
	const auto outcome = DispatchAgentSessionRestore(plan,
		[&resumed](const AgentSessionRestorePlan& toInstall) {
			AgentSessionRestoreInstallResult installed;
			for (const auto& group : toInstall.groups) {
				for (const auto& leaf : group.leaves) {
					if (leaf.session
						&& leaf.session->provider == EAgentSessionProvider::ClaudeCode) {
						resumed.push_back(leaf.session->id);
						++installed.resumed;
					} else {
						++installed.failed;
					}
				}
			}
			return installed;
		});
	EXPECT_EQ(EAgentSessionRestoreStatus::Ready, outcome.status);
	EXPECT_EQ(1U, outcome.resumed);
	EXPECT_EQ(1U, outcome.failed);
	EXPECT_EQ(1U, outcome.skipped);
	EXPECT_EQ((std::vector<std::wstring>{ L"0f1e2d3c-4b5a-4968-8776-655443322110" }),
		resumed);

	// A non-Ready plan never touches the seam, a missing seam installs
	// nothing, and a throwing seam counts every planned leaf as failed.
	bool called = false;
	const AgentSessionRestoreEntry one{ .folder = kFolder, .sessions = { kClaude } };
	const auto disabled = DispatchAgentSessionRestore(
		PlanAgentSessionRestore(false, one, sessions),
		[&called](const AgentSessionRestorePlan&) {
			called = true;
			return AgentSessionRestoreInstallResult{};
		});
	EXPECT_EQ(EAgentSessionRestoreStatus::Disabled, disabled.status);
	EXPECT_FALSE(called);
	EXPECT_EQ(0U, DispatchAgentSessionRestore(plan, {}).resumed);
	const auto thrown = DispatchAgentSessionRestore(plan,
		[](const AgentSessionRestorePlan&) -> AgentSessionRestoreInstallResult {
			throw std::runtime_error("install failed");
		});
	EXPECT_EQ(0U, thrown.resumed);
	EXPECT_EQ(2U, thrown.failed);
}

TEST(AgentSessionRestoreService, NothingStoredGivesAnEmptyRecordAndTheFirstChangeIsSaved)
{
	const auto script = std::make_shared<StoreScript>();
	CAgentSessionRestoreService service(std::make_unique<ScriptedStore>(script));
	EXPECT_TRUE(service.Snapshot().entries.empty());
	EXPECT_TRUE(RecordedSessionsOf(service, kFolder).empty());

	service.Load();
	EXPECT_TRUE(script->saved.empty());

	EXPECT_TRUE(service.Update([](AgentSessionRestoreState& state) {
		return SetAgentSessionRestoreEntry(state, kFolder, { kClaude });
	}));
	ASSERT_EQ(1U, script->saved.size());
	EXPECT_EQ(R"({"version":2,"projects":[{"folder":"C:\\work\\project","sessions":[)"
		R"("claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"]}]})", script->saved[0]);
	EXPECT_EQ((std::vector<std::wstring>{ kClaude }), RecordedSessionsOf(service, kFolder));

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
	// The version-1 record loaded from another window is rewritten as
	// version 2 with no layout member.
	EXPECT_EQ(R"({"version":2,"projects":[{"folder":"D:\\other","sessions":["devin:ninth-tamarind"]},)"
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
	EXPECT_EQ((std::vector<std::wstring>{ kCodex }), RecordedSessionsOf(service, kFolder));
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
