/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMateLineCache.h"

#include <algorithm>

namespace textmate {

void TextMateLineCache::Reset(std::size_t lineCount)
{
	m_entries.clear();
	m_entries.resize(lineCount);
	m_invalid.clear();
	if (lineCount > 0) m_invalid.emplace_back(0, lineCount);
}

void TextMateLineCache::AddInvalidRange(std::size_t start, std::size_t end)
{
	if (start >= end) return;
	// Insert sorted and merge overlapping/adjacent neighbours. R is small in
	// practice (one range per pending edit region), so a linear merge is fine.
	std::vector<std::pair<std::size_t, std::size_t>> merged;
	merged.reserve(m_invalid.size() + 1);
	auto it = m_invalid.begin();
	while (it != m_invalid.end() && it->second < start) {
		merged.push_back(*it++);
	}
	merged.emplace_back(start, end);
	while (it != m_invalid.end()) {
		auto& back = merged.back();
		if (it->first <= back.second) {
			// Overlap or adjacency on either side: lower the start too, or a
			// pending range that ends exactly at `start` would be swallowed
			// and its lines lost (they were never tokenized).
			back.first = (std::min)(back.first, it->first);
			back.second = (std::max)(back.second, it->second);
		} else {
			merged.push_back(*it);
		}
		++it;
	}
	m_invalid = std::move(merged);
}

void TextMateLineCache::PopFrontInvalidLine() noexcept
{
	// Only ever called for m_invalid.front().first (EnsureResolved pops the
	// smallest invalid line), so a front shrink suffices -- no split logic.
	auto& front = m_invalid.front();
	++front.first;
	if (front.first >= front.second) m_invalid.erase(m_invalid.begin());
}

bool TextMateLineCache::InvalidContains(std::size_t line) const noexcept
{
	// Binary search over disjoint sorted ranges.
	std::size_t lo = 0, hi = m_invalid.size();
	while (lo < hi) {
		const std::size_t mid = (lo + hi) / 2;
		if (line < m_invalid[mid].first) hi = mid;
		else if (line >= m_invalid[mid].second) lo = mid + 1;
		else return true;
	}
	return false;
}

void TextMateLineCache::AcceptChange(std::size_t firstLine, std::size_t oldLineCount, std::size_t newLineCount)
{
	if (firstLine > m_entries.size()) firstLine = m_entries.size();
	oldLineCount = (std::min)(oldLineCount, m_entries.size() - firstLine);

	// Convergence anchor: when old and new lines both exist, the state stored
	// after the LAST old line is carried to the LAST new line (VS Code
	// `TokenizationStateStore.acceptChange` replaces length-1 entries, which
	// leaves that state in place). The anchor's tokens are discarded with the
	// rest of the replaced range.
	RuleStackHandle kept;
	if (oldLineCount > 0 && newLineCount > 0) {
		kept = std::move(m_entries[firstLine + oldLineCount - 1].stateAfter);
	}

	// Splice only the difference: reset min(old, new) entries in place, then
	// erase or insert |old - new| entries. A 1->1 in-line edit moves nothing.
	const std::size_t common = (std::min)(oldLineCount, newLineCount);
	for (std::size_t i = 0; i < common; ++i) {
		auto& entry = m_entries[firstLine + i];
		entry.stateAfter.reset();
		entry.tokens.clear();
		entry.hasTokens = false;
	}
	if (newLineCount > oldLineCount) {
		m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(firstLine + common),
			newLineCount - oldLineCount, LineEntry{});
	} else if (oldLineCount > newLineCount) {
		m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(firstLine + common),
			m_entries.begin() + static_cast<std::ptrdiff_t>(firstLine + oldLineCount));
	}
	if (kept) {
		m_entries[firstLine + newLineCount - 1].stateAfter = std::move(kept);
	}

	// Invalid ranges: drop the part inside the old range, shift ranges after
	// it by the delta, then mark the produced lines.
	const std::size_t oldEnd = firstLine + oldLineCount;
	const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(newLineCount)
		- static_cast<std::ptrdiff_t>(oldLineCount);
	std::vector<std::pair<std::size_t, std::size_t>> shifted;
	shifted.reserve(m_invalid.size() + 2);
	for (const auto& range : m_invalid) {
		if (range.second <= firstLine) {
			shifted.push_back(range);
			continue;
		}
		if (range.first < firstLine) {
			shifted.emplace_back(range.first, firstLine);
		}
		if (range.second > oldEnd) {
			const std::size_t s = (std::max)(range.first, oldEnd);
			shifted.emplace_back(
				static_cast<std::size_t>(static_cast<std::ptrdiff_t>(s) + delta),
				static_cast<std::size_t>(static_cast<std::ptrdiff_t>(range.second) + delta));
		}
	}
	m_invalid = std::move(shifted);
	// The line at the edit position is always (re)produced -- even a pure
	// deletion leaves a line there whose stored start/end state may now be
	// wrong -- so at least one line is re-validated whenever one exists.
	std::size_t markEnd = firstLine + newLineCount;
	if (firstLine < m_entries.size() && markEnd == firstLine) markEnd = firstLine + 1;
	AddInvalidRange(firstLine, markEnd);
}

