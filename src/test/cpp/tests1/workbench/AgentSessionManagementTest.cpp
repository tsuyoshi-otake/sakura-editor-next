/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/projects/AgentSessionManagement.h"
#include "workbench/projects/AgentSessionViewStateService.h"

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace workbench::projects;

constexpr wchar_t kFirst[] = L"claude-code:0f1e2d3c-4b5a-4968-8776-655443322110";
constexpr wchar_t kSecond[] = L"codex:019a0000-0000-7000-8000-000000000001";
constexpr wchar_t kThird[] = L"pi:019a1111-2222-7333-8444-555566667777";

using Command = EAgentSessionCommand;

std::vector<Command> Commands(const std::vector<AgentSessionMenuItem>& menu)
{
	std::vector<Command> commands;
	for (const auto& item : menu) commands.push_back(item.command);
	return commands;
}

std::vector<Command> SeparatedCommands(const std::vector<AgentSessionMenuItem>& menu)
{
	std::vector<Command> commands;
	for (const auto& item : menu) {
		if (item.separatorBefore) commands.push_back(item.command);
	}
	return commands;
}

//! Shared with the test so results can be scripted after the service owns the store.
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

TEST(AgentSessionManagement, IdentitiesAndDisplayCountsAreValidated)
{
	EXPECT_TRUE(IsValidAgentSessionIdentity(kFirst));
	EXPECT_TRUE(IsValidAgentSessionIdentity(kSecond));
	EXPECT_TRUE(IsValidAgentSessionIdentity(kThird));
	EXPECT_TRUE(IsValidAgentSessionIdentity(L"devin:ninth-tamarind"));
	for (const std::wstring_view identity : { std::wstring_view(L""), std::wstring_view(L"claude-code:"),
		std::wstring_view(L"claude-code"), std::wstring_view(L"gemini:abc"), std::wstring_view(L":abc"),
		std::wstring_view(L"codex:a b"), std::wstring_view(L"codex:-flag"), std::wstring_view(L"#archived"),
		std::wstring_view(L"Codex:abc") }) {
		EXPECT_FALSE(IsValidAgentSessionIdentity(identity)) << std::wstring(identity).size();
	}

	for (const std::size_t count : kAgentSessionDisplayCounts) EXPECT_TRUE(IsAgentSessionDisplayCount(count));
	EXPECT_TRUE(IsAgentSessionDisplayCount(kDefaultAgentSessionDisplayCount));
	EXPECT_FALSE(IsAgentSessionDisplayCount(0));
	EXPECT_FALSE(IsAgentSessionDisplayCount(15));
	EXPECT_FALSE(IsAgentSessionDisplayCount(100));
}

