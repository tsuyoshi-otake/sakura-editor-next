/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionRestore.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <variant>

namespace workbench::projects {

namespace {

using platform::serialization::JsoncValue;

constexpr std::size_t kMaximumRestorePayloadBytes = 64u * 1024u;
constexpr std::size_t kMaximumRestoreFolderCharacters = 1024;

constexpr std::array<std::wstring_view, 4> kProviderKeys{ L"claude-code", L"codex", L"devin", L"pi" };
constexpr std::array<EAgentSessionProvider, 4> kProviders{
	EAgentSessionProvider::ClaudeCode, EAgentSessionProvider::Codex,
	EAgentSessionProvider::Devin, EAgentSessionProvider::Pi };

//! A valid folder string: non-empty, bounded, well-formed UTF-16, no control
//! characters (they cannot appear in a real path and would complicate the
//! stored record for no benefit).
bool IsValidRestoreFolder(const std::wstring_view folder) noexcept
{
	if (folder.empty() || folder.size() > kMaximumRestoreFolderCharacters) return false;
	for (std::size_t index = 0; index < folder.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(folder[index]);
		if (unit < 0x20U) return false;
		if (unit >= 0xd800U && unit <= 0xdbffU) {
			if (++index == folder.size()) return false;
			const auto low = static_cast<std::uint32_t>(folder[index]);
			if (low < 0xdc00U || low > 0xdfffU) return false;
		} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
			return false;
		}
	}
	return true;
}

bool IsValidSessionList(const std::vector<std::wstring>& sessions) noexcept
{
	if (sessions.size() > kMaximumAgentSessionRestoreSessions) return false;
	for (std::size_t index = 0; index < sessions.size(); ++index) {
		if (!IsValidAgentSessionIdentity(sessions[index])) return false;
		for (std::size_t earlier = 0; earlier < index; ++earlier) {
			if (sessions[earlier] == sessions[index]) return false;
		}
	}
	return true;
}

bool IsValidState(const AgentSessionRestoreState& state) noexcept
{
	if (state.entries.size() > kMaximumAgentSessionRestoreProjects) return false;
	for (std::size_t index = 0; index < state.entries.size(); ++index) {
		const auto& entry = state.entries[index];
		// An empty list means "record nothing" and is never stored: SetEntry
		// removes the row instead, so no valid state carries one.
		if (!IsValidRestoreFolder(entry.folder) || entry.sessions.empty()
			|| !IsValidSessionList(entry.sessions)) return false;
		for (std::size_t earlier = 0; earlier < index; ++earlier) {
			if (IsSameAgentSessionFolder(state.entries[earlier].folder, entry.folder)) return false;
		}
	}
	return true;
}

bool AppendUtf8(std::string& target, const std::uint32_t codePoint)
{
	if (codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return false;
	if (codePoint <= 0x7fU) target.push_back(static_cast<char>(codePoint));
	else if (codePoint <= 0x7ffU) {
		target.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else if (codePoint <= 0xffffU) {
		target.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	} else {
		target.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
		target.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
	}
	return true;
}

//! Folder strings can carry any UTF-16, so they need real JSON string quoting;
//! identities are ASCII by construction (IsValidAgentSessionIdentity) and are
//! appended bare.
bool AppendJsonString(std::string& out, const std::wstring_view value)
{
	out.push_back('"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit == L'"' || unit == L'\\') {
			out.push_back('\\');
			out.push_back(static_cast<char>(unit));
		} else if (unit == L'\b') out += "\\b";
		else if (unit == L'\f') out += "\\f";
		else if (unit == L'\n') out += "\\n";
		else if (unit == L'\r') out += "\\r";
		else if (unit == L'\t') out += "\\t";
		else if (unit <= 0x1fU) {
			static constexpr char hex[] = "0123456789abcdef";
			out += "\\u00";
			out.push_back(hex[(unit >> 4U) & 0xfU]);
			out.push_back(hex[unit & 0xfU]);
		} else if (unit >= 0xd800U && unit <= 0xdbffU) {
			const auto low = static_cast<std::uint32_t>(value[++index]);
			if (!AppendUtf8(out, 0x10000U + ((unit - 0xd800U) << 10U) + (low - 0xdc00U))) return false;
		} else if (!AppendUtf8(out, unit)) return false;
	}
	out.push_back('"');
	return true;
}

void AppendIdentityList(std::string& out, const std::vector<std::wstring>& sessions)
{
	out.push_back('[');
	for (std::size_t index = 0; index < sessions.size(); ++index) {
		if (index != 0) out.push_back(',');
		out.push_back('"');
		for (const wchar_t character : sessions[index]) out.push_back(static_cast<char>(character));
		out.push_back('"');
	}
	out.push_back(']');
}

std::optional<std::wstring> StringMember(const JsoncValue::Object& object,
	const std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	if (const auto* value = std::get_if<std::wstring>(&found->second.Value())) return *value;
	return std::nullopt;
}

} // namespace

std::optional<EAgentSessionProvider> AgentSessionIdentityProvider(
	const std::wstring_view identity) noexcept
{
	if (!IsValidAgentSessionIdentity(identity)) return std::nullopt;
	const auto colon = identity.find(L':');
	if (colon == std::wstring_view::npos) return std::nullopt;
	const auto key = identity.substr(0, colon);
	for (std::size_t index = 0; index < kProviderKeys.size(); ++index) {
		if (kProviderKeys[index] == key) return kProviders[index];
	}
	return std::nullopt;
}

std::vector<std::wstring> RecordLiveAgentSessions(
	const std::span<const LiveAgentSessionSource> agents,
	const std::span<const AgentSessionRecord> sessions,
	const std::size_t maximum)
{
	if (maximum == 0) return {};
	std::vector<std::wstring> recorded;
	const auto claim = [&recorded, maximum](const std::wstring_view identity) {
		if (recorded.size() == maximum) return false;
		if (std::ranges::find(recorded, identity) != recorded.end()) return true;
		recorded.emplace_back(identity);
		return true;
	};
	// Sessions Sakura resumed are known, not inferred; they claim first so a
	// hand-started agent of the same provider cannot take their identity.
	for (const auto& agent : agents) {
		if (!agent.knownSessionIdentity || !IsValidAgentSessionIdentity(*agent.knownSessionIdentity)) {
			continue;
		}
		const auto provider = AgentSessionIdentityProvider(*agent.knownSessionIdentity);
		if (provider && AgentSessionProviderDisplayName(*provider) == agent.name) {
			(void)claim(*agent.knownSessionIdentity);
		}
	}
	for (const auto provider : kProviders) {
		const auto name = AgentSessionProviderDisplayName(provider);
		std::vector<const LiveAgentSessionSource*> inferable;
		for (const auto& agent : agents) {
			if (agent.name != name || agent.startedUnixSeconds <= 0) continue;
			if (agent.knownSessionIdentity && IsValidAgentSessionIdentity(*agent.knownSessionIdentity)
				&& AgentSessionIdentityProvider(*agent.knownSessionIdentity) == provider) {
				continue;	// already recorded above
			}
			inferable.push_back(&agent);
		}
		if (inferable.empty()) continue;
		// The latest agent is the hardest to cover, so it claims the newest
		// session first; a session too old for it may still cover an earlier
		// one (the same arbitration CountAgentsAwaitingSessionHistory uses).
		std::ranges::sort(inferable, [](const auto* left, const auto* right) {
			return left->startedUnixSeconds > right->startedUnixSeconds;
		});
		std::vector<const AgentSessionRecord*> candidates;
		for (const auto& session : sessions) {
			if (session.provider == provider && session.lastActivityUnixSeconds > 0) {
				candidates.push_back(&session);
			}
		}
		std::ranges::sort(candidates, [](const auto* left, const auto* right) {
			return left->lastActivityUnixSeconds > right->lastActivityUnixSeconds;
		});
		std::size_t next = 0;
		for (const auto* agent : inferable) {
			if (next == candidates.size()) break;
			// The newest unclaimed session is too old for this agent only; an
			// earlier-started agent may still claim it, so `next` advances on
			// a successful claim alone.
			if (candidates[next]->lastActivityUnixSeconds >= agent->startedUnixSeconds) {
				(void)claim(AgentSessionIdentity(*candidates[next]));
				++next;
			}
		}
	}
	return recorded;
}

std::span<const std::wstring> FindAgentSessionRestoreEntry(
	const AgentSessionRestoreState& state, const std::wstring_view folder) noexcept
{
	if (folder.empty()) return {};
	const auto found = std::ranges::find_if(state.entries, [folder](const auto& entry) {
		return IsSameAgentSessionFolder(entry.folder, folder);
	});
	return found == state.entries.end()
		? std::span<const std::wstring>{}
		: std::span<const std::wstring>(found->sessions);
}

bool SetAgentSessionRestoreEntry(AgentSessionRestoreState& state, const std::wstring_view folder,
	std::vector<std::wstring> sessions)
{
	if (!IsValidRestoreFolder(folder) || !IsValidSessionList(sessions)) return false;
	const auto found = std::ranges::find_if(state.entries, [folder](const auto& entry) {
		return IsSameAgentSessionFolder(entry.folder, folder);
	});
	if (sessions.empty()) {
		if (found == state.entries.end()) return false;
		state.entries.erase(found);
		return true;
	}
	if (found != state.entries.end()) {
		if (found->folder == folder && found->sessions == sessions) return false;
		// Rewriting an entry moves it to the most recently written position,
		// identical sessions included: a rewrite still means the folder was
		// active most recently.
		state.entries.erase(found);
	}
	else if (state.entries.size() == kMaximumAgentSessionRestoreProjects) {
		state.entries.erase(state.entries.begin());
	}
	state.entries.push_back({ std::wstring(folder), std::move(sessions) });
	return true;
}

std::optional<std::string> EncodeAgentSessionRestoreState(const AgentSessionRestoreState& state)
{
	if (!IsValidState(state)) return std::nullopt;
	std::string result = "{\"version\":" + std::to_string(kAgentSessionRestoreSchemaVersion)
		+ ",\"projects\":[";
	for (std::size_t index = 0; index < state.entries.size(); ++index) {
		if (index != 0) result.push_back(',');
		result += "{\"folder\":";
		if (!AppendJsonString(result, state.entries[index].folder)) return std::nullopt;
		result += ",\"sessions\":";
		AppendIdentityList(result, state.entries[index].sessions);
		result.push_back('}');
		if (result.size() > kMaximumRestorePayloadBytes) return std::nullopt;
	}
	result += "]}";
	return result.size() <= kMaximumRestorePayloadBytes
		? std::optional<std::string>(std::move(result)) : std::nullopt;
}

std::optional<AgentSessionRestoreState> DecodeAgentSessionRestoreState(const std::string_view payload)
{
	if (payload.empty() || payload.size() > kMaximumRestorePayloadBytes) return std::nullopt;
	const auto parsed = platform::serialization::CJsoncDocument::Parse(payload);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* root = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (root == nullptr) return std::nullopt;
	const auto version = root->find(L"version");
	const auto entries = root->find(L"projects");
	if (version == root->end() || entries == root->end()) return std::nullopt;
	const auto* versionValue = std::get_if<std::int64_t>(&version->second.Value());
	const auto* array = std::get_if<JsoncValue::Array>(&entries->second.Value());
	if (versionValue == nullptr || *versionValue != kAgentSessionRestoreSchemaVersion
		|| array == nullptr || array->size() > kMaximumAgentSessionRestoreProjects) {
		return std::nullopt;
	}
	AgentSessionRestoreState state;
	state.entries.reserve(array->size());
	for (const auto& value : *array) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return std::nullopt;
		const auto folder = StringMember(*object, L"folder");
		const auto sessionsValue = object->find(L"sessions");
		if (!folder || sessionsValue == object->end()) return std::nullopt;
		const auto* sessionsArray = std::get_if<JsoncValue::Array>(&sessionsValue->second.Value());
		if (sessionsArray == nullptr
			|| sessionsArray->size() > kMaximumAgentSessionRestoreSessions) return std::nullopt;
		AgentSessionRestoreEntry entry{ .folder = std::move(*folder) };
		entry.sessions.reserve(sessionsArray->size());
		for (const auto& item : *sessionsArray) {
			const auto* identity = std::get_if<std::wstring>(&item.Value());
			if (identity == nullptr) return std::nullopt;
			entry.sessions.push_back(*identity);
		}
		state.entries.push_back(std::move(entry));
	}
	if (!IsValidState(state)) return std::nullopt;
	return state;
}

