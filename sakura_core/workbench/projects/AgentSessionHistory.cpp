/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionHistory.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <climits>
#include <cwctype>
#include <set>
#include <tuple>
#include <utility>

namespace workbench::projects {
namespace {

using JsonArray = picojson::array;
using JsonObject = picojson::object;
using JsonValue = picojson::value;

constexpr std::size_t kMaximumJsonNestingDepth = 64;
constexpr std::size_t kMaximumDevinSessions = 4096;

bool HasBoundedJsonNesting(const std::string_view payload) noexcept
{
	std::size_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (const char character : payload) {
		if (inString) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == '"') inString = false;
			continue;
		}
		if (character == '"') {
			inString = true;
		} else if (character == '{' || character == '[') {
			if (++depth > kMaximumJsonNestingDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && depth == 0;
}

//! One complete JSON value, or nothing. Never throws.
std::optional<JsonValue> ParseJson(std::string_view text) noexcept
{
	try {
		while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
			text.remove_suffix(1);
		}
		if (text.empty() || !HasBoundedJsonNesting(text)) return std::nullopt;
		JsonValue value;
		std::string error;
		auto position = picojson::parse(value, text.begin(), text.end(), &error);
		while (position != text.end() && (*position == ' ' || *position == '\t')) ++position;
		if (!error.empty() || position != text.end()) return std::nullopt;
		return value;
	} catch (...) {
		return std::nullopt;
	}
}

template <typename Visitor>
void ForEachJsonLine(const std::string_view text, Visitor&& visitor)
{
	std::size_t start = 0;
	while (start < text.size()) {
		auto end = text.find('\n', start);
		if (end == std::string_view::npos) end = text.size();
		const auto line = text.substr(start, end - start);
		start = end + 1;
		if (line.empty() || line.front() != '{') continue;
		if (auto value = ParseJson(line); value && value->is<JsonObject>()) {
			if (!visitor(value->get<JsonObject>())) return;
		}
	}
}

const JsonValue* Find(const JsonObject& object, const std::string_view key)
{
	const auto found = object.find(std::string(key));
	return found == object.end() ? nullptr : &found->second;
}

const JsonObject* FindObject(const JsonObject& object, const std::string_view key)
{
	const auto* value = Find(object, key);
	return value != nullptr && value->is<JsonObject>() ? &value->get<JsonObject>() : nullptr;
}

const std::string* FindString(const JsonObject& object, const std::string_view key)
{
	const auto* value = Find(object, key);
	return value != nullptr && value->is<std::string>() ? &value->get<std::string>() : nullptr;
}

//! Invalid sequences decode to U+FFFD; display text never fails the record.
std::wstring Widen(const std::string_view value)
{
	if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) return {};
	const int length = static_cast<int>(value.size());
	const int required = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), length, nullptr, 0);
	if (required <= 0) return {};
	std::wstring result(static_cast<std::size_t>(required), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, 0, value.data(), length, result.data(), required) != required) {
		return {};
	}
	return result;
}

std::wstring StringField(const JsonObject& object, const std::string_view key)
{
	const auto* value = FindString(object, key);
	return value == nullptr ? std::wstring{} : Widen(*value);
}

bool IsAsciiAlphanumeric(const wchar_t character) noexcept
{
	return (character >= L'0' && character <= L'9')
		|| (character >= L'a' && character <= L'z')
		|| (character >= L'A' && character <= L'Z');
}

std::wstring_view TrimFolder(std::wstring_view folder) noexcept
{
	while (folder.size() > 1 && (folder.back() == L'\\' || folder.back() == L'/')) {
		// Keep a drive root's separator ("C:\") so it does not become "C:".
		if (folder.size() == 3 && folder[1] == L':') break;
		folder.remove_suffix(1);
	}
	return folder;
}