TEST(AgentSessionManagement, PinAndArchiveAreExclusiveAndRefuseNoOpsInvalidIdentitiesAndFullLists)
{
	AgentSessionViewState state;
	EXPECT_EQ(kDefaultAgentSessionDisplayCount, state.displayCount);

	EXPECT_TRUE(PinAgentSession(state, kFirst));
	EXPECT_FALSE(PinAgentSession(state, kFirst));
	EXPECT_TRUE(PinAgentSession(state, kSecond));
	EXPECT_EQ((std::vector<std::wstring>{ kFirst, kSecond }), state.pinned);
	EXPECT_TRUE(IsAgentSessionPinned(state, kFirst));
	EXPECT_FALSE(IsAgentSessionArchived(state, kFirst));

	// Archiving unpins; pinning unarchives.
	EXPECT_TRUE(ArchiveAgentSession(state, kFirst));
	EXPECT_FALSE(IsAgentSessionPinned(state, kFirst));
	EXPECT_TRUE(IsAgentSessionArchived(state, kFirst));
	EXPECT_FALSE(ArchiveAgentSession(state, kFirst));
	EXPECT_TRUE(PinAgentSession(state, kFirst));
	EXPECT_TRUE(state.archived.empty());
	EXPECT_EQ((std::vector<std::wstring>{ kSecond, kFirst }), state.pinned);

	EXPECT_TRUE(UnpinAgentSession(state, kSecond));
	EXPECT_FALSE(UnpinAgentSession(state, kSecond));
	EXPECT_FALSE(UnarchiveAgentSession(state, kSecond));
	EXPECT_TRUE(ArchiveAgentSession(state, kThird));
	EXPECT_TRUE(UnarchiveAgentSession(state, kThird));

	const auto before = state;
	EXPECT_FALSE(PinAgentSession(state, L"gemini:abc"));
	EXPECT_FALSE(ArchiveAgentSession(state, L""));
	EXPECT_FALSE(ForgetAgentSession(state, kThird));
	EXPECT_EQ(before, state);

	EXPECT_TRUE(ArchiveAgentSession(state, kThird));
	EXPECT_TRUE(ForgetAgentSession(state, kFirst));
	EXPECT_TRUE(ForgetAgentSession(state, kThird));
	EXPECT_TRUE(state.pinned.empty());
	EXPECT_TRUE(state.archived.empty());

	EXPECT_TRUE(SetAgentSessionDisplayCount(state, 50));
	EXPECT_FALSE(SetAgentSessionDisplayCount(state, 50));
	EXPECT_FALSE(SetAgentSessionDisplayCount(state, 15));
	EXPECT_EQ(50U, state.displayCount);

	AgentSessionViewState full;
	for (std::size_t index = 0; index < kMaximumAgentSessionMarks; ++index) {
		ASSERT_TRUE(PinAgentSession(full, L"codex:s" + std::to_wstring(index)));
	}
	EXPECT_FALSE(PinAgentSession(full, kFirst));
	EXPECT_EQ(kMaximumAgentSessionMarks, full.pinned.size());
	// The other list has room of its own, and moving a mark frees its slot.
	EXPECT_TRUE(ArchiveAgentSession(full, kFirst));
	EXPECT_TRUE(ArchiveAgentSession(full, L"codex:s0"));
	EXPECT_EQ(kMaximumAgentSessionMarks - 1U, full.pinned.size());
	EXPECT_TRUE(PinAgentSession(full, kFirst));
	EXPECT_TRUE(EncodeAgentSessionViewState(full).has_value());
}

TEST(AgentSessionManagement, CodecRoundTripsAndRejectsEverythingDecodeWouldNotProduce)
{
	EXPECT_EQ(std::optional<std::string>(R"({"version":1,"displayCount":20,"pinned":[],"archived":[]})"),
		EncodeAgentSessionViewState({}));

	AgentSessionViewState state{ .displayCount = 30, .pinned = { kFirst, kSecond }, .archived = { kThird } };
	const auto encoded = EncodeAgentSessionViewState(state);
	ASSERT_TRUE(encoded.has_value());
	EXPECT_EQ(R"({"version":1,"displayCount":30,"pinned":["claude-code:0f1e2d3c-4b5a-4968-8776-655443322110",)"
		R"("codex:019a0000-0000-7000-8000-000000000001"],"archived":["pi:019a1111-2222-7333-8444-555566667777"]})",
		*encoded);
	EXPECT_EQ(std::optional<AgentSessionViewState>(state), DecodeAgentSessionViewState(*encoded));

	for (const std::string_view payload : {
		std::string_view(""),
		std::string_view("[]"),
		std::string_view("{"),
		std::string_view(R"({"displayCount":20,"pinned":[],"archived":[]})"),
		std::string_view(R"({"version":2,"displayCount":20,"pinned":[],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":15,"pinned":[],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":"20","pinned":[],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":20,"pinned":["gemini:x"],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":20,"pinned":[7],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":20,"pinned":["codex:x","codex:x"],"archived":[]})"),
		std::string_view(R"({"version":1,"displayCount":20,"pinned":["codex:x"],"archived":["codex:x"]})"),
		std::string_view(R"({"version":1,"displayCount":20,"pinned":{},"archived":[]})") }) {
		EXPECT_FALSE(DecodeAgentSessionViewState(payload).has_value()) << payload;
	}

	// Encode refuses what Decode refuses.
	EXPECT_FALSE(EncodeAgentSessionViewState({ .displayCount = 15 }).has_value());
	EXPECT_FALSE(EncodeAgentSessionViewState({ .pinned = { L"codex:x" }, .archived = { L"codex:x" } }).has_value());
	EXPECT_FALSE(EncodeAgentSessionViewState({ .pinned = { L"codex:x", L"codex:x" } }).has_value());
	EXPECT_FALSE(EncodeAgentSessionViewState({ .archived = { L"nope" } }).has_value());

	std::string oversized = R"({"version":1,"displayCount":20,"pinned":[)";
	for (std::size_t index = 0; index <= kMaximumAgentSessionMarks; ++index) {
		if (index != 0) oversized += ',';
		oversized += "\"codex:s" + std::to_string(index) + "\"";
	}
	oversized += R"(],"archived":[]})";
	EXPECT_FALSE(DecodeAgentSessionViewState(oversized).has_value());
}

