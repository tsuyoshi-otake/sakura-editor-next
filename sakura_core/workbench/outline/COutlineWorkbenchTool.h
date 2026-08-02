/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"

#include <CommCtrl.h>
#include <cstdint>

class CDlgFuncList;

namespace workbench::outline {

enum class OutlineToolLifecycle : std::uint8_t {
	Idle,
	Inactive,
	Active,
	Closed,
};

enum class OutlineToolEvent : std::uint8_t {
	Create,
	Activate,
	Deactivate,
	Close,
};

struct OutlineToolLayout {
	RECT bounds{};
	unsigned int dpi = 96;

	friend bool operator==( const OutlineToolLayout& left, const OutlineToolLayout& right ) noexcept
	{
		return left.bounds.left == right.bounds.left
			&& left.bounds.top == right.bounds.top
			&& left.bounds.right == right.bounds.right
			&& left.bounds.bottom == right.bounds.bottom
			&& left.dpi == right.dpi;
	}
};

//! Pure lifecycle reducer. Closed is terminal and unsupported transitions are no-ops.
[[nodiscard]] OutlineToolLifecycle AdvanceOutlineToolLifecycle(
	OutlineToolLifecycle current,
	OutlineToolEvent event ) noexcept;

//! Normalizes host geometry before it reaches SetWindowPos.
[[nodiscard]] OutlineToolLayout NormalizeOutlineToolLayout(
	const RECT& contentRect,
	unsigned int dpi ) noexcept;

//! A created outline child stays visible while its panel is visible, even when the
//! editor remains focused.  Activation controls keyboard focus, not visibility.
[[nodiscard]] bool ShouldShowOutlineDialog( OutlineToolLifecycle lifecycle ) noexcept;

//! A successful reload that created a dialog after the host layout needs one
//! explicit layout pass.  This decision deliberately has no activation input.
[[nodiscard]] bool ShouldRelayoutOutlineAfterReload(
	bool commandSucceeded,
	bool rightPanelVisible,
	bool dialogCreated ) noexcept;

//! Outline is a View nested in the Explorer ViewContainer, so every background
//! surface it owns uses the containing Side Bar Part role.
[[nodiscard]] COLORREF OutlineBackgroundColor( const theme::ThemePalette& palette ) noexcept;

//! Adapts the existing outline dialog to the deliberately small IWorkbenchTool boundary.
//!
//! Create establishes the workbench parent/mode but does not force outline parsing. The
	//! existing Command_FUNCLIST path may create CDlgFuncList later; the next Layout call
	//! discovers that child, places it in the right panel, and shows it without taking focus.
class COutlineWorkbenchTool final : public IWorkbenchTool {
public:
	explicit COutlineWorkbenchTool( CDlgFuncList& dialog ) noexcept;
	~COutlineWorkbenchTool() override;
	COutlineWorkbenchTool( const COutlineWorkbenchTool& ) = delete;
	COutlineWorkbenchTool& operator=( const COutlineWorkbenchTool& ) = delete;

	bool Create( HWND parent ) override;
	void Layout( const RECT& contentRect, unsigned int dpi ) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage( MSG& message ) override;
	void Close() override;
	void SetVisible( bool visible ) noexcept;
	void SetPalette( const theme::ThemePalette& palette );

	//! Moves this View under another host window.  Outline is a View inside the Explorer
	//! ViewContainer, so it travels with that container when VS Code moves it between the
	//! Primary and Secondary Side Bar.  Returns false when there is nothing to reparent.
	bool Reparent( HWND parent ) noexcept;

	[[nodiscard]] OutlineToolLifecycle GetLifecycle() const noexcept { return m_lifecycle; }
	[[nodiscard]] OutlineToolLayout GetLayout() const noexcept { return m_layout; }
	[[nodiscard]] HWND GetParent() const noexcept { return m_parent; }

private:
	void ApplyLayout() noexcept;
	void ApplyAppearance() noexcept;
	void RecreateSymbolImages() noexcept;
	[[nodiscard]] HWND GetDialogWindow() const noexcept;

	CDlgFuncList* m_dialog = nullptr;
	HWND m_parent = nullptr;
	OutlineToolLifecycle m_lifecycle = OutlineToolLifecycle::Idle;
	OutlineToolLayout m_layout{};
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	HIMAGELIST m_symbolImages = nullptr;
	bool m_visible = true;
};

} // namespace workbench::outline