//! Text of a user prompt, or empty when the content is not a typed prompt:
//! tool results, images, and the `<command-name>` / `<system-reminder>` style
//! wrappers agents inject on the user's behalf.
std::wstring PromptText(const JsonValue& content)
{
	const auto accept = [](const std::string& text) {
		const auto first = text.find_first_not_of(" \t\r\n");
		return first != std::string::npos && text[first] != '<';
	};
	if (content.is<std::string>()) {
		const auto& text = content.get<std::string>();
		return accept(text) ? NormalizeAgentSessionTitle(Widen(text)) : std::wstring{};
	}
	if (!content.is<JsonArray>()) return {};
	for (const auto& item : content.get<JsonArray>()) {
		if (!item.is<JsonObject>()) continue;
		const auto& part = item.get<JsonObject>();
		const auto* type = FindString(part, "type");
		const auto* text = FindString(part, "text");
		if (type == nullptr || *type != "text" || text == nullptr) continue;
		if (accept(*text)) return NormalizeAgentSessionTitle(Widen(*text));
		return {};
	}
	return {};
}

std::wstring FormatCount(const std::wstring_view format, const std::int64_t count)
{
	std::wstring result(format);
	const auto marker = result.find(L"{0}");
	if (marker != std::wstring::npos) result.replace(marker, 3, std::to_wstring(count));
	return result;
}

} // namespace

std::wstring_view AgentSessionProviderDisplayName(const EAgentSessionProvider provider) noexcept
{
	switch (provider) {
	case EAgentSessionProvider::ClaudeCode: return L"Claude Code";
	case EAgentSessionProvider::Codex: return L"Codex";
	case EAgentSessionProvider::Devin: return L"Devin";
	case EAgentSessionProvider::Pi: return L"Pi";
	}
	return L"";
}

bool IsValidAgentSessionId(const std::wstring_view id) noexcept
{
	if (id.empty() || id.size() > kMaximumAgentSessionIdLength || !IsAsciiAlphanumeric(id.front())) {
		return false;
	}
	return std::ranges::all_of(id, [](const wchar_t character) {
		return IsAsciiAlphanumeric(character) || character == L'-' || character == L'_';
	});
}

std::optional<std::wstring> BuildAgentSessionResumeCommand(
	const EAgentSessionProvider provider, const std::wstring_view id)
{
	if (!IsValidAgentSessionId(id)) return std::nullopt;
	switch (provider) {
	case EAgentSessionProvider::ClaudeCode: return L"claude --resume " + std::wstring(id);
	case EAgentSessionProvider::Codex: return L"codex resume " + std::wstring(id);
	case EAgentSessionProvider::Devin: return L"devin -r " + std::wstring(id);
	case EAgentSessionProvider::Pi: return L"pi --session " + std::wstring(id);
	}
	return std::nullopt;
}

std::wstring AgentSessionIdentity(const AgentSessionRecord& session)
{
	switch (session.provider) {
	case EAgentSessionProvider::ClaudeCode: return L"claude-code:" + session.id;
	case EAgentSessionProvider::Codex: return L"codex:" + session.id;
	case EAgentSessionProvider::Devin: return L"devin:" + session.id;
	case EAgentSessionProvider::Pi: return L"pi:" + session.id;
	}
	return session.id;
}

bool IsSameAgentSessionFolder(const std::wstring_view left, const std::wstring_view right) noexcept
{
	const auto a = TrimFolder(left);
	const auto b = TrimFolder(right);
	if (a.empty() || a.size() != b.size()) return false;
	for (std::size_t index = 0; index < a.size(); ++index) {
		wchar_t x = a[index] == L'/' ? L'\\' : a[index];
		wchar_t y = b[index] == L'/' ? L'\\' : b[index];
		if (std::towlower(x) != std::towlower(y)) return false;
	}
	return true;
}

std::wstring ClaudeCodeProjectDirectoryName(const std::wstring_view folder)
{
	std::wstring result(TrimFolder(folder));
	for (auto& character : result) {
		if (!IsAsciiAlphanumeric(character)) character = L'-';
	}
	return result;
}

std::wstring PiSessionDirectoryName(const std::wstring_view folder)
{
	std::wstring_view trimmed = TrimFolder(folder);
	if (!trimmed.empty() && (trimmed.front() == L'\\' || trimmed.front() == L'/')) trimmed.remove_prefix(1);
	std::wstring result(trimmed);
	for (auto& character : result) {
		if (character == L'\\' || character == L'/' || character == L':') character = L'-';
	}
	return L"--" + result + L"--";
}

