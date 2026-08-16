/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/ActivityBarModel.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::activity {
namespace {

constexpr int kDefaultDpi = 96;

[[nodiscard]] int NonNegative(int value) noexcept
{
	return std::max(0, value);
}

} // namespace

void ActivityBarModel::SetEntries(std::vector<ActivityBarEntry> entries)
{
	// Identity, not position, decides what survives: an extension that reconnects re-sends the
	// same container id, and the user must not lose the selected container because a different
	// extension happened to register earlier this time.
	const std::string selected(IdAt(m_selected));
	const std::string hovered(IdAt(m_hovered));
	const std::string pressed(IdAt(m_pressed));
	const std::string focused(IdAt(m_focused));

	std::erase_if(entries, [](const ActivityBarEntry& entry) { return entry.id.empty(); });
	m_entries = std::move(entries);
	m_bounds.assign(m_entries.size(), ActivityBarRect{});

	m_selected = ResolveInteractive(selected);
	m_hovered = ResolveInteractive(hovered);
	m_pressed = ResolveInteractive(pressed);
	m_focused = ResolveInteractive(focused);
	Reflow();
}

void ActivityBarModel::SetViewport(int widthPixels, int heightPixels, unsigned int dpi) noexcept
{
	m_widthPixels = NonNegative(widthPixels);
	m_heightPixels = NonNegative(heightPixels);
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	Reflow();
}

std::size_t ActivityBarModel::IndexOf(std::string_view id) const noexcept
{
	if (id.empty()) return kNoIndex;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (m_entries[index].id == id) return index;
	}
	return kNoIndex;
}

std::string_view ActivityBarModel::IdAt(std::size_t index) const noexcept
{
	return index < m_entries.size() ? std::string_view(m_entries[index].id) : std::string_view{};
}

std::size_t ActivityBarModel::ResolveInteractive(std::string_view id) const noexcept
{
	const auto index = IndexOf(id);
	return IsInteractive(index) ? index : kNoIndex;
}

void ActivityBarModel::SetSelectedItem(std::string_view id) noexcept
{
	const auto index = ResolveInteractive(id);
	if (index != kNoIndex && m_entries[index].IsGlobalAction()) {
		m_selected = kNoIndex;
		return;
	}
	m_selected = index;
}

bool ActivityBarModel::IsDraggable(std::string_view id) const noexcept
{
	const auto index = IndexOf(id);
	return IsInteractive(index) && !m_entries[index].IsGlobalAction();
}

void ActivityBarModel::SetEnabled(std::string_view id, bool enabled) noexcept
{
	const auto index = IndexOf(id);
	if (index == kNoIndex) return;
	m_entries[index].enabled = enabled;
	ForgetUnusableStates();
}

bool ActivityBarModel::IsEnabled(std::string_view id) const noexcept
{
	return IsInteractive(IndexOf(id));
}

void ActivityBarModel::SetItemVisible(std::string_view id, bool visible) noexcept
{
	const auto index = IndexOf(id);
	if (index == kNoIndex || m_entries[index].visible == visible) return;
	m_entries[index].visible = visible;
	ForgetUnusableStates();
	// A removed entry closes its gap, so every remaining entry shifts.
	Reflow();
}

bool ActivityBarModel::IsVisible(std::string_view id) const noexcept
{
	const auto index = IndexOf(id);
	return index != kNoIndex && m_entries[index].visible;
}

bool ActivityBarModel::IsInteractive(std::size_t index) const noexcept
{
	return index < m_entries.size() && m_entries[index].enabled && m_entries[index].visible;
}

void ActivityBarModel::ForgetUnusableStates() noexcept
{
	for (auto* state : { &m_selected, &m_hovered, &m_pressed, &m_focused }) {
		if (!IsInteractive(*state)) *state = kNoIndex;
	}
}

void ActivityBarModel::SetHoveredItem(std::string_view id) noexcept
{
	m_hovered = ResolveInteractive(id);
}

void ActivityBarModel::SetPressedItem(std::string_view id) noexcept
{
	m_pressed = ResolveInteractive(id);
}

void ActivityBarModel::SetFocusedItem(std::string_view id) noexcept
{
	m_focused = ResolveInteractive(id);
}