const std::vector<TextMateToken>* TextMateLineCache::GetTokens(std::size_t line) const noexcept
{
	if (line >= m_entries.size()) return nullptr;
	const auto& entry = m_entries[line];
	// A line that has never produced tokens (or whose anchor entry lost them)
	// returns nothing; an invalid line keeps returning its previous ones so
	// paint does not regress to legacy colors before the tokenizer reaches it
	// (vscode-textmate behaviour).
	return entry.hasTokens ? &entry.tokens : nullptr;
}

bool TextMateLineCache::HasTokens(std::size_t line) const noexcept
{
	return line < m_entries.size() && m_entries[line].hasTokens;
}

bool TextMateLineCache::IsInvalid(std::size_t line) const noexcept
{
	return InvalidContains(line);
}

std::size_t TextMateLineCache::FirstInvalidLine() const noexcept
{
	return m_invalid.empty() ? m_entries.size() : m_invalid.front().first;
}

TextMateLineCache::EnsureResult TextMateLineCache::EnsureResolved(
	std::size_t uptoLine, std::size_t budget,
	const RuleStackHandle& initialState,
	const LineTextFn& lineText, const TokenizeFn& tokenize)
{
	EnsureResult result;
	while (budget > 0 && !m_invalid.empty()) {
		const std::size_t line = m_invalid.front().first;
		if (line > uptoLine || line >= m_entries.size()) break;

		const RuleStackHandle previous = line == 0 ? initialState : m_entries[line - 1].stateAfter;
		const RuleStackHandle storedAfter = m_entries[line].stateAfter;

		RuleStackHandle next;
		auto tokens = tokenize(lineText(line), previous, next);
		m_entries[line].stateAfter = std::move(next);
		m_entries[line].tokens = std::move(tokens);
		m_entries[line].hasTokens = true;
		PopFrontInvalidLine();
		--budget;
		++result.tokenized;

		// Convergence: when the produced end state is equivalent to the state
		// previously stored at this same position, line+1 tokenizes
		// identically to before and is not invalidated. Lines invalidated by
		// other edits stay invalid either way.
		const bool converged = storedAfter && m_entries[line].stateAfter
			&& Equivalent(m_entries[line].stateAfter, storedAfter);
		if (!converged && line + 1 < m_entries.size() && !InvalidContains(line + 1)) {
			if (!result.firstNewlyInvalidatedLine) {
				result.firstNewlyInvalidatedLine = line + 1;
			}
			AddInvalidRange(line + 1, line + 2);
		}
	}
	return result;
}

} // namespace textmate
