/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace terminal {

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
};

} // namespace terminal
