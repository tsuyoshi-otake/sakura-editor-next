/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace workbench::activity {

//! Stable identifiers exposed to the workbench and future UIA/MSAA providers.
enum class ActivityBarItem : std::uint8_t {
	Explorer,
	Outline,
	Terminal,
	Count,
};

//! A physical-pixel rectangle with no dependency on Win32 types.
struct ActivityBarRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int Width() const noexcept { return right - left; }
	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
	[[nodiscard]] constexpr bool Contains(int x, int y) const noexcept
	{
		return x >= left && x < right && y >= top && y < bottom;
	}
	[[nodiscard]] constexpr bool operator==(const ActivityBarRect&) const noexcept = default;
};

//! Snapshot of one logical button. Bounds are in client pixels.
struct ActivityBarButtonInfo {
	ActivityBarItem item = ActivityBarItem::Explorer;
	ActivityBarRect bounds{};
	bool selected = false;
	bool hovered = false;
	bool pressed = false;
	bool focused = false;
	bool enabled = true;

	[[nodiscard]] constexpr bool operator==(const ActivityBarButtonInfo&) const noexcept = default;
};

//! Pure state and geometry for CActivityBar. It deliberately has no HWND, GDI, or callback ownership.
class ActivityBarModel final {
public:
	static constexpr int kWidthDip = 42;
	static constexpr int kButtonExtentDip = 42;
	static constexpr std::size_t kItemCount = static_cast<std::size_t>(ActivityBarItem::Count);

	//! Sets physical client dimensions and reflows the vertical button strip.
	void SetViewport(int widthPixels, int heightPixels, unsigned int dpi = 96) noexcept;
	//! Lets the owner reflect the currently visible workbench tool. std::nullopt means none is selected.
	void SetSelectedItem(std::optional<ActivityBarItem> item) noexcept;
	[[nodiscard]] std::optional<ActivityBarItem> GetSelectedItem() const noexcept { return m_selected; }

	void SetEnabled(ActivityBarItem item, bool enabled) noexcept;
	//! Semantic name used by UI hosts and tests; retained alongside SetEnabled for low-level callers.
	void SetItemEnabled(ActivityBarItem item, bool enabled) noexcept { SetEnabled(item, enabled); }
	[[nodiscard]] bool IsEnabled(ActivityBarItem item) const noexcept;
	void SetHoveredItem(std::optional<ActivityBarItem> item) noexcept;
	void SetPressedItem(std::optional<ActivityBarItem> item) noexcept;
	void SetFocusedItem(std::optional<ActivityBarItem> item) noexcept;
	[[nodiscard]] std::optional<ActivityBarItem> GetFocusedItem() const noexcept { return m_focused; }

	[[nodiscard]] std::size_t GetButtonCount() const noexcept { return kItemCount; }
	[[nodiscard]] ActivityBarButtonInfo GetButton(std::size_t index) const noexcept;
	[[nodiscard]] std::optional<ActivityBarItem> HitTest(int x, int y) const noexcept;
	//! Moves focus through enabled items. A positive direction moves down/right; a negative direction moves up/left.
	[[nodiscard]] std::optional<ActivityBarItem> MoveFocus(int direction) noexcept;
	//! Returns the requested item when it is enabled. The owner decides how to toggle the corresponding panel.
	[[nodiscard]] std::optional<ActivityBarItem> Invoke(ActivityBarItem item) noexcept;
	[[nodiscard]] std::optional<ActivityBarItem> InvokeFocused() noexcept;

	[[nodiscard]] int GetDpi() const noexcept { return m_dpi; }
	[[nodiscard]] int GetPreferredWidthPixels() const noexcept;

private:
	[[nodiscard]] static constexpr std::size_t ToIndex(ActivityBarItem item) noexcept
	{
		return static_cast<std::size_t>(item);
	}
	[[nodiscard]] static int ScaleDip(int dip, unsigned int dpi) noexcept;
	[[nodiscard]] bool IsValid(ActivityBarItem item) const noexcept;
	[[nodiscard]] std::optional<ActivityBarItem> FirstEnabled(int direction) const noexcept;
	void Reflow() noexcept;

	int m_widthPixels = kWidthDip;
	int m_heightPixels = 0;
	unsigned int m_dpi = 96;
	std::array<ActivityBarRect, kItemCount> m_bounds{};
	std::array<bool, kItemCount> m_enabled{ true, true, true };
	std::optional<ActivityBarItem> m_selected = ActivityBarItem::Explorer;
	std::optional<ActivityBarItem> m_hovered;
	std::optional<ActivityBarItem> m_pressed;
	std::optional<ActivityBarItem> m_focused;
};

[[nodiscard]] constexpr const wchar_t* ActivityBarItemName(ActivityBarItem item) noexcept
{
	switch (item) {
	case ActivityBarItem::Explorer: return L"Explorer";
	case ActivityBarItem::Outline: return L"Outline";
	case ActivityBarItem::Terminal: return L"Terminal";
	case ActivityBarItem::Count: break;
	}
	return L"";
}

} // namespace workbench::activity
