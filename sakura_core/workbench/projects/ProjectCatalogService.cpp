/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/ProjectCatalogService.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <variant>

namespace workbench::projects {
namespace {

using platform::serialization::JsoncValue;

constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U;
constexpr std::size_t kMaximumLabelCharacters = 256;

bool IsWellFormedUtf16(const std::wstring_view value) noexcept
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

std::optional<std::string> QuoteJson(const std::wstring_view value)
{
	if (!IsWellFormedUtf16(value)) return std::nullopt;
	std::string result(1, '"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto unit = static_cast<std::uint32_t>(value[index]);
		if (unit == L'"' || unit == L'\\') {
			result.push_back('\\');
			result.push_back(static_cast<char>(unit));
		} else if (unit == L'\b') result += "\\b";
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

std::optional<std::wstring> StringMember(const JsoncValue::Object& object,
	const std::wstring_view key)
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
		std::ranges::replace(path, L'/', L'\\');
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
	return path.size() >= extension.size()
		&& _wcsicmp(path.c_str() + path.size() - extension.size(), extension.data()) == 0;
}

const char* KindText(const EProjectKind kind) noexcept
{
	return kind == EProjectKind::Folder ? "folder" : "workspace";
}

std::optional<EProjectKind> ParseKind(const std::wstring_view value) noexcept
{
	if (value == L"folder") return EProjectKind::Folder;
	if (value == L"workspace") return EProjectKind::Workspace;
	return std::nullopt;
}

} // namespace

CProjectCatalogService::CProjectCatalogService(std::unique_ptr<IProjectCatalogStore> store) noexcept :
	m_store(std::move(store))
{
}

ProjectCatalogResult CProjectCatalogService::Load()
{
	m_state = EProjectCatalogState::Unavailable;
	if (!m_store) return { EProjectCatalogOutcome::Failed, "project catalog store is not configured" };
	const auto loaded = m_store->Load();
	if (loaded.status == EProjectCatalogStoreLoadStatus::NotFound) {
		m_entries.clear();
		m_state = EProjectCatalogState::Ready;
		return { EProjectCatalogOutcome::Succeeded, {} };
	}
	if (loaded.status != EProjectCatalogStoreLoadStatus::Succeeded || !loaded.payload) {
		return { EProjectCatalogOutcome::Failed,
			loaded.diagnostic.empty() ? "project catalog store read failed" : loaded.diagnostic };
	}
	std::string diagnostic;
	const auto decoded = Decode(*loaded.payload, diagnostic);
	if (!decoded) return { EProjectCatalogOutcome::Failed, std::move(diagnostic) };
	m_entries = *decoded;
	m_state = EProjectCatalogState::Ready;
	return { EProjectCatalogOutcome::Succeeded, {} };
}

std::vector<ProjectEntry> CProjectCatalogService::Snapshot() const
{
	return m_entries;
}

ProjectCatalogResult CProjectCatalogService::RecordSuccessfulOpen(ProjectEntry entry)
{
	if (m_state != EProjectCatalogState::Ready) {
		return { EProjectCatalogOutcome::Failed,
			"project catalog writes require a successful coherent load" };
	}
	const auto normalized = Normalize(std::move(entry));
	if (!normalized) return { EProjectCatalogOutcome::Failed, "project entry is invalid" };
	for (int attempt = 0; attempt < 2; ++attempt) {
		auto next = m_entries;
		const auto found = std::ranges::find_if(next, [&normalized](const auto& current) {
			return SameIdentity(current.uri, normalized->uri);
		});
		if (found == next.end()) {
			if (next.size() >= kMaximumProjects) {
				return { EProjectCatalogOutcome::Failed, "project catalog capacity was reached" };
			}
			next.push_back(*normalized);
		} else {
			*found = *normalized;
		}
		const auto saved = SaveEntries(next);
		if (saved.status == EProjectCatalogStoreSaveStatus::Succeeded) {
			m_entries = std::move(next);
			return { EProjectCatalogOutcome::Succeeded, {} };
		}
		if (saved.status != EProjectCatalogStoreSaveStatus::Conflict || attempt != 0) {
			return { EProjectCatalogOutcome::Failed,
				saved.diagnostic.empty() ? "project catalog store write failed" : saved.diagnostic };
		}
		const auto reloaded = Load();
		if (reloaded.outcome != EProjectCatalogOutcome::Succeeded) return reloaded;
	}
	return { EProjectCatalogOutcome::Failed, "project catalog conflict retry was exhausted" };
}

ProjectCatalogResult CProjectCatalogService::Remove(const platform::uri::Uri& uri)
{
	if (m_state != EProjectCatalogState::Ready) {
		return { EProjectCatalogOutcome::Failed,
			"project catalog writes require a successful coherent load" };
	}
	const auto canonical = CanonicalizeUri(uri);
	if (!canonical) return { EProjectCatalogOutcome::Failed, "project URI is invalid" };
	for (int attempt = 0; attempt < 2; ++attempt) {
		auto next = m_entries;
		std::erase_if(next, [&canonical](const auto& current) {
			return SameIdentity(current.uri, *canonical);
		});
		if (next.size() == m_entries.size()) return { EProjectCatalogOutcome::Succeeded, {} };
		const auto saved = SaveEntries(next);
		if (saved.status == EProjectCatalogStoreSaveStatus::Succeeded) {
			m_entries = std::move(next);
			return { EProjectCatalogOutcome::Succeeded, {} };
		}
		if (saved.status != EProjectCatalogStoreSaveStatus::Conflict || attempt != 0) {
			return { EProjectCatalogOutcome::Failed,
				saved.diagnostic.empty() ? "project catalog store write failed" : saved.diagnostic };
		}
		const auto reloaded = Load();
		if (reloaded.outcome != EProjectCatalogOutcome::Succeeded) return reloaded;
	}
	return { EProjectCatalogOutcome::Failed, "project catalog conflict retry was exhausted" };
}

std::optional<ProjectEntry> CProjectCatalogService::Normalize(ProjectEntry entry)
{
	if (entry.label && (entry.label->empty() || entry.label->size() > kMaximumLabelCharacters
		|| !IsWellFormedUtf16(*entry.label))) return std::nullopt;
	const auto uri = CanonicalizeUri(entry.uri);
	if (!uri || uri->Path().empty() || uri->Query() || uri->Fragment()) return std::nullopt;
	if (entry.kind == EProjectKind::Workspace && !IsWorkspaceUri(*uri)) return std::nullopt;
	entry.uri = *uri;
	return entry;
}

bool CProjectCatalogService::SameIdentity(const platform::uri::Uri& left,
	const platform::uri::Uri& right) noexcept
{
	return platform::uri::UriIdentityService::IsEqual(left, right);
}

std::optional<std::vector<ProjectEntry>> CProjectCatalogService::Decode(
	const std::string_view payload, std::string& diagnostic)
{
	if (payload.empty() || payload.size() > kMaximumPayloadBytes) {
		diagnostic = "project catalog payload is empty or exceeds the byte limit";
		return std::nullopt;
	}
	const auto parsed = platform::serialization::CJsoncDocument::Parse(payload);
	if (!parsed.Succeeded()) {
		diagnostic = "project catalog payload is malformed";
		return std::nullopt;
	}
	const auto* root = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (!root) { diagnostic = "project catalog root is not an object"; return std::nullopt; }
	const auto version = root->find(L"version");
	const auto entries = root->find(L"entries");
	if (version == root->end() || entries == root->end()) {
		diagnostic = "project catalog payload is incomplete";
		return std::nullopt;
	}
	const auto* versionValue = std::get_if<std::int64_t>(&version->second.Value());
	const auto* array = std::get_if<JsoncValue::Array>(&entries->second.Value());
	if (!versionValue || *versionValue != kProjectCatalogSchemaVersion || !array
		|| array->size() > kMaximumProjects) {
		diagnostic = "project catalog payload has an unsupported schema or entry count";
		return std::nullopt;
	}
	std::vector<ProjectEntry> result;
	result.reserve(array->size());
	for (const auto& value : *array) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (!object) { diagnostic = "project catalog contains an invalid entry"; return std::nullopt; }
		const auto kind = StringMember(*object, L"kind");
		const auto uriText = StringMember(*object, L"uri");
		if (!kind || !uriText) { diagnostic = "project catalog entry is incomplete"; return std::nullopt; }
		const auto parsedKind = ParseKind(*kind);
		const auto parsedUri = platform::uri::Uri::Parse(*uriText);
		std::optional<std::wstring> label;
		if (object->contains(L"label")) label = StringMember(*object, L"label");
		if (!parsedKind || !parsedUri || (object->contains(L"label") && !label)) {
			diagnostic = "project catalog entry has an invalid type";
			return std::nullopt;
		}
		auto entry = Normalize({ *parsedKind, std::move(*parsedUri.value), std::move(label) });
		if (!entry || std::ranges::any_of(result, [&entry](const auto& existing) {
			return SameIdentity(existing.uri, entry->uri);
		})) {
			diagnostic = "project catalog entry is invalid or duplicated";
			return std::nullopt;
		}
		result.push_back(std::move(*entry));
	}
	return result;
}

std::optional<std::string> CProjectCatalogService::Encode(
	const std::vector<ProjectEntry>& entries)
{
	if (entries.size() > kMaximumProjects) return std::nullopt;
	std::string result = "{\"version\":1,\"entries\":[";
	for (std::size_t index = 0; index < entries.size(); ++index) {
		const auto normalized = Normalize(entries[index]);
		if (!normalized) return std::nullopt;
		const auto uri = QuoteJson(normalized->uri.ToString());
		if (!uri) return std::nullopt;
		if (index != 0) result.push_back(',');
		result += "{\"kind\":\"";
		result += KindText(normalized->kind);
		result += "\",\"uri\":" + *uri;
		if (normalized->label) {
			const auto label = QuoteJson(*normalized->label);
			if (!label) return std::nullopt;
			result += ",\"label\":" + *label;
		}
		result.push_back('}');
		if (result.size() > kMaximumPayloadBytes) return std::nullopt;
	}
	result += "]}";
	return result.size() <= kMaximumPayloadBytes
		? std::optional<std::string>(std::move(result)) : std::nullopt;
}

ProjectCatalogStoreSaveResult CProjectCatalogService::SaveEntries(
	const std::vector<ProjectEntry>& entries)
{
	if (!m_store) return { EProjectCatalogStoreSaveStatus::Failed, "project catalog store is not configured" };
	const auto payload = Encode(entries);
	if (!payload) return { EProjectCatalogStoreSaveStatus::Failed, "project catalog serialization failed" };
	return m_store->Save(*payload);
}

} // namespace workbench::projects
