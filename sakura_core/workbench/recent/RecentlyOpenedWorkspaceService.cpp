/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/recent/RecentlyOpenedWorkspaceService.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>
#include <variant>

namespace workbench::recent {
namespace {

using platform::serialization::JsoncValue;

constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U;
constexpr std::size_t kMaximumLabelCharacters = 256;

bool IsWellFormedUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit >= 0xd800U && unit <= 0xdbffU) {
			if (++index == value.size()) return false;
			const auto low = static_cast<std::uint32_t>(value[index]);
			if (low < 0xdc00U || low > 0xdfffU) return false;
		} else if (unit >= 0xdc00U && unit <= 0xdfffU) {
			return false;
		}
	}
	return true;
}

bool AppendUtf8(std::string& target, std::uint32_t codePoint)
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

std::optional<std::string> QuoteJson(std::wstring_view value)
{
	if (!IsWellFormedUtf16(value)) return std::nullopt;
	std::string result;
	result.push_back('"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit == L'"' || unit == L'\\') { result.push_back('\\'); result.push_back(static_cast<char>(unit)); }
		else if (unit == L'\b') result += "\\b";
		else if (unit == L'\f') result += "\\f";
		else if (unit == L'\n') result += "\\n";
		else if (unit == L'\r') result += "\\r";
		else if (unit == L'\t') result += "\\t";
		else if (unit <= 0x1fU) {
			static constexpr char hex[] = "0123456789abcdef";
			result += "\\u00";
			result.push_back(hex[(unit >> 4U) & 0xfU]);
			result.push_back(hex[unit & 0xfU]);
		} else if (unit >= 0xd800U && unit <= 0xdbffU) {
			const auto low = static_cast<std::uint32_t>(value[++index]);
			if (!AppendUtf8(result, 0x10000U + ((unit - 0xd800U) << 10U) + (low - 0xdc00U))) return std::nullopt;
		} else if (!AppendUtf8(result, unit)) return std::nullopt;
	}
	result.push_back('"');
	return result;
}

std::optional<std::wstring> StringMember(const JsoncValue::Object& object, std::wstring_view key)
{
	const auto found = object.find(key);
	if (found == object.end()) return std::nullopt;
	if (const auto* value = std::get_if<std::wstring>(&found->second.Value())) return *value;
	return std::nullopt;
}

std::optional<platform::uri::Uri> CanonicalizeUri(const platform::uri::Uri& uri)
{
	if (_wcsicmp(uri.Scheme().c_str(), L"file") == 0) {
		const auto windowsPath = uri.ToWindowsPath();
		if (!windowsPath.value) return std::nullopt;
		auto path = *windowsPath.value;
		std::replace(path.begin(), path.end(), L'/', L'\\');
		if (path.size() >= 2U && path[1] == L':' && path[0] >= L'a' && path[0] <= L'z') {
			path[0] = static_cast<wchar_t>(path[0] - L'a' + L'A');
		}
		auto reparsed = platform::uri::Uri::FromWindowsPath(path);
		if (!reparsed) return std::nullopt;
		return std::move(*reparsed.value);
	}
	auto reparsed = platform::uri::Uri::Parse(uri.ToString());
	if (!reparsed) return std::nullopt;
	return std::move(*reparsed.value);
}

bool IsWorkspaceUri(const platform::uri::Uri& uri) noexcept
{
	constexpr std::wstring_view extension = L".code-workspace";
	const auto& path = uri.Path();
	if (path.size() < extension.size()) return false;
	return _wcsicmp(path.c_str() + path.size() - extension.size(), extension.data()) == 0;
}

const char* KindText(ERecentlyOpenedWorkspaceKind kind) noexcept
{
	return kind == ERecentlyOpenedWorkspaceKind::Folder ? "folder" : "workspace";
}

std::optional<ERecentlyOpenedWorkspaceKind> ParseKind(std::wstring_view value) noexcept
{
	if (value == L"folder") return ERecentlyOpenedWorkspaceKind::Folder;
	if (value == L"workspace") return ERecentlyOpenedWorkspaceKind::Workspace;
	return std::nullopt;
}

} // namespace

CRecentlyOpenedWorkspaceService::CRecentlyOpenedWorkspaceService(std::unique_ptr<IRecentlyOpenedWorkspaceStore> store) noexcept :
	m_store(std::move(store))
{
}

RecentlyOpenedWorkspaceResult CRecentlyOpenedWorkspaceService::Load()
{
	if (!m_store) return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace store is not configured" };
	const auto loaded = m_store->Load();
	if (loaded.status == ERecentlyOpenedWorkspaceStoreLoadStatus::NotFound) {
		m_entries.clear();
		return { ERecentlyOpenedWorkspaceOutcome::Succeeded, {} };
	}
	if (loaded.status != ERecentlyOpenedWorkspaceStoreLoadStatus::Succeeded || !loaded.payload) {
		return { ERecentlyOpenedWorkspaceOutcome::Failed,
			loaded.diagnostic.empty() ? "recent workspace store read failed" : loaded.diagnostic };
	}
	std::string diagnostic;
	const auto decoded = Decode(*loaded.payload, diagnostic);
	if (!decoded) return { ERecentlyOpenedWorkspaceOutcome::Failed, std::move(diagnostic) };
	m_entries = *decoded;
	return { ERecentlyOpenedWorkspaceOutcome::Succeeded, std::move(diagnostic) };
}

