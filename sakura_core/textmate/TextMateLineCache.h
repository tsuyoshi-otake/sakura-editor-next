/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "textmate/TextMateTokenizer.h"

namespace textmate {

//! Per-line incremental tokenization state, modeled on vscode-textmate's
//! `TrackingTokenizationStateStore` (`textModelTokens.ts`): one entry per
//! logical line plus an explicit set of INVALID lines. Re-tokenization always
//! resumes at the first invalid line; after tokenizing line L its produced end
//! state is compared (`Equivalent`) against the state previously stored for
//! that position -- if equal, line L+1 is *not* invalidated ("convergence"),
//! while invalid lines recorded by other edits stay invalid. An invalid line
//! that still holds tokens keeps returning them for painting (VS Code keeps
//! stale tokens visible until the tokenizer reaches the line); only a line
//! that has never been tokenized returns nothing.
//!
//! The invalid set is a sorted vector of disjoint half-open `[start, end)`
//! ranges (VS Code's `RangePriorityQueueImpl` shape): O(R) per edit where R is
//! the number of ranges, O(log R) lookup. `Reset`/`AcceptChange` never
//! allocate per line beyond the entries themselves.
//!
//! Not thread-safe, same as `TextMateTokenizer`; driven from the UI thread
//! under a per-call budget by the owner (`CEditView`). This class stays free
//! of `doc/` dependencies: edit records are duck-typed through the
//! `ApplyEdits` template.
class TextMateLineCache final {
public:
	struct LineEntry final {
		//! End-of-line rule stack. May be non-null for a line with no tokens
		//! yet: `AcceptChange` keeps the last old line's state on the last new
		//! line as the convergence anchor (VS Code
		//! `TokenizationStateStore.acceptChange`).
		RuleStackHandle stateAfter;
		std::vector<TextMateToken> tokens;
		bool hasTokens = false;
	};

	struct EnsureResult final {
		std::size_t tokenized = 0;
		//! The smallest line index that this call *newly* marked invalid
		//! because the previous line's end state changed. `nullopt` when no
		//! new invalidation happened. The view uses it to repaint the lines
		//! below a state-changing edit (VS Code repaints them via
		//! `onDidChangeTokens`).
		std::optional<std::size_t> firstNewlyInvalidatedLine;
	};

	//! Discards every entry and marks the whole document `[0, lineCount)`
	//! unresolved.
	void Reset(std::size_t lineCount);

	//! Replaces the entries of old lines `[firstLine, firstLine+oldLineCount)`
	//! with `newLineCount` new entries and marks `[firstLine, firstLine +
	//! markCount)` invalid, where `markCount` is `newLineCount` or at least 1
	//! when a line still exists at `firstLine` -- the line at the edit
	//! position is always (re)produced in VS Code (`eolCount + 1`), and a pure
	//! deletion must still re-validate the line that shifted into place.
	//! Entries outside the range keep tokens/state and are NOT invalidated.
	//! When both counts are non-zero the last new entry keeps the last old
	//! line's `stateAfter` as the convergence anchor; its tokens are cleared
	//! (`hasTokens == false`).
	void AcceptChange(std::size_t firstLine, std::size_t oldLineCount, std::size_t newLineCount);

	//! Applies a sequence of edit records (e.g. `CDocLineMgr::CollectTextEdits`
	//! output) and validates the result against `currentLineCount`, the
	//! document's real line count. Element requirements: `.full` (bool),
	//! `.firstLine`, `.oldLineCount`, `.newLineCount` (integer-convertible).
	//! Any `.full` record (or a record set whose net delta does not reach
	//! `currentLineCount`) resets the cache to `currentLineCount` all-invalid
	//! entries: the cache cannot reconstruct a state it never saw, so it
	//! fails closed instead of trusting inconsistent records.
	template <typename EditRange>
	void ApplyEdits(const EditRange& edits, std::size_t currentLineCount)
	{
		for (const auto& edit : edits) {
			if (edit.full) {
				Reset(currentLineCount);
				return;
			}
		}
		for (const auto& edit : edits) {
			AcceptChange(
				static_cast<std::size_t>(static_cast<long long>(edit.firstLine)),
				static_cast<std::size_t>(static_cast<long long>(edit.oldLineCount)),
				static_cast<std::size_t>(static_cast<long long>(edit.newLineCount)));
		}
		if (LineCount() != currentLineCount) Reset(currentLineCount);
	}

	//! Tokens for `line`, including a still-invalid line's previous tokens.
	//! nullptr only for a line that has never produced tokens (`hasTokens`).
	[[nodiscard]] const std::vector<TextMateToken>* GetTokens(std::size_t line) const noexcept;

	[[nodiscard]] bool HasTokens(std::size_t line) const noexcept;
	[[nodiscard]] bool IsInvalid(std::size_t line) const noexcept;
	//! Smallest invalid index, or `LineCount()` when fully resolved.
	[[nodiscard]] std::size_t FirstInvalidLine() const noexcept;
	[[nodiscard]] std::size_t LineCount() const noexcept { return m_entries.size(); }
	[[nodiscard]] const LineEntry& Entry(std::size_t line) const { return m_entries[line]; }

	using LineTextFn = std::function<std::wstring_view(std::size_t)>;
	using TokenizeFn = std::function<std::vector<TextMateToken>(
		std::wstring_view, const RuleStackHandle&, RuleStackHandle&)>;

	//! Tokenizes from the first invalid line while `budget > 0` and the
	//! invalid index stays `<= uptoLine`. `initialState` is the state line 0
	//! starts from (the session's `InitialState()`); `lineText(i)` must
	//! return line `i`'s text without line-ending characters; `tokenize`
	//! wraps one `TokenizeLine` call.
	EnsureResult EnsureResolved(std::size_t uptoLine, std::size_t budget,
		const RuleStackHandle& initialState,
		const LineTextFn& lineText, const TokenizeFn& tokenize);

private:
	// Sorted disjoint half-open ranges, the RangePriorityQueueImpl shape.
	void AddInvalidRange(std::size_t start, std::size_t end);
	//! Pops `m_invalid.front().first` after it was tokenized.
	void PopFrontInvalidLine() noexcept;
	[[nodiscard]] bool InvalidContains(std::size_t line) const noexcept;

	std::deque<LineEntry> m_entries;                  //!< one slot per logical line
	std::vector<std::pair<std::size_t, std::size_t>> m_invalid; //!< lines needing (re)tokenization
};

} // namespace textmate
