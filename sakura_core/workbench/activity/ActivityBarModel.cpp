/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/ActivityBarModel.h"

#include <algorithm>
#include <limits>

namespace workbench::activity {
namespace {

constexpr int kDefaultDpi = 96;

[[nodiscard]] int NonNegative(int value) noexcept
{
	return std::max(0, value);
}

} // namespace

void ActivityBarModel::SetViewport(int widthPixels, int heightPixels, unsigned int dpi) noexcept
{
	m_widthPixels = NonNegative(widthPixels);
	m_heightPixels = NonNegative(heightPixels);
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	Reflow();
}

void ActivityBarModel::SetSelectedItem(std::optional<ActivityBarItem> item) noexcept
{
	m_selected = item && IsValid(*item) ? item : std::nullopt;
}

void ActivityBarModel::SetEnabled(ActivityBarItem item, bool enabled) noexcept
{
	if (!IsValid(item)) return;
	m_enabled[ToIndex(item)] = enabled;
	if (!enabled) {
		if (m_hovered == item) m_hovered.reset();
		if (m_pressed == item) m_pressed.reset();
		if (m_focused == item) m_focused.reset();
		if (m_selected == item) m_selected.reset();
	}
}

bool ActivityBarModel::IsEnabled(ActivityBarItem item) const noexcept
{
	return IsValid(item) && IsInteractive(ToIndex(item));
}

void ActivityBarModel::SetItemVisible(ActivityBarItem item, bool visible) noexcept
{
	if (!IsValid(item)) return;
	const auto index = ToIndex(item);
	if (m_visible[index] == visible) return;
	m_visible[index] = visible;
	if (!visible) {
		if (m_hovered == item) m_hovered.reset();
		if (m_pressed == item) m_pressed.reset();
		if (m_focused == item) m_focused.reset();
		if (m_selected == item) m_selected.reset();
	}
	// A removed entry closes its gap, so every remaining entry shifts.
	Reflow();
}

bool ActivityBarModel::IsVisible(ActivityBarItem item) const noexcept
{
	return IsValid(item) && m_visible[ToIndex(item)];
}

bool ActivityBarModel::IsInteractive(std::size_t index) const noexcept
{
	return index < kItemCount && m_enabled[index] && m_visible[index];
}

void ActivityBarModel::SetHoveredItem(std::optional<ActivityBarItem> item) noexcept
{
	m_hovered = item && IsEnabled(*item) ? item : std::nullopt;
}

void ActivityBarModel::SetPressedItem(std::optional<ActivityBarItem> item) noexcept
{
	m_pressed = item && IsEnabled(*item) ? item : std::nullopt;
}

void ActivityBarModel::SetFocusedItem(std::optional<ActivityBarItem> item) noexcept
{
	m_focused = item && IsEnabled(*item) ? item : std::nullopt;
}

ActivityBarButtonInfo ActivityBarModel::GetButton(std::size_t index) const noexcept
{
	if (index >= kItemCount) return {};
	const auto item = static_cast<ActivityBarItem>(index);
	return {
		.item = item,
		.bounds = m_bounds[index],
		.selected = m_selected == item,
		.hovered = m_hovered == item,
		.pressed = m_pressed == item,
		.focused = m_focused == item,
		.enabled = m_enabled[index],
		.visible = m_visible[index],
	};
}

std::optional<ActivityBarItem> ActivityBarModel::HitTest(int x, int y) const noexcept
{
	for (std::size_t index = 0; index < kItemCount; ++index) {
		if (IsInteractive(index) && m_bounds[index].Contains(x, y)) return static_cast<ActivityBarItem>(index);
	}
	return std::nullopt;
}

std::optional<ActivityBarItem> ActivityBarModel::MoveFocus(int direction) noexcept
{
	if (direction == 0) return m_focused;
	const int step = direction < 0 ? -1 : 1;
	if (!m_focused || !IsEnabled(*m_focused)) {
		m_focused = FirstEnabled(step);
		return m_focused;
	}

	const int start = static_cast<int>(ToIndex(*m_focused));
	for (int offset = 1; offset <= static_cast<int>(kItemCount); ++offset) {
		const int index = (start + step * offset + static_cast<int>(kItemCount)) % static_cast<int>(kItemCount);
		if (IsInteractive(static_cast<std::size_t>(index))) {
			m_focused = static_cast<ActivityBarItem>(index);
			return m_focused;
		}
	}
	m_focused.reset();
	return std::nullopt;
}

std::optional<ActivityBarItem> ActivityBarModel::Invoke(ActivityBarItem item) noexcept
{
	return IsEnabled(item) ? std::optional<ActivityBarItem>{ item } : std::nullopt;
}

std::optional<ActivityBarItem> ActivityBarModel::InvokeFocused() noexcept
{
	return m_focused ? Invoke(*m_focused) : std::nullopt;
}

int ActivityBarModel::GetPreferredWidthPixels() const noexcept
{
	return ScaleDip(kWidthDip, m_dpi);
}

int ActivityBarModel::ScaleDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const auto scaled = (static_cast<std::int64_t>(NonNegative(dip)) * effectiveDpi + kDefaultDpi / 2) / kDefaultDpi;
	return static_cast<int>(std::min<std::int64_t>(scaled, std::numeric_limits<int>::max()));
}

bool ActivityBarModel::IsValid(ActivityBarItem item) const noexcept
{
	return ToIndex(item) < kItemCount;
}

std::optional<ActivityBarItem> ActivityBarModel::FirstEnabled(int direction) const noexcept
{
	if (direction < 0) {
		for (std::size_t index = kItemCount; index-- > 0;) {
			if (IsInteractive(index)) return static_cast<ActivityBarItem>(index);
		}
	} else {
		for (std::size_t index = 0; index < kItemCount; ++index) {
			if (IsInteractive(index)) return static_cast<ActivityBarItem>(index);
		}
	}
	return std::nullopt;
}

void ActivityBarModel::Reflow() noexcept
{
	const int slot = ScaleDip(kButtonExtentDip, m_dpi);
	const int right = std::min(m_widthPixels, GetPreferredWidthPixels());
	// Only present entries occupy a slot, so a container moved out of the Primary Side
	// Bar leaves no gap behind, exactly as VS Code's composite bar reflows.
	std::size_t slotIndex = 0;
	for (std::size_t index = 0; index < kItemCount; ++index) {
		if (!m_visible[index]) {
			m_bounds[index] = {};
			continue;
		}
		const auto top64 = static_cast<std::int64_t>(slot) * slotIndex;
		++slotIndex;
		const int top = static_cast<int>(std::min<std::int64_t>(top64, m_heightPixels));
		const int bottom = std::min(m_heightPixels, static_cast<int>(std::min<std::int64_t>(
			top64 + slot, std::numeric_limits<int>::max())));
		m_bounds[index] = { 0, top, right, std::max(top, bottom) };
	}
}

} // namespace workbench::activity