std::vector<RecentlyOpenedWorkspaceEntry> CRecentlyOpenedWorkspaceService::Snapshot() const
{
	return m_entries;
}

RecentlyOpenedWorkspaceResult CRecentlyOpenedWorkspaceService::RecordSuccessfulOpen(RecentlyOpenedWorkspaceEntry entry)
{
	const auto normalized = Normalize(std::move(entry));
	if (!normalized) return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace entry is invalid" };
	for (int attempt = 0; attempt < 2; ++attempt) {
		std::vector<RecentlyOpenedWorkspaceEntry> next;
		next.reserve(kMaximumRecentlyOpenedWorkspaces);
		next.push_back(*normalized);
		for (const auto& current : m_entries) {
			if (!SameIdentity(current.uri, normalized->uri) && next.size() < kMaximumRecentlyOpenedWorkspaces) next.push_back(current);
		}
		const auto saved = SaveEntries(next);
		if (saved.status == ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded) {
			m_entries = std::move(next);
			return { ERecentlyOpenedWorkspaceOutcome::Succeeded, {} };
		}
		if (saved.status != ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict || attempt != 0) {
			return { ERecentlyOpenedWorkspaceOutcome::Failed,
				saved.diagnostic.empty() ? "recent workspace store write failed" : saved.diagnostic };
		}
		// The control client has synchronously requested a fresh storage snapshot.
		// Reload it and replay this semantic promotion once so concurrent editor
		// windows merge their MRU intents instead of overwriting one another.
		const auto reloaded = Load();
		if (reloaded.outcome != ERecentlyOpenedWorkspaceOutcome::Succeeded) return reloaded;
	}
	return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace conflict retry was exhausted" };
}

RecentlyOpenedWorkspaceResult CRecentlyOpenedWorkspaceService::RemoveConfirmedNotFound(const platform::uri::Uri& uri)
{
	const auto canonical = CanonicalizeUri(uri);
	if (!canonical) return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace URI is invalid" };
	for (int attempt = 0; attempt < 2; ++attempt) {
		std::vector<RecentlyOpenedWorkspaceEntry> next;
		next.reserve(m_entries.size());
		for (const auto& current : m_entries) {
			if (!SameIdentity(current.uri, *canonical)) next.push_back(current);
		}
		if (next.size() == m_entries.size()) return { ERecentlyOpenedWorkspaceOutcome::Succeeded, "recent workspace entry was absent" };
		const auto saved = SaveEntries(next);
		if (saved.status == ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded) {
			m_entries = std::move(next);
			return { ERecentlyOpenedWorkspaceOutcome::Succeeded, {} };
		}
		if (saved.status != ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict || attempt != 0) {
			return { ERecentlyOpenedWorkspaceOutcome::Failed,
				saved.diagnostic.empty() ? "recent workspace store write failed" : saved.diagnostic };
		}
		const auto reloaded = Load();
		if (reloaded.outcome != ERecentlyOpenedWorkspaceOutcome::Succeeded) return reloaded;
	}
	return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace conflict retry was exhausted" };
}

RecentlyOpenedWorkspaceResult CRecentlyOpenedWorkspaceService::Clear()
{
	if (m_entries.empty()) return { ERecentlyOpenedWorkspaceOutcome::Succeeded, "recent workspace history was already empty" };
	for (int attempt = 0; attempt < 2; ++attempt) {
		const auto saved = SaveEntries({});
		if (saved.status == ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded) {
			m_entries.clear();
			return { ERecentlyOpenedWorkspaceOutcome::Succeeded, {} };
		}
		if (saved.status != ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict || attempt != 0) {
			return { ERecentlyOpenedWorkspaceOutcome::Failed,
				saved.diagnostic.empty() ? "recent workspace store write failed" : saved.diagnostic };
		}
		// A conflicting write only moves the store revision forward; clearing
		// stays the requested outcome, so reload for the current revision and
		// replay the same empty payload once.
		const auto reloaded = Load();
		if (reloaded.outcome != ERecentlyOpenedWorkspaceOutcome::Succeeded) return reloaded;
		if (m_entries.empty()) return { ERecentlyOpenedWorkspaceOutcome::Succeeded, "recent workspace history was already empty" };
	}
	return { ERecentlyOpenedWorkspaceOutcome::Failed, "recent workspace conflict retry was exhausted" };
}