TEST(AgentSessionManagement, CommandIdsUseTheVsCodeAgentSessionIdsWhereTheyExist)
{
	EXPECT_EQ(L"sakura.agentSession.resume", AgentSessionCommandId(Command::Resume));
	EXPECT_EQ(L"agentSession.rename", AgentSessionCommandId(Command::Rename));
	EXPECT_EQ(L"agentSession.pin", AgentSessionCommandId(Command::Pin));
	EXPECT_EQ(L"agentSession.unpin", AgentSessionCommandId(Command::Unpin));
	EXPECT_EQ(L"agentSession.archive", AgentSessionCommandId(Command::Archive));
	EXPECT_EQ(L"agentSession.unarchive", AgentSessionCommandId(Command::Unarchive));
	EXPECT_EQ(L"agentSession.delete", AgentSessionCommandId(Command::Delete));
	EXPECT_EQ(L"sakura.agentSession.copyId", AgentSessionCommandId(Command::CopyId));
	EXPECT_EQ(L"sakura.agentSession.copyResumeCommand", AgentSessionCommandId(Command::CopyResumeCommand));
	EXPECT_EQ(L"sakura.agentSessions.setDisplayCount", AgentSessionCommandId(Command::ShowCount));
}

TEST(AgentSessionManagement, SessionMenuFollowsTheProviderAndTheRowState)
{
	const std::vector<Command> counts(4, Command::ShowCount);
	const auto withCounts = [&counts](std::vector<Command> commands) {
		commands.insert(commands.end(), counts.begin(), counts.end());
		return commands;
	};

	const auto plain = BuildAgentSessionMenu(EAgentSessionProvider::ClaudeCode, false, false, 20);
	EXPECT_EQ(withCounts({ Command::Resume, Command::Rename, Command::Pin, Command::Archive, Command::CopyId,
		Command::CopyResumeCommand, Command::Delete }), Commands(plain));
	EXPECT_EQ((std::vector<Command>{ Command::Rename, Command::CopyId, Command::Delete, Command::ShowCount }),
		SeparatedCommands(plain));
	// The four counts, in order, with exactly the current one checked.
	ASSERT_EQ(11U, plain.size());
	for (std::size_t index = 0; index < kAgentSessionDisplayCounts.size(); ++index) {
		const auto& item = plain[7 + index];
		EXPECT_EQ(kAgentSessionDisplayCounts[index], item.displayCount);
		EXPECT_EQ(item.displayCount == 20, item.checked);
		EXPECT_EQ(index == 0, item.separatorBefore);
	}
	for (std::size_t index = 0; index < 7; ++index) {
		EXPECT_FALSE(plain[index].checked);
		EXPECT_EQ(0U, plain[index].displayCount);
	}

	EXPECT_EQ(withCounts({ Command::Resume, Command::Rename, Command::Unpin, Command::Archive, Command::CopyId,
		Command::CopyResumeCommand, Command::Delete }),
		Commands(BuildAgentSessionMenu(EAgentSessionProvider::Codex, true, false, 10)));

	// An archived row offers Unarchive and no Pin.
	const auto archived = BuildAgentSessionMenu(EAgentSessionProvider::Pi, false, true, 50);
	EXPECT_EQ(withCounts({ Command::Resume, Command::Rename, Command::Unarchive, Command::CopyId,
		Command::CopyResumeCommand, Command::Delete }), Commands(archived));
	EXPECT_TRUE(archived.back().checked);

	// Devin has no rename, so the separator moves to the next entry.
	const auto devin = BuildAgentSessionMenu(EAgentSessionProvider::Devin, false, false, 20);
	EXPECT_EQ(withCounts({ Command::Resume, Command::Pin, Command::Archive, Command::CopyId,
		Command::CopyResumeCommand, Command::Delete }), Commands(devin));
	EXPECT_EQ((std::vector<Command>{ Command::Pin, Command::CopyId, Command::Delete, Command::ShowCount }),
		SeparatedCommands(devin));
	const auto devinArchived = BuildAgentSessionMenu(EAgentSessionProvider::Devin, false, true, 20);
	EXPECT_EQ((std::vector<Command>{ Command::Unarchive, Command::CopyId, Command::Delete, Command::ShowCount }),
		SeparatedCommands(devinArchived));

	EXPECT_TRUE(CanRenameAgentSession(EAgentSessionProvider::ClaudeCode));
	EXPECT_TRUE(CanRenameAgentSession(EAgentSessionProvider::Codex));
	EXPECT_TRUE(CanRenameAgentSession(EAgentSessionProvider::Pi));
	EXPECT_FALSE(CanRenameAgentSession(EAgentSessionProvider::Devin));
	EXPECT_FALSE(DeletesAgentSessionPermanently(EAgentSessionProvider::ClaudeCode));
	EXPECT_FALSE(DeletesAgentSessionPermanently(EAgentSessionProvider::Pi));
	EXPECT_TRUE(DeletesAgentSessionPermanently(EAgentSessionProvider::Codex));
	EXPECT_TRUE(DeletesAgentSessionPermanently(EAgentSessionProvider::Devin));
}