ActivityBarButtonInfo ActivityBarModel::GetButton(std::size_t index) const noexcept
{
	if (index >= m_entries.size()) return {};
	const auto& entry = m_entries[index];
	return {
		.id = entry.id,
		.label = entry.label,
		.codicon = entry.codicon,
		.bounds = m_bounds[index],
		.kind = entry.kind,
		.selected = m_selected == index,
		.hovered = m_hovered == index,
		.pressed = m_pressed == index,
		.focused = m_focused == index,
		.enabled = entry.enabled,
		.visible = entry.visible,
	};
}

std::string_view ActivityBarModel::HitTest(int x, int y) const noexcept
{
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (IsInteractive(index) && m_bounds[index].Contains(x, y)) return m_entries[index].id;
	}
	return {};
}

std::string_view ActivityBarModel::MoveFocus(int direction) noexcept
{
	if (m_entries.empty()) return {};
	if (direction == 0) return IdAt(m_focused);
	const int step = direction < 0 ? -1 : 1;
	if (!IsInteractive(m_focused)) {
		m_focused = FirstEnabled(step);
		return IdAt(m_focused);
	}

	const auto count = m_entries.size();
	for (std::size_t offset = 1; offset <= count; ++offset) {
		// Walking backwards is walking forwards by (count - 1) steps, which keeps the
		// arithmetic unsigned and free of the wrap-around sign games a negative step needs.
		const std::size_t stride = step < 0 ? count - 1 : 1;
		const std::size_t index = (m_focused + stride * offset) % count;
		if (IsInteractive(index)) {
			m_focused = index;
			return IdAt(m_focused);
		}
	}
	m_focused = kNoIndex;
	return {};
}

std::string_view ActivityBarModel::FocusEdge(int direction) noexcept
{
	m_focused = FirstEnabled(direction < 0 ? -1 : 1);
	return IdAt(m_focused);
}

std::string_view ActivityBarModel::Invoke(std::string_view id) const noexcept
{
	const auto index = ResolveInteractive(id);
	return IdAt(index);
}

std::string_view ActivityBarModel::InvokeFocused() const noexcept
{
	return IsInteractive(m_focused) ? IdAt(m_focused) : std::string_view{};
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

std::size_t ActivityBarModel::FirstEnabled(int direction) const noexcept
{
	if (direction < 0) {
		for (std::size_t index = m_entries.size(); index-- > 0;) {
			if (IsInteractive(index)) return index;
		}
	} else {
		for (std::size_t index = 0; index < m_entries.size(); ++index) {
			if (IsInteractive(index)) return index;
		}
	}
	return kNoIndex;
}

void ActivityBarModel::Reflow() noexcept
{
	const int slot = ScaleDip(kButtonExtentDip, m_dpi);
	const int right = std::min(m_widthPixels, GetPreferredWidthPixels());
	// ViewContainers stack from the top; GlobalCompositeBar actions pin to the bottom
	// (Accounts above Manage), matching VS Code's Activity Bar layout.
	std::size_t globalVisible = 0;
	for (const auto& entry : m_entries) {
		if (entry.visible && entry.IsGlobalAction()) ++globalVisible;
	}
	const auto globalHeight64 = static_cast<std::int64_t>(slot) * globalVisible;
	const int containerAreaHeight = static_cast<int>(std::max<std::int64_t>(
		0, static_cast<std::int64_t>(m_heightPixels) - globalHeight64));

	std::size_t containerSlot = 0;
	std::size_t globalSlot = 0;
	for (std::size_t index = 0; index < m_entries.size(); ++index) {
		if (!m_entries[index].visible) {
			m_bounds[index] = {};
			continue;
		}
		if (m_entries[index].IsGlobalAction()) {
			const auto fromBottom = static_cast<std::int64_t>(globalVisible - globalSlot) * slot;
			++globalSlot;
			const int top = static_cast<int>(std::max<std::int64_t>(
				0, static_cast<std::int64_t>(m_heightPixels) - fromBottom));
			const int bottom = std::min(m_heightPixels, top + slot);
			m_bounds[index] = { 0, top, right, std::max(top, bottom) };
			continue;
		}
		const auto top64 = static_cast<std::int64_t>(slot) * containerSlot;
		++containerSlot;
		const int top = static_cast<int>(std::min<std::int64_t>(top64, containerAreaHeight));
		const int bottom = std::min(containerAreaHeight, static_cast<int>(std::min<std::int64_t>(
			top64 + slot, std::numeric_limits<int>::max())));
		m_bounds[index] = { 0, top, right, std::max(top, bottom) };
	}
}

} // namespace workbench::activity
