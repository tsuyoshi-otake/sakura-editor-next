/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace editor::document {

//! A presentation-neutral logical line/column coordinate. Columns count UTF-8 bytes.
class DocumentPosition final {
public:
	constexpr DocumentPosition(std::size_t line = 0, std::size_t column = 0) noexcept
		: m_line(line), m_column(column) {}
	[[nodiscard]] constexpr std::size_t Line() const noexcept { return m_line; }
	[[nodiscard]] constexpr std::size_t Column() const noexcept { return m_column; }

private:
	const std::size_t m_line;
	const std::size_t m_column;
};

//! Converts between logical byte offsets and logical line/column coordinates.
//! Visual wrapping, font metrics, tabs, and Win32 painting are intentionally out of scope.
class LayoutProjection final {
public:
	[[nodiscard]] static DocumentPosition PositionAt(std::string_view text, std::size_t offset) noexcept;
	[[nodiscard]] static std::size_t OffsetAt(std::string_view text, DocumentPosition position) noexcept;
};

} // namespace editor::document