TEST(AgentSessionManagement, GroupMenuOffersOnlyTheDisplayCounts)
{
	const auto menu = BuildAgentSessionsGroupMenu(30);
	ASSERT_EQ(kAgentSessionDisplayCounts.size(), menu.size());
	for (std::size_t index = 0; index < menu.size(); ++index) {
		EXPECT_EQ(Command::ShowCount, menu[index].command);
		EXPECT_EQ(kAgentSessionDisplayCounts[index], menu[index].displayCount);
		EXPECT_EQ(menu[index].displayCount == 30, menu[index].checked);
		EXPECT_FALSE(menu[index].separatorBefore);
	}
}

TEST(AgentSessionViewStateService, NothingStoredGivesTheDefaultsAndTheFirstChangeIsSaved)
{
	const auto script = std::make_shared<StoreScript>();
	CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));
	EXPECT_EQ(AgentSessionViewState{}, service.Snapshot());

	service.Load();
	EXPECT_EQ(AgentSessionViewState{}, service.Snapshot());
	EXPECT_TRUE(script->saved.empty());

	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
	ASSERT_EQ(1U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"displayCount":20,"pinned":["claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"],)"
		R"("archived":[]})", script->saved[0]);
	EXPECT_TRUE(IsAgentSessionPinned(service.Snapshot(), kFirst));

	// A change that changes nothing is not saved, and neither is a missing one.
	EXPECT_FALSE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
	EXPECT_FALSE(service.Update({}));
	EXPECT_EQ(1U, script->saved.size());
	EXPECT_EQ(1, script->loadCalls);
}

TEST(AgentSessionViewStateService, LoadsTheStoredStateAndUpdatesFromIt)
{
	const auto script = std::make_shared<StoreScript>();
	script->loads.push_back(Stored(R"({"version":1,"displayCount":50,"pinned":["codex:a"],"archived":["pi:b"]})"));
	CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));

	// The first Update of a window that never loaded reads the store first.
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return SetAgentSessionDisplayCount(state, 10); }));
	EXPECT_EQ(1, script->loadCalls);
	ASSERT_EQ(1U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"displayCount":10,"pinned":["codex:a"],"archived":["pi:b"]})", script->saved[0]);
}

