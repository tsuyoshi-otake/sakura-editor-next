/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionCommandPalette.h"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <optional>

namespace {

bool ValidIdentifier(std::wstring_view value, std::size_t maximum)
{
	return !value.empty() && value.size() <= maximum &&
		value.find(L'\0') == std::wstring_view::npos;
}

std::wstring Lower(std::wstring_view value)
{
	std::wstring result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towlower(character));
	});
	return result;
}

std::vector<std::wstring> QueryWords(std::wstring_view query)
{
	std::vector<std::wstring> words;
	std::wstring current;
	for (const wchar_t character : query) {
		if (std::iswspace(character)) {
			if (!current.empty()) words.emplace_back(Lower(current));
			current.clear();
		} else {
			current.push_back(character);
		}
	}
	if (!current.empty()) words.emplace_back(Lower(current));
	return words;
}

std::optional<int> SubsequenceScore(std::wstring_view needle, std::wstring_view haystack)
{
	if (needle.empty()) return 0;
	const auto exact = haystack.find(needle);
	if (exact != std::wstring_view::npos) return exact == 0 ? 1000 : 800 - static_cast<int>((std::min<std::size_t>)(exact, 400));
	std::size_t cursor = 0;
	int gap = 0;
	for (const wchar_t character : needle) {
		const auto found = haystack.find(character, cursor);
		if (found == std::wstring_view::npos) return std::nullopt;
		gap += static_cast<int>(found - cursor);
		cursor = found + 1;
	}
	return 500 - (std::min)(gap, 450);
}

std::optional<int> MatchScore(const SExtensionCommandDescriptor& command, const std::vector<std::wstring>& words)
{
	const std::wstring label = command.category.empty() ? command.title : command.category + L": " + command.title;
	const std::wstring haystack = Lower(label + L" " + command.id);
	int total = 0;
	for (const auto& word : words) {
		const auto score = SubsequenceScore(word, haystack);
		if (!score) return std::nullopt;
		total += *score;
	}
	return total;
}

} // namespace

bool CExtensionCommandPalette::Register(SExtensionCommandDescriptor command)
{
	if (!ValidIdentifier(command.id, 512) || !ValidIdentifier(command.title, 2048) ||
		(!command.builtIn && !ValidIdentifier(command.extensionId, 255))) return false;
	std::unique_lock lock(m_mutex);
	return m_commands.emplace(command.id, std::move(command)).second;
}

bool CExtensionCommandPalette::Unregister(std::wstring_view commandId, std::wstring_view ownerExtensionId)
{
	std::unique_lock lock(m_mutex);
	const auto found = m_commands.find(std::wstring(commandId));
	if (found == m_commands.end()) return false;
	if (!ownerExtensionId.empty() && found->second.extensionId != ownerExtensionId) return false;
	m_commands.erase(found);
	return true;
}

void CExtensionCommandPalette::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	if (extensionId.empty()) return;
	std::unique_lock lock(m_mutex);
	std::erase_if(m_commands, [extensionId, generation](const auto& pair) {
		return !pair.second.builtIn && pair.second.extensionId == extensionId &&
			(generation == 0 || pair.second.generation == generation);
	});
}

void CExtensionCommandPalette::Clear()
{
	std::unique_lock lock(m_mutex);
	m_commands.clear();
}

bool CExtensionCommandPalette::Contains(std::wstring_view commandId) const
{
	std::shared_lock lock(m_mutex);
	return m_commands.contains(std::wstring(commandId));
}

std::vector<std::wstring> CExtensionCommandPalette::CommandIds(bool filterInternal) const
{
	std::shared_lock lock(m_mutex);
	std::vector<std::wstring> result;
	result.reserve(m_commands.size());
	for (const auto& [id, command] : m_commands) {
		if (!filterInternal || id.empty() || id.front() != L'_') result.push_back(id);
	}
	std::sort(result.begin(), result.end());
	return result;
}

std::vector<SExtensionCommandPaletteItem> CExtensionCommandPalette::Search(
	std::wstring_view query,
	const CExtensionContextKeys& contextKeys,
	std::size_t maximumResults) const
{
	struct Candidate {
		int score = 0;
		SExtensionCommandPaletteItem item;
	};
	const auto words = QueryWords(query);
	std::vector<Candidate> candidates;
	{
		std::shared_lock lock(m_mutex);
		candidates.reserve(m_commands.size());
		for (const auto& [id, command] : m_commands) {
			if (!contextKeys.Evaluate(command.whenClause)) continue;
			const auto score = MatchScore(command, words);
			if (!score) continue;
			candidates.push_back({ *score, {
				.id = id,
				.label = command.category.empty() ? command.title : command.category + L": " + command.title,
				.detail = command.builtIn ? L"Sakura Editor" : command.extensionId,
				.extensionId = command.extensionId,
				.enabled = contextKeys.Evaluate(command.enablementClause),
				.builtIn = command.builtIn,
			} });
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
		if (left.score != right.score) return left.score > right.score;
		return left.item.label < right.item.label;
	});
	if (candidates.size() > maximumResults) candidates.resize(maximumResults);
	std::vector<SExtensionCommandPaletteItem> result;
	result.reserve(candidates.size());
	for (auto& candidate : candidates) result.emplace_back(std::move(candidate.item));
	return result;
}
