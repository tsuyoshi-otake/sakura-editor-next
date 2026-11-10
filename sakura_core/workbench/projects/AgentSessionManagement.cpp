/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionManagement.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <variant>

namespace workbench::projects {

namespace {

using platform::serialization::JsoncValue;

constexpr std::size_t kMaximumViewStatePayloadBytes = 128u * 1024u;

constexpr std::array<std::wstring_view, 4> kProviderKeys{ L"claude-code", L"codex", L"devin", L"pi" };

bool Contains(const std::vector<std::wstring>& list, const std::wstring_view identity) noexcept
{
	return std::ranges::find(list, identity) != list.end();
}

bool Remove(std::vector<std::wstring>& list, const std::wstring_view identity)
{
	const auto found = std::ranges::find(list, identity);
	if (found == list.end()) return false;
	list.erase(found);
	return true;
}

//! Adds `identity` to `target` and removes it from `other`. Refused (no
//! change) when `target` is full; `other` is only touched on success.
bool Mark(std::vector<std::wstring>& target, std::vector<std::wstring>& other, const std::wstring_view identity)
{
	if (!IsValidAgentSessionIdentity(identity) || Contains(target, identity)) return false;
	if (target.size() >= kMaximumAgentSessionMarks) return false;
	target.emplace_back(identity);
	(void)Remove(other, identity);
	return true;
}

bool IsValidList(const std::vector<std::wstring>& list) noexcept
{
	if (list.size() > kMaximumAgentSessionMarks) return false;
	for (std::size_t index = 0; index < list.size(); ++index) {
		if (!IsValidAgentSessionIdentity(list[index])) return false;
		for (std::size_t earlier = 0; earlier < index; ++earlier) {
			if (list[earlier] == list[index]) return false;
		}
	}
	return true;
}

bool IsValidState(const AgentSessionViewState& state) noexcept
{
	if (!IsAgentSessionDisplayCount(state.displayCount) || !IsValidList(state.pinned)
		|| !IsValidList(state.archived)) return false;
	return std::ranges::none_of(state.pinned, [&state](const auto& identity) {
		return Contains(state.archived, identity);
	});
}

//! Identities are ASCII by construction (IsValidAgentSessionIdentity), so
//! they need no JSON escaping.
void AppendList(std::string& out, const std::vector<std::wstring>& list)
{
	out.push_back('[');
	for (std::size_t index = 0; index < list.size(); ++index) {
		if (index != 0) out.push_back(',');
		out.push_back('"');
		for (const wchar_t character : list[index]) out.push_back(static_cast<char>(character));
		out.push_back('"');
	}
	out.push_back(']');
}

std::optional<std::vector<std::wstring>> DecodeList(const JsoncValue::Object& root, const std::wstring_view key)
{
	const auto found = root.find(key);
	if (found == root.end()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&found->second.Value());
	if (array == nullptr || array->size() > kMaximumAgentSessionMarks) return std::nullopt;
	std::vector<std::wstring> result;
	result.reserve(array->size());
	for (const auto& value : *array) {
		const auto* text = std::get_if<std::wstring>(&value.Value());
		if (text == nullptr) return std::nullopt;
		result.push_back(*text);
	}
	return result;
}

AgentSessionMenuItem Item(const EAgentSessionCommand command, const bool separatorBefore = false)
{
	return { .command = command, .separatorBefore = separatorBefore };
}

void AppendShowCounts(std::vector<AgentSessionMenuItem>& menu, const std::size_t displayCount, const bool separatorBefore)
{
	bool first = true;
	for (const auto count : kAgentSessionDisplayCounts) {
		menu.push_back({ .command = EAgentSessionCommand::ShowCount, .displayCount = count,
			.checked = count == displayCount, .separatorBefore = separatorBefore && first });
		first = false;
	}
}

} // namespace

bool IsAgentSessionDisplayCount(const std::size_t count) noexcept
{
	return std::ranges::find(kAgentSessionDisplayCounts, count) != kAgentSessionDisplayCounts.end();
}

bool IsValidAgentSessionIdentity(const std::wstring_view identity) noexcept
{
	const auto colon = identity.find(L':');
	if (colon == std::wstring_view::npos) return false;
	const auto key = identity.substr(0, colon);
	return std::ranges::find(kProviderKeys, key) != kProviderKeys.end()
		&& IsValidAgentSessionId(identity.substr(colon + 1));
}

bool IsAgentSessionPinned(const AgentSessionViewState& state, const std::wstring_view identity) noexcept
{
	return Contains(state.pinned, identity);
}

bool IsAgentSessionArchived(const AgentSessionViewState& state, const std::wstring_view identity) noexcept
{
	return Contains(state.archived, identity);
}

bool PinAgentSession(AgentSessionViewState& state, const std::wstring_view identity)
{
	return Mark(state.pinned, state.archived, identity);
}

bool UnpinAgentSession(AgentSessionViewState& state, const std::wstring_view identity)
{
	return Remove(state.pinned, identity);
}

bool ArchiveAgentSession(AgentSessionViewState& state, const std::wstring_view identity)
{
	return Mark(state.archived, state.pinned, identity);
}

bool UnarchiveAgentSession(AgentSessionViewState& state, const std::wstring_view identity)
{
	return Remove(state.archived, identity);
}

bool ForgetAgentSession(AgentSessionViewState& state, const std::wstring_view identity)
{
	const bool unpinned = Remove(state.pinned, identity);
	const bool unarchived = Remove(state.archived, identity);
	return unpinned || unarchived;
}

bool SetAgentSessionDisplayCount(AgentSessionViewState& state, const std::size_t count)
{
	if (!IsAgentSessionDisplayCount(count) || state.displayCount == count) return false;
	state.displayCount = count;
	return true;
}

std::optional<std::string> EncodeAgentSessionViewState(const AgentSessionViewState& state)
{
	if (!IsValidState(state)) return std::nullopt;
	std::string result = "{\"version\":" + std::to_string(kAgentSessionViewStateSchemaVersion)
		+ ",\"displayCount\":" + std::to_string(state.displayCount) + ",\"pinned\":";
	AppendList(result, state.pinned);
	result += ",\"archived\":";
	AppendList(result, state.archived);
	result.push_back('}');
	if (result.size() > kMaximumViewStatePayloadBytes) return std::nullopt;
	return result;
}

std::optional<AgentSessionViewState> DecodeAgentSessionViewState(const std::string_view payload)
{
	if (payload.empty() || payload.size() > kMaximumViewStatePayloadBytes) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::Parse(payload);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* root = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (root == nullptr) return std::nullopt;
	const auto version = root->find(L"version");
	const auto count = root->find(L"displayCount");
	if (version == root->end() || count == root->end()) return std::nullopt;
	const auto* versionValue = std::get_if<std::int64_t>(&version->second.Value());
	const auto* countValue = std::get_if<std::int64_t>(&count->second.Value());
	if (versionValue == nullptr || *versionValue != kAgentSessionViewStateSchemaVersion
		|| countValue == nullptr || *countValue <= 0) return std::nullopt;
	auto pinned = DecodeList(*root, L"pinned");
	auto archived = DecodeList(*root, L"archived");
	if (!pinned || !archived) return std::nullopt;
	AgentSessionViewState state{
		.displayCount = static_cast<std::size_t>(*countValue),
		.pinned = std::move(*pinned),
		.archived = std::move(*archived),
	};
	if (!IsValidState(state)) return std::nullopt;
	return state;
}

std::wstring_view AgentSessionCommandId(const EAgentSessionCommand command) noexcept
{
	switch (command) {
	case EAgentSessionCommand::Resume: return L"sakura.agentSession.resume";
	case EAgentSessionCommand::Rename: return L"agentSession.rename";
	case EAgentSessionCommand::Pin: return L"agentSession.pin";
	case EAgentSessionCommand::Unpin: return L"agentSession.unpin";
	case EAgentSessionCommand::Archive: return L"agentSession.archive";
	case EAgentSessionCommand::Unarchive: return L"agentSession.unarchive";
	case EAgentSessionCommand::CopyId: return L"sakura.agentSession.copyId";
	case EAgentSessionCommand::CopyResumeCommand: return L"sakura.agentSession.copyResumeCommand";
	case EAgentSessionCommand::Delete: return L"agentSession.delete";
	case EAgentSessionCommand::ShowCount: return L"sakura.agentSessions.setDisplayCount";
	}
	return {};
}

bool CanRenameAgentSession(const EAgentSessionProvider provider) noexcept
{
	return provider != EAgentSessionProvider::Devin;
}

bool DeletesAgentSessionPermanently(const EAgentSessionProvider provider) noexcept
{
	return provider == EAgentSessionProvider::Codex || provider == EAgentSessionProvider::Devin;
}

std::vector<AgentSessionMenuItem> BuildAgentSessionMenu(const EAgentSessionProvider provider,
	const bool pinned, const bool archived, const std::size_t displayCount)
{
	std::vector<AgentSessionMenuItem> menu;
	menu.push_back(Item(EAgentSessionCommand::Resume));
	bool separator = true;
	if (CanRenameAgentSession(provider)) {
		menu.push_back(Item(EAgentSessionCommand::Rename, separator));
		separator = false;
	}
	if (!archived) {
		menu.push_back(Item(pinned ? EAgentSessionCommand::Unpin : EAgentSessionCommand::Pin, separator));
		separator = false;
	}
	menu.push_back(Item(archived ? EAgentSessionCommand::Unarchive : EAgentSessionCommand::Archive, separator));
	menu.push_back(Item(EAgentSessionCommand::CopyId, true));
	menu.push_back(Item(EAgentSessionCommand::CopyResumeCommand));
	menu.push_back(Item(EAgentSessionCommand::Delete, true));
	AppendShowCounts(menu, displayCount, true);
	return menu;
}

std::vector<AgentSessionMenuItem> BuildAgentSessionsGroupMenu(const std::size_t displayCount)
{
	std::vector<AgentSessionMenuItem> menu;
	AppendShowCounts(menu, displayCount, false);
	return menu;
}

} // namespace workbench::projects