AgentSessionRestorePlan PlanAgentSessionRestore(
	const bool enabled,
	const std::span<const std::wstring> recorded,
	const std::optional<std::vector<AgentSessionRecord>>& sessions,
	const std::size_t maximum)
{
	AgentSessionRestorePlan plan;
	if (!enabled) {
		plan.status = EAgentSessionRestoreStatus::Disabled;
		return plan;
	}
	if (recorded.empty()) {
		plan.status = EAgentSessionRestoreStatus::NothingRecorded;
		return plan;
	}
	if (!sessions) {
		plan.status = EAgentSessionRestoreStatus::DiscoveryFailed;
		return plan;
	}
	for (std::size_t index = 0; index < recorded.size(); ++index) {
		if (index == maximum) {
			plan.dropped = recorded.size() - maximum;
			break;
		}
		const auto found = std::ranges::find_if(*sessions, [&recorded, index](const auto& session) {
			return AgentSessionIdentity(session) == recorded[index];
		});
		if (found == sessions->end()) plan.skipped.push_back(recorded[index]);
		else plan.resume.push_back(*found);
	}
	return plan;
}

AgentSessionRestoreOutcome DispatchAgentSessionRestore(
	const AgentSessionRestorePlan& plan,
	const std::function<bool(const AgentSessionRecord& session)>& resume)
{
	AgentSessionRestoreOutcome outcome{
		.status = plan.status,
		.skipped = plan.skipped.size(),
		.dropped = plan.dropped,
	};
	if (plan.status != EAgentSessionRestoreStatus::Ready || !resume) return outcome;
	for (const auto& session : plan.resume) {
		try {
			if (resume(session)) ++outcome.resumed;
			else ++outcome.failed;
		} catch (...) {
			// One failed resume never stops the rest of this attempt.
			++outcome.failed;
		}
	}
	return outcome;
}

} // namespace workbench::projects