std::wstring NormalizeAgentSessionTitle(const std::wstring_view raw)
{
	std::wstring result;
	bool pendingSpace = false;
	bool truncated = false;
	for (const wchar_t character : raw) {
		if (character < 0x20 || character == 0x7f || std::iswspace(character)) {
			pendingSpace = !result.empty();
			continue;
		}
		if (result.size() + (pendingSpace ? 1U : 0U) >= kMaximumAgentSessionTitleLength) {
			truncated = true;
			break;
		}
		if (pendingSpace) result.push_back(L' ');
		pendingSpace = false;
		result.push_back(character);
	}
	if (truncated) {
		// The ellipsis counts toward the bound. Never leave a trailing space or
		// half of a surrogate pair before it.
		if (result.size() >= kMaximumAgentSessionTitleLength) result.resize(kMaximumAgentSessionTitleLength - 1U);
		while (!result.empty() && result.back() == L' ') result.pop_back();
		if (!result.empty() && result.back() >= 0xD800 && result.back() <= 0xDBFF) result.pop_back();
		result += L"\u2026";
	}
	return result;
}

std::optional<AgentSessionRecord> ParseClaudeCodeTranscript(
	const std::string_view head, const std::string_view tail, const std::wstring_view fileStem,
	const std::wstring_view folder, const std::int64_t lastWriteUnixSeconds)
{
	if (!IsValidAgentSessionId(fileStem)) return std::nullopt;
	std::optional<std::wstring> cwd;
	std::optional<bool> sidechainTranscript;
	std::wstring customTitle;
	std::wstring aiTitle;
	std::wstring summary;
	std::wstring firstPrompt;
	const auto visit = [&](const JsonObject& record) {
		if (!cwd) {
			if (const auto* value = FindString(record, "cwd")) cwd = Widen(*value);
		}
		if (!sidechainTranscript) {
			// The first record that says decides: a sub-agent transcript (older
			// Claude Code kept `agent-*.jsonl` beside the sessions) is all
			// sidechain and cannot be resumed with `claude --resume`.
			if (const auto* value = Find(record, "isSidechain"); value != nullptr && value->is<bool>()) {
				sidechainTranscript = value->get<bool>();
			}
		}
		const auto* type = FindString(record, "type");
		if (type == nullptr) return true;
		if (*type == "custom-title") {
			if (auto title = NormalizeAgentSessionTitle(StringField(record, "customTitle")); !title.empty()) {
				customTitle = std::move(title);
			}
		} else if (*type == "ai-title") {
			if (auto title = NormalizeAgentSessionTitle(StringField(record, "aiTitle")); !title.empty()) {
				aiTitle = std::move(title);
			}
		} else if (*type == "summary") {
			if (auto title = NormalizeAgentSessionTitle(StringField(record, "summary")); !title.empty()) {
				summary = std::move(title);
			}
		} else if (*type == "user" && firstPrompt.empty()) {
			const auto* meta = Find(record, "isMeta");
			const auto* sidechain = Find(record, "isSidechain");
			const bool isMeta = meta != nullptr && meta->is<bool>() && meta->get<bool>();
			const bool isSidechain = sidechain != nullptr && sidechain->is<bool>() && sidechain->get<bool>();
			const auto* message = FindObject(record, "message");
			if (!isMeta && !isSidechain && message != nullptr) {
				if (const auto* content = Find(*message, "content")) firstPrompt = PromptText(*content);
			}
		}
		return true;
	};
	ForEachJsonLine(head, visit);
	ForEachJsonLine(tail, visit);
	if (!cwd || !IsSameAgentSessionFolder(*cwd, folder) || sidechainTranscript.value_or(false)) {
		return std::nullopt;
	}
	AgentSessionRecord record;
	record.provider = EAgentSessionProvider::ClaudeCode;
	record.id.assign(fileStem);
	record.title = !customTitle.empty() ? customTitle
		: !aiTitle.empty() ? aiTitle
		: !summary.empty() ? summary
		: firstPrompt;
	record.lastActivityUnixSeconds = lastWriteUnixSeconds;
	return record;
}

