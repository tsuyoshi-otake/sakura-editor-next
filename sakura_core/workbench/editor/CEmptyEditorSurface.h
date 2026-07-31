/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "accessibility/CustomUiAutomationProvider.h"
#include "theme/CThemeService.h"
#include "workbench/editor/EmptyEditorSurfaceModel.h"

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace workbench::editor {

//! Native welcome surface for an editor group that has no active input.
//!
//! This class is a view adapter: it neither reads CEditDoc nor mutates the
//! Editor Core model. Every action is emitted as a stable workbench command ID.
class CEmptyEditorSurface final : public accessibility::ICustomUiAutomationHost {
public:
	using CommandCallback = std::function<void(std::string_view commandId)>;

	explicit CEmptyEditorSurface(CommandCallback onCommand = {});
	~CEmptyEditorSurface();
	CEmptyEditorSurface(const CEmptyEditorSurface&) = delete;
	CEmptyEditorSurface& operator=(const CEmptyEditorSurface&) = delete;

	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance);
	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance, CommandCallback onCommand);
	//! Safe to call repeatedly, including after its parent has already destroyed it.
	void Destroy() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show() noexcept;
	void Hide() noexcept;
	void Focus() noexcept;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	[[nodiscard]] const theme::ThemePalette& GetPalette() const noexcept { return m_palette; }
	void SetCommandCallback(CommandCallback callback) { m_onCommand = std::move(callback); }
	void SetActionEnabled(EmptyEditorSurfaceAction action, bool enabled) noexcept;
	[[nodiscard]] bool Invoke(EmptyEditorSurfaceAction action) noexcept;
	[[nodiscard]] bool PreTranslateMessage(MSG& message) noexcept;
	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }
	[[nodiscard]] bool IsVisible() const noexcept { return m_window != nullptr && ::IsWindowVisible(m_window) != FALSE; }
	[[nodiscard]] const EmptyEditorSurfaceModel& GetModel() const noexcept { return m_model; }

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	[[nodiscard]] HWND AccessibilityWindow() const noexcept override { return m_window; }
	[[nodiscard]] std::shared_ptr<accessibility::CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept override { return m_accessibilityLifetime; }
	[[nodiscard]] std::wstring AccessibilityName() const override { return L"Sakura Editor NEXT の開始"; }
	[[nodiscard]] std::wstring AccessibilityAutomationId() const override { return L"Sakura.EmptyEditorSurface"; }
	[[nodiscard]] CONTROLTYPEID AccessibilityControlType() const noexcept override { return UIA_PaneControlTypeId; }
	[[nodiscard]] int AccessibilityChildCount(int parentId) const noexcept override;
	[[nodiscard]] int AccessibilityChildAt(int parentId, int index) const noexcept override;
	[[nodiscard]] int AccessibilityParent(int nodeId) const noexcept override;
	[[nodiscard]] accessibility::CustomUiAutomationNode AccessibilityNode(int nodeId) const override;
	[[nodiscard]] int AccessibilityFocusedNode() const noexcept override;
	[[nodiscard]] bool AccessibilityInvoke(int nodeId) noexcept override;
	void AccessibilitySetFocus(int nodeId) noexcept override;

	[[nodiscard]] LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void UpdateClientLayout(unsigned int dpi) noexcept;
	void Paint() noexcept;
	void Invalidate() const noexcept;
	[[nodiscard]] bool InvokeRequest(std::optional<EmptyEditorSurfaceAction> action) noexcept;
	[[nodiscard]] bool HandleNavigationKey(WPARAM key) noexcept;
	void SetHoverFromPoint(POINT point) noexcept;

	EmptyEditorSurfaceModel m_model;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	theme::CThemeFont m_wordmarkFont;
	CommandCallback m_onCommand;
	HWND m_window = nullptr;
	std::optional<EmptyEditorSurfaceAction> m_captureAction;
	bool m_trackingMouseLeave = false;
	bool m_destroyed = false;
	bool m_destroying = false;
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};

} // namespace workbench::editor