TEST(AgentSessionViewStateService, AnUndecodableRecordGivesTheDefaultsAndIsNeverOverwritten)
{
	for (const char* stored : { "not json", R"({"version":9,"displayCount":20,"pinned":[],"archived":[]})" }) {
		const auto script = std::make_shared<StoreScript>();
		// Every load, including the one each Update retries, sees the same record.
		for (int index = 0; index < 4; ++index) script->loads.push_back(Stored(stored));
		CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));
		service.Load();
		EXPECT_EQ(AgentSessionViewState{}, service.Snapshot());

		// The change holds in this window but nothing is written.
		EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
		EXPECT_TRUE(script->saved.empty()) << stored;
	}

	// A Succeeded load without a payload is not a usable record either.
	const auto script = std::make_shared<StoreScript>();
	for (int index = 0; index < 2; ++index) {
		script->loads.push_back({ EProjectCatalogStoreLoadStatus::Succeeded, std::nullopt, {} });
	}
	CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));
	service.Load();
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
	EXPECT_TRUE(script->saved.empty());
}

TEST(AgentSessionViewStateService, AnUnavailableStoreKeepsTheWindowStateAndSavesOnceItLoads)
{
	const auto script = std::make_shared<StoreScript>();
	script->loads.push_back({ EProjectCatalogStoreLoadStatus::Unavailable, std::nullopt, {} });
	script->loads.push_back({ EProjectCatalogStoreLoadStatus::Failed, std::nullopt, {} });
	CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));
	service.Load();

	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
	EXPECT_TRUE(script->saved.empty());
	EXPECT_TRUE(IsAgentSessionPinned(service.Snapshot(), kFirst));

	// The store is ready now: the next Update loads what is stored and saves.
	script->loads.push_back(Stored(R"({"version":1,"displayCount":30,"pinned":[],"archived":[]})"));
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return ArchiveAgentSession(state, kSecond); }));
	ASSERT_EQ(1U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"displayCount":30,"pinned":[],"archived":["codex:019a0000-0000-7000-8000-000000000001"]})",
		script->saved[0]);

	// A failed save keeps the change in this window.
	script->saves.push_back(EProjectCatalogStoreSaveStatus::Failed);
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kThird); }));
	EXPECT_TRUE(IsAgentSessionPinned(service.Snapshot(), kThird));
	EXPECT_EQ(2U, script->saved.size());
}

TEST(AgentSessionViewStateService, AConflictReloadsTheOtherWindowsStateAndAppliesTheChangeToIt)
{
	const auto script = std::make_shared<StoreScript>();
	CAgentSessionViewStateService service(std::make_unique<ScriptedStore>(script));
	service.Load();

	// Another window pinned kSecond and chose 50 rows before this save landed.
	script->saves.push_back(EProjectCatalogStoreSaveStatus::Conflict);
	script->loads.push_back(Stored(
		R"({"version":1,"displayCount":50,"pinned":["codex:019a0000-0000-7000-8000-000000000001"],"archived":[]})"));
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));

	ASSERT_EQ(2U, script->saved.size());
	EXPECT_EQ(R"({"version":1,"displayCount":50,"pinned":["codex:019a0000-0000-7000-8000-000000000001",)"
		R"("claude-code:0f1e2d3c-4b5a-4968-8776-655443322110"],"archived":[]})", script->saved[1]);
	const auto merged = service.Snapshot();
	EXPECT_EQ(50U, merged.displayCount);
	EXPECT_EQ((std::vector<std::wstring>{ kSecond, kFirst }), merged.pinned);

	// When the other window already made the same change, nothing more is written.
	script->saves.push_back(EProjectCatalogStoreSaveStatus::Conflict);
	script->loads.push_back(Stored(
		R"({"version":1,"displayCount":10,"pinned":[],"archived":["pi:019a1111-2222-7333-8444-555566667777"]})"));
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return ArchiveAgentSession(state, kThird); }));
	EXPECT_EQ(3U, script->saved.size());
	EXPECT_EQ(10U, service.Snapshot().displayCount);
	EXPECT_EQ((std::vector<std::wstring>{ kThird }), service.Snapshot().archived);
}

TEST(AgentSessionViewStateService, WithoutAStoreTheStateLivesInTheWindowOnly)
{
	CAgentSessionViewStateService service(nullptr);
	service.Load();
	EXPECT_TRUE(service.Update([](AgentSessionViewState& state) { return PinAgentSession(state, kFirst); }));
	EXPECT_TRUE(IsAgentSessionPinned(service.Snapshot(), kFirst));
}
