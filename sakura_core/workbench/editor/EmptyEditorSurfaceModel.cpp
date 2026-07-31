/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/EmptyEditorSurfaceModel.h"

#include <algorithm>

namespace workbench::editor {
namespace {

constexpr unsigned int kDefaultDpi = 96;
constexpr int kActionWidthDip = 288;
constexpr int kActionHeightDip = 28;
constexpr int kActionGapDip = 4;

} // namespace

void EmptyEditorSurfaceModel::SetViewport(int widthPixels, int heightPixels, unsigned int dpi) noexcept
{
	m_widthPixels = std::max(0, widthPixels);
	m_heightPixels = std::max(0, heightPixels);
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	Reflow();
}

EmptyEditorSurfaceActionInfo EmptyEditorSurfaceModel::GetAction(std::size_t index) const noexcept
{
	if (index >= kActionCount) return {};
	const auto action = static_cast<EmptyEditorSurfaceAction>(index);
	return { action, CommandId(action), Label(action), Shortcut(action), m_bounds[index], m_enabled[index],
		m_hovered == action, m_pressed == action, m_focused == action };
}

std::optional<EmptyEditorSurfaceAction> EmptyEditorSurfaceModel::HitTest(int x, int y) const noexcept
{
	for (std::size_t index = 0; index < kActionCount; ++index) {
		if (m_enabled[index] && m_bounds[index].Contains(x, y)) return static_cast<EmptyEditorSurfaceAction>(index);
	}
	return std::nullopt;
}

void EmptyEditorSurfaceModel::SetEnabled(EmptyEditorSurfaceAction action, bool enabled) noexcept
{
	if (!IsValid(action)) return;
	const auto index = ToIndex(action);
	m_enabled[index] = enabled;
	if (!enabled) {
		if (m_hovered == action) m_hovered.reset();
		if (m_pressed == action) m_pressed.reset();
		if (m_focused == action) m_focused.reset();
	}
}

bool EmptyEditorSurfaceModel::IsEnabled(EmptyEditorSurfaceAction action) const noexcept
{
	return IsValid(action) && m_enabled[ToIndex(action)];
}

void EmptyEditorSurfaceModel::SetHovered(std::optional<EmptyEditorSurfaceAction> action) noexcept
{
	m_hovered = action && IsEnabled(*action) ? action : std::nullopt;
}

void EmptyEditorSurfaceModel::SetPressed(std::optional<EmptyEditorSurfaceAction> action) noexcept
{
	m_pressed = action && IsEnabled(*action) ? action : std::nullopt;
}

void EmptyEditorSurfaceModel::SetFocused(std::optional<EmptyEditorSurfaceAction> action) noexcept
{
	m_focused = action && IsEnabled(*action) ? action : std::nullopt;
}

std::optional<EmptyEditorSurfaceAction> EmptyEditorSurfaceModel::MoveFocus(int direction) noexcept
{
	const int step = direction < 0 ? -1 : 1;
	const int count = static_cast<int>(kActionCount);
	int current = m_focused ? static_cast<int>(*m_focused) : (step > 0 ? -1 : 0);
	for (int attempt = 0; attempt < count; ++attempt) {
		current = (current + step + count) % count;
		const auto candidate = static_cast<EmptyEditorSurfaceAction>(current);
		if (IsEnabled(candidate)) {
			m_focused = candidate;
			return candidate;
		}
	}
	m_focused.reset();
	return std::nullopt;
}

std::optional<EmptyEditorSurfaceAction> EmptyEditorSurfaceModel::Invoke(EmptyEditorSurfaceAction action) const noexcept
{
	return IsEnabled(action) ? std::optional<EmptyEditorSurfaceAction>(action) : std::nullopt;
}

std::optional<EmptyEditorSurfaceAction> EmptyEditorSurfaceModel::InvokeFocused() const noexcept
{
	return m_focused ? Invoke(*m_focused) : std::nullopt;
}

int EmptyEditorSurfaceModel::ScaleDip(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? kDefaultDpi : dpi) + static_cast<int>(kDefaultDpi / 2)) / static_cast<int>(kDefaultDpi));
}

bool EmptyEditorSurfaceModel::IsValid(EmptyEditorSurfaceAction action) const noexcept
{
	return ToIndex(action) < kActionCount;
}

void EmptyEditorSurfaceModel::Reflow() noexcept
{
	const int width = ScaleDip(kActionWidthDip, m_dpi);
	const int height = ScaleDip(kActionHeightDip, m_dpi);
	const int gap = ScaleDip(kActionGapDip, m_dpi);
	const int totalHeight = height * static_cast<int>(kActionCount) + gap * static_cast<int>(kActionCount - 1);
	const int left = std::max(0, (m_widthPixels - width) / 2);
	const int availableWidth = std::max(0, std::min(width, m_widthPixels));
	const int top = std::max(0, (m_heightPixels - totalHeight) / 2);
	for (std::size_t index = 0; index < kActionCount; ++index) {
		const int actionTop = std::min(m_heightPixels, top + static_cast<int>(index) * (height + gap));
		m_bounds[index] = { left, actionTop, left + availableWidth,
			std::min(m_heightPixels, actionTop + height) };
	}
}

} // namespace workbench::editor