std::map<std::wstring, std::wstring> ParseCodexSessionIndex(const std::string_view text)
{
	std::map<std::wstring, std::wstring> result;
	ForEachJsonLine(text, [&result](const JsonObject& record) {
		auto id = StringField(record, "id");
		if (!IsValidAgentSessionId(id)) return true;
		auto name = NormalizeAgentSessionTitle(StringField(record, "thread_name"));
		if (name.empty()) result.erase(id);
		else result.insert_or_assign(std::move(id), std::move(name));
		return true;
	});
	return result;
}

std::optional<AgentSessionRecord> ParseCodexRollout(
	const std::string_view head, const std::wstring_view folder, const std::int64_t lastWriteUnixSeconds,
	const std::map<std::wstring, std::wstring>& threadNames)
{
	std::optional<AgentSessionRecord> result;
	bool first = true;
	bool rejected = false;
	ForEachJsonLine(head, [&](const JsonObject& record) {
		if (first) {
			first = false;
			const auto* type = FindString(record, "type");
			const auto* payload = FindObject(record, "payload");
			if (type == nullptr || *type != "session_meta" || payload == nullptr) {
				rejected = true;
				return false;
			}
			auto id = StringField(*payload, "id");
			const auto cwd = StringField(*payload, "cwd");
			const auto* threadSource = FindString(*payload, "thread_source");
			const auto* source = Find(*payload, "source");
			const bool subagent = (threadSource != nullptr && *threadSource != "user")
				|| (source != nullptr && source->is<JsonObject>()
					&& Find(source->get<JsonObject>(), "subagent") != nullptr);
			if (!IsValidAgentSessionId(id) || subagent || !IsSameAgentSessionFolder(cwd, folder)) {
				rejected = true;
				return false;
			}
			AgentSessionRecord session;
			session.provider = EAgentSessionProvider::Codex;
			if (const auto found = threadNames.find(id); found != threadNames.end()) {
				session.title = found->second;
			}
			session.id = std::move(id);
			session.lastActivityUnixSeconds = lastWriteUnixSeconds;
			result = std::move(session);
			return !result->title.empty() ? false : true;
		}
		const auto* type = FindString(record, "type");
		const auto* payload = FindObject(record, "payload");
		if (type == nullptr || *type != "event_msg" || payload == nullptr) return true;
		const auto* payloadType = FindString(*payload, "type");
		if (payloadType == nullptr || *payloadType != "user_message") return true;
		if (const auto* message = Find(*payload, "message")) {
			if (auto title = PromptText(*message); !title.empty()) {
				result->title = std::move(title);
				return false;
			}
		}
		return true;
	});
	if (rejected) return std::nullopt;
	return result;
}

std::optional<AgentSessionRecord> ParsePiSession(
	const std::string_view head, const std::string_view tail, const std::wstring_view folder,
	const std::int64_t lastWriteUnixSeconds)
{
	std::optional<AgentSessionRecord> result;
	bool first = true;
	bool rejected = false;
	std::optional<std::wstring> name;
	std::wstring firstPrompt;
	const auto visitBody = [&](const JsonObject& record) {
		const auto* type = FindString(record, "type");
		if (type == nullptr) return;
		if (*type == "session_info") {
			auto value = NormalizeAgentSessionTitle(StringField(record, "name"));
			name = std::move(value);	// an empty name clears the title
		} else if (*type == "message" && firstPrompt.empty()) {
			const auto* message = FindObject(record, "message");
			const auto* role = message != nullptr ? FindString(*message, "role") : nullptr;
			if (role != nullptr && *role == "user") {
				if (const auto* content = Find(*message, "content")) firstPrompt = PromptText(*content);
			}
		}
	};
	ForEachJsonLine(head, [&](const JsonObject& record) {
		if (first) {
			first = false;
			const auto* type = FindString(record, "type");
			auto id = StringField(record, "id");
			if (type == nullptr || *type != "session" || !IsValidAgentSessionId(id)
				|| !IsSameAgentSessionFolder(StringField(record, "cwd"), folder)) {
				rejected = true;
				return false;
			}
			AgentSessionRecord session;
			session.provider = EAgentSessionProvider::Pi;
			session.id = std::move(id);
			session.lastActivityUnixSeconds = lastWriteUnixSeconds;
			result = std::move(session);
			return true;
		}
		visitBody(record);
		return true;
	});
	if (rejected || !result) return std::nullopt;
	ForEachJsonLine(tail, [&](const JsonObject& record) {
		visitBody(record);
		return true;
	});
	result->title = name ? (!name->empty() ? *name : firstPrompt) : firstPrompt;
	return result;
}

