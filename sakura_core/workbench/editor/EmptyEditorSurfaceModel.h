/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCommandIds.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace workbench::editor {

//! Stable action identifiers for the empty editor welcome surface.
enum class EmptyEditorSurfaceAction : std::uint8_t {
	NewFile,
	OpenFile,
	OpenFolder,
	ShowAllCommands,
	OpenSettings,
	Count,
};

//! A platform-neutral pixel rectangle used by the pure welcome-surface model.
struct EmptyEditorSurfaceRect {
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
	[[nodiscard]] constexpr bool operator==(const EmptyEditorSurfaceRect&) const noexcept = default;
};

//! The immutable presentation metadata and mutable interaction state of one action.
struct EmptyEditorSurfaceActionInfo {
	EmptyEditorSurfaceAction action = EmptyEditorSurfaceAction::NewFile;
	std::string_view commandId;
	const wchar_t* label = L"";
	const wchar_t* shortcut = L"";
	EmptyEditorSurfaceRect bounds{};
	bool enabled = true;
	bool hovered = false;
	bool pressed = false;
	bool focused = false;
};

//! HWND-free geometry and interaction state for CEmptyEditorSurface.
//!
//! The welcome surface is deliberately a view model only. It has no document or
//! editor-core dependency: its owner decides whether an empty editor group is visible.
class EmptyEditorSurfaceModel final {
public:
	static constexpr std::size_t kActionCount = static_cast<std::size_t>(EmptyEditorSurfaceAction::Count);

	//! Sets physical client dimensions and recalculates the centered action list.
	void SetViewport(int widthPixels, int heightPixels, unsigned int dpi = 96) noexcept;
	[[nodiscard]] unsigned int GetDpi() const noexcept { return m_dpi; }
	[[nodiscard]] int GetWidthPixels() const noexcept { return m_widthPixels; }
	[[nodiscard]] int GetHeightPixels() const noexcept { return m_heightPixels; }

	//! Square logo box above the action list, mirroring VS Code's `.letterpress` watermark.
	//!
	//! An empty rectangle means the viewport is too short to show the logo; the action list
	//! then owns the whole centered block exactly as it did before the logo existed.
	[[nodiscard]] EmptyEditorSurfaceRect GetLetterpressBounds() const noexcept { return m_letterpress; }

	[[nodiscard]] std::size_t GetActionCount() const noexcept { return kActionCount; }
	[[nodiscard]] EmptyEditorSurfaceActionInfo GetAction(std::size_t index) const noexcept;
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> HitTest(int x, int y) const noexcept;
	void SetEnabled(EmptyEditorSurfaceAction action, bool enabled) noexcept;
	[[nodiscard]] bool IsEnabled(EmptyEditorSurfaceAction action) const noexcept;
	void SetHovered(std::optional<EmptyEditorSurfaceAction> action) noexcept;
	void SetPressed(std::optional<EmptyEditorSurfaceAction> action) noexcept;
	void SetFocused(std::optional<EmptyEditorSurfaceAction> action) noexcept;
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> GetHovered() const noexcept { return m_hovered; }
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> GetPressed() const noexcept { return m_pressed; }
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> GetFocused() const noexcept { return m_focused; }
	//! Moves through enabled actions, wrapping at either end. Zero is treated as forward.
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> MoveFocus(int direction) noexcept;
	//! Returns the action only when it is enabled. The native host owns command dispatch.
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> Invoke(EmptyEditorSurfaceAction action) const noexcept;
	[[nodiscard]] std::optional<EmptyEditorSurfaceAction> InvokeFocused() const noexcept;

	[[nodiscard]] static constexpr std::string_view CommandId(EmptyEditorSurfaceAction action) noexcept;
	[[nodiscard]] static constexpr const wchar_t* Label(EmptyEditorSurfaceAction action) noexcept;
	[[nodiscard]] static constexpr const wchar_t* Shortcut(EmptyEditorSurfaceAction action) noexcept;

private:
	[[nodiscard]] static constexpr std::size_t ToIndex(EmptyEditorSurfaceAction action) noexcept
	{
		return static_cast<std::size_t>(action);
	}
	[[nodiscard]] static int ScaleDip(int dip, unsigned int dpi) noexcept;
	[[nodiscard]] bool IsValid(EmptyEditorSurfaceAction action) const noexcept;
	void Reflow() noexcept;

	int m_widthPixels = 0;
	int m_heightPixels = 0;
	unsigned int m_dpi = 96;
	EmptyEditorSurfaceRect m_letterpress{};
	std::array<EmptyEditorSurfaceRect, kActionCount> m_bounds{};
	std::array<bool, kActionCount> m_enabled{ true, true, true, true, true };
	std::optional<EmptyEditorSurfaceAction> m_hovered;
	std::optional<EmptyEditorSurfaceAction> m_pressed;
	std::optional<EmptyEditorSurfaceAction> m_focused;
};

constexpr std::string_view EmptyEditorSurfaceModel::CommandId(EmptyEditorSurfaceAction action) noexcept
{
	switch (action) {
	case EmptyEditorSurfaceAction::NewFile: return command_ids::NewUntitledFile;
	case EmptyEditorSurfaceAction::OpenFile: return command_ids::OpenFile;
	case EmptyEditorSurfaceAction::OpenFolder: return command_ids::OpenFolder;
	case EmptyEditorSurfaceAction::ShowAllCommands: return command_ids::ShowCommands;
	case EmptyEditorSurfaceAction::OpenSettings: return command_ids::OpenSettings;
	case EmptyEditorSurfaceAction::Count: break;
	}
	return {};
}

constexpr const wchar_t* EmptyEditorSurfaceModel::Label(EmptyEditorSurfaceAction action) noexcept
{
	switch (action) {
	// The model is deliberately presentation-neutral.  Localized labels are
	// resolved by CEmptyEditorSurface at the native UI boundary.
	case EmptyEditorSurfaceAction::NewFile: return L"New File";
	case EmptyEditorSurfaceAction::OpenFile: return L"Open File...";
	case EmptyEditorSurfaceAction::OpenFolder: return L"Open Folder...";
	case EmptyEditorSurfaceAction::ShowAllCommands: return L"Show All Commands";
	case EmptyEditorSurfaceAction::OpenSettings: return L"Open Settings";
	case EmptyEditorSurfaceAction::Count: break;
	}
	return L"";
}

constexpr const wchar_t* EmptyEditorSurfaceModel::Shortcut(EmptyEditorSurfaceAction action) noexcept
{
	switch (action) {
	case EmptyEditorSurfaceAction::NewFile: return L"Ctrl+N";
	case EmptyEditorSurfaceAction::OpenFile: return L"Ctrl+O";
	// VS Code's Windows default is the two-stroke Ctrl+K, Ctrl+O chord.
	case EmptyEditorSurfaceAction::OpenFolder: return L"Ctrl+K Ctrl+O";
	case EmptyEditorSurfaceAction::ShowAllCommands: return L"Ctrl+Shift+P";
	case EmptyEditorSurfaceAction::OpenSettings: return L"Ctrl+,";
	case EmptyEditorSurfaceAction::Count: break;
	}
	return L"";
}

} // namespace workbench::editor
