/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace terminal {

//! Client-space locations shared by the native caret and the Windows IME.
//! The composition string is placed immediately below the active cell, while
//! the candidate form excludes the active cell so the IME can choose a safe
//! side when the list would otherwise run beyond the client or screen edge.
struct TerminalImeWindowPosition final {
	POINT caret{};
	POINT composition{};
	RECT candidateArea{};
};

//! Terminal grid dimensions a client rectangle can carry.
struct TerminalGridExtent final {
	std::uint16_t columns{};
	std::uint16_t rows{};

	[[nodiscard]] friend constexpr bool operator==(
		const TerminalGridExtent& lhs, const TerminalGridExtent& rhs ) noexcept
	{
		return lhs.columns == rhs.columns && lhs.rows == rhs.rows;
	}
};

//! Shared terminal content inset used by PTY sizing, painting, hit testing, and
//! caret/IME placement.
//!
//! Upstream VS Code reserves a fixed 20 CSS-pixel left gutter for command
//! decorations.  Sakura apportions that same 20 DIP budget across the four
//! sides (5 DIP each) so the grid is inset uniformly rather than left-only.
struct TerminalViewportGeometry final {
	static constexpr int kPaddingBudgetDip = 20;
	static constexpr int kSideCount = 4;
	static constexpr int kPaddingDip = kPaddingBudgetDip / kSideCount;
	static constexpr unsigned int kDefaultDpi = 96;

	int padding{};

	[[nodiscard]] static constexpr TerminalViewportGeometry FromDpi(unsigned int dpi) noexcept
	{
		const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
		// MulDiv's positive-integer rounding is equivalent here and keeps this
		// helper usable by geometry tests without a user32/gdi dependency.
		const auto scaled = (static_cast<std::uint64_t>(kPaddingDip) * effectiveDpi + kDefaultDpi / 2) /
			kDefaultDpi;
		return { static_cast<int>(std::min<std::uint64_t>(scaled,
			static_cast<std::uint64_t>(std::numeric_limits<int>::max()))) };
	}

	[[nodiscard]] constexpr int GridOriginX() const noexcept
	{
		return std::max(0, padding);
	}

	[[nodiscard]] constexpr int GridOriginY() const noexcept
	{
		return std::max(0, padding);
	}

	[[nodiscard]] constexpr int GridWidth(int clientWidth) const noexcept
	{
		return std::max(0, clientWidth - GridOriginX() * 2);
	}

	[[nodiscard]] constexpr int GridHeight(int clientHeight) const noexcept
	{
		return std::max(0, clientHeight - GridOriginY() * 2);
	}

	[[nodiscard]] constexpr int TranslateToGridX(int clientX) const noexcept
	{
		return std::max(0, clientX - GridOriginX());
	}

	[[nodiscard]] constexpr int TranslateToGridY(int clientY) const noexcept
	{
		return std::max(0, clientY - GridOriginY());
	}

	[[nodiscard]] constexpr RECT GridRect(const RECT& client) const noexcept
	{
		const auto insetX = GridOriginX();
		const auto insetY = GridOriginY();
		const auto left = std::clamp(client.left + insetX, client.left, client.right);
		const auto top = std::clamp(client.top + insetY, client.top, client.bottom);
		const auto right = std::clamp(client.right - insetX, left, client.right);
		const auto bottom = std::clamp(client.bottom - insetY, top, client.bottom);
		return { left, top, right, bottom };
	}

	//! Columns and rows this client extent can hold, or nothing when the client
	//! has no extent at all.
	//!
	//! A minimized frame lays every child out at 0x0, and a hidden bottom Panel
	//! reaches the same path. Deriving a grid from that extent would clamp to a
	//! single column and row, and resizing a model to it truncates every retained
	//! row, so a caller must keep its last real measurement instead. This is what
	//! VS Code's TerminalInstance._evaluateColsAndRows() does when the terminal
	//! element reports a zero dimension: it returns null and changes nothing.
	[[nodiscard]] constexpr std::optional<TerminalGridExtent> MeasureGrid(
		int clientWidth, int clientHeight, int cellWidth, int cellHeight ) const noexcept
	{
		if( clientWidth <= 0 || clientHeight <= 0 ) return std::nullopt;
		constexpr int kMaxExtent = 65535;
		return TerminalGridExtent{
			static_cast<std::uint16_t>(std::clamp(
				GridWidth(clientWidth) / std::max(1, cellWidth), 1, kMaxExtent)),
			static_cast<std::uint16_t>(std::clamp(
				GridHeight(clientHeight) / std::max(1, cellHeight), 1, kMaxExtent)),
		};
	}

	[[nodiscard]] constexpr TerminalImeWindowPosition ImeWindowPosition(
		std::size_t column, std::size_t row, int cellWidth, int cellHeight ) const noexcept
	{
		const auto width = std::max(1, cellWidth);
		const auto height = std::max(1, cellHeight);
		const auto x = GridOriginX() + static_cast<LONG>(column * static_cast<std::size_t>(width));
		const auto top = GridOriginY() + static_cast<LONG>(row * static_cast<std::size_t>(height));
		const auto bottom = top + height;
		return {
			{ x, top },
			{ x, bottom },
			{ x, top, x + width, bottom },
		};
	}
};

} // namespace terminal