std::vector<AgentSessionRecord> ParseDevinSessionList(
	const std::string_view json, const std::wstring_view folder)
{
	std::vector<AgentSessionRecord> result;
	const auto parsed = ParseJson(json);
	if (!parsed || !parsed->is<JsonArray>()) return result;
	for (const auto& item : parsed->get<JsonArray>()) {
		if (result.size() == kMaximumDevinSessions) break;
		if (!item.is<JsonObject>()) continue;
		const auto& entry = item.get<JsonObject>();
		auto id = StringField(entry, "id");
		if (!IsValidAgentSessionId(id)
			|| !IsSameAgentSessionFolder(StringField(entry, "working_directory"), folder)) continue;
		AgentSessionRecord session;
		session.provider = EAgentSessionProvider::Devin;
		session.id = std::move(id);
		session.title = NormalizeAgentSessionTitle(StringField(entry, "title"));
		if (const auto* activity = Find(entry, "last_activity_at")) {
			try {
				if (activity->is<std::string>()) {
					const auto& text = activity->get<std::string>();
					std::size_t used = 0;
					const auto value = std::stoll(text, &used);
					if (used == text.size() && value > 0) session.lastActivityUnixSeconds = value;
				} else if (activity->is<double>() && activity->get<double>() > 0
					&& activity->get<double>() < 9.0e15) {
					session.lastActivityUnixSeconds = static_cast<std::int64_t>(activity->get<double>());
				}
			} catch (...) {
				session.lastActivityUnixSeconds = 0;
			}
		}
		result.push_back(std::move(session));
	}
	return result;
}

std::vector<AgentSessionRecord> MergeAgentSessions(
	std::vector<AgentSessionRecord> sessions, const std::size_t maximum)
{
	std::ranges::stable_sort(sessions, [](const auto& left, const auto& right) {
		const bool leftKnown = left.lastActivityUnixSeconds > 0;
		const bool rightKnown = right.lastActivityUnixSeconds > 0;
		if (leftKnown != rightKnown) return leftKnown;
		if (left.lastActivityUnixSeconds != right.lastActivityUnixSeconds) {
			return left.lastActivityUnixSeconds > right.lastActivityUnixSeconds;
		}
		return std::tie(left.provider, left.id) < std::tie(right.provider, right.id);
	});
	std::vector<AgentSessionRecord> result;
	std::set<std::wstring> seen;
	for (auto& session : sessions) {
		if (result.size() == maximum) break;
		if (!seen.insert(AgentSessionIdentity(session)).second) continue;
		result.push_back(std::move(session));
	}
	return result;
}

std::wstring FormatAgentSessionAge(const std::int64_t lastActivityUnixSeconds,
	const std::int64_t nowUnixSeconds, const AgentSessionAgeTexts& texts)
{
	if (lastActivityUnixSeconds <= 0) return {};
	const auto elapsed = nowUnixSeconds - lastActivityUnixSeconds;
	if (elapsed < 60) return texts.now;
	if (elapsed < 60 * 60) return FormatCount(texts.minutesFormat, elapsed / 60);
	if (elapsed < 24 * 60 * 60) return FormatCount(texts.hoursFormat, elapsed / (60 * 60));
	return FormatCount(texts.daysFormat, elapsed / (24 * 60 * 60));
}

} // namespace workbench::projects