std::optional<RecentlyOpenedWorkspaceEntry> CRecentlyOpenedWorkspaceService::Normalize(RecentlyOpenedWorkspaceEntry entry)
{
	if (!IsValidLabel(entry.label)) return std::nullopt;
	const auto uri = CanonicalizeUri(entry.uri);
	if (!uri || uri->Path().empty() || uri->Query() || uri->Fragment()) return std::nullopt;
	if (entry.kind == ERecentlyOpenedWorkspaceKind::Workspace && !IsWorkspaceUri(*uri)) return std::nullopt;
	entry.uri = *uri;
	return entry;
}

bool CRecentlyOpenedWorkspaceService::IsValidLabel(const std::optional<std::wstring>& label) noexcept
{
	return !label || (!label->empty() && label->size() <= kMaximumLabelCharacters && IsWellFormedUtf16(*label));
}

bool CRecentlyOpenedWorkspaceService::SameIdentity(const platform::uri::Uri& left, const platform::uri::Uri& right) noexcept
{
	return platform::uri::UriIdentityService::IsEqual(left, right);
}

std::optional<std::vector<RecentlyOpenedWorkspaceEntry>> CRecentlyOpenedWorkspaceService::Decode(
	std::string_view payload, std::string& diagnostic)
{
	if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
		diagnostic = "recent workspace payload is empty or exceeds the byte limit";
		return std::nullopt;
	}
	const auto parsed = platform::serialization::CJsoncDocument::Parse(payload);
	if (!parsed.Succeeded()) {
		diagnostic = "recent workspace payload is malformed";
		return std::nullopt;
	}
	const auto* root = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (!root) { diagnostic = "recent workspace payload root is not an object"; return std::nullopt; }
	const auto version = root->find(L"version");
	const auto entries = root->find(L"entries");
	if (version == root->end() || entries == root->end()
		|| std::get_if<std::int64_t>(&version->second.Value()) == nullptr
		|| *std::get_if<std::int64_t>(&version->second.Value()) != kRecentlyOpenedWorkspaceSchemaVersion) {
		diagnostic = "recent workspace payload has an unsupported schema";
		return std::nullopt;
	}
	const auto* array = std::get_if<JsoncValue::Array>(&entries->second.Value());
	if (array == nullptr || array->size() > kMaximumRecentlyOpenedWorkspaces) {
		diagnostic = "recent workspace payload has an invalid entry count";
		return std::nullopt;
	}
	std::vector<RecentlyOpenedWorkspaceEntry> result;
	result.reserve(array->size());
	bool ignored = false;
	for (const auto& value : *array) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) { ignored = true; continue; }
		const auto kind = StringMember(*object, L"kind");
		const auto uriText = StringMember(*object, L"uri");
		if (!kind || !uriText) { ignored = true; continue; }
		const auto parsedKind = ParseKind(*kind);
		const auto parsedUri = platform::uri::Uri::Parse(*uriText);
		std::optional<std::wstring> label;
		if (object->contains(L"label")) label = StringMember(*object, L"label");
		if (!parsedKind || !parsedUri || (object->contains(L"label") && !label)) { ignored = true; continue; }
		auto entry = Normalize({ *parsedKind, std::move(*parsedUri.value), std::move(label) });
		if (!entry) { ignored = true; continue; }
		if (std::ranges::any_of(result, [&entry](const auto& existing) { return SameIdentity(existing.uri, entry->uri); })) {
			ignored = true;
			continue;
		}
		result.push_back(std::move(*entry));
	}
	if (ignored) diagnostic = "recent workspace payload contained invalid entries";
	return result;
}

std::optional<std::string> CRecentlyOpenedWorkspaceService::Encode(const std::vector<RecentlyOpenedWorkspaceEntry>& entries)
{
	if (entries.size() > kMaximumRecentlyOpenedWorkspaces) return std::nullopt;
	std::string result = "{\"version\":1,\"entries\":[";
	for (std::size_t index = 0; index < entries.size(); ++index) {
		const auto normalized = Normalize(entries[index]);
		if (!normalized) return std::nullopt;
		const auto uri = QuoteJson(normalized->uri.ToString());
		if (!uri) return std::nullopt;
		if (index != 0) result.push_back(',');
		result += "{\"kind\":\"";
		result += KindText(normalized->kind);
		result += "\",\"uri\":";
		result += *uri;
		if (normalized->label) {
			const auto label = QuoteJson(*normalized->label);
			if (!label) return std::nullopt;
			result += ",\"label\":";
			result += *label;
		}
		result.push_back('}');
		if (result.size() > kMaximumPayloadBytes) return std::nullopt;
	}
	result += "]}";
	return result.size() <= kMaximumPayloadBytes ? std::optional<std::string>(std::move(result)) : std::nullopt;
}

RecentlyOpenedWorkspaceStoreSaveResult CRecentlyOpenedWorkspaceService::SaveEntries(
	const std::vector<RecentlyOpenedWorkspaceEntry>& entries)
{
	if (!m_store) return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "recent workspace store is not configured" };
	const auto payload = Encode(entries);
	if (!payload) return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "recent workspace payload could not be encoded" };
	return m_store->Save(*payload);
}

} // namespace workbench::recent
