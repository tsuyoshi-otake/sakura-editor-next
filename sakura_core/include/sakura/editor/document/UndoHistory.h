/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace editor::document {

//! One complete logical-content transition. The history owns these snapshots;
//! TextDocument remains the sole owner of its current content.
class UndoEntry final {
public:
	UndoEntry(std::string before, std::string after)
		: m_before(std::move(before)), m_after(std::move(after)) {}
	[[nodiscard]] const std::string& Before() const noexcept { return m_before; }
	[[nodiscard]] const std::string& After() const noexcept { return m_after; }

private:
	const std::string m_before;
	const std::string m_after;
};

//! Owns undo/redo cursor state. A new record discards the redo branch.
class UndoHistory final {
public:
	[[nodiscard]] bool Record(UndoEntry entry) noexcept;
	[[nodiscard]] const UndoEntry* NextUndo() const noexcept;
	[[nodiscard]] const UndoEntry* NextRedo() const noexcept;
	void CommitUndo() noexcept;
	void CommitRedo() noexcept;
	void Clear() noexcept;

	[[nodiscard]] bool CanUndo() const noexcept { return m_cursor != 0; }
	[[nodiscard]] bool CanRedo() const noexcept { return m_cursor < m_entries.size(); }
	[[nodiscard]] std::size_t UndoCount() const noexcept { return m_cursor; }
	[[nodiscard]] std::size_t RedoCount() const noexcept { return m_entries.size() - m_cursor; }

private:
	std::vector<UndoEntry> m_entries;
	std::size_t m_cursor = 0;
};

} // namespace editor::document
