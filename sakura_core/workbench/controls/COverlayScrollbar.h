/*! @file
	@brief The workbench's VS Code-style overlay vertical scrollbar

	VS Code does not use the platform scrollbar in its lists: it draws a thin
	overlay that sits on top of the scrolled content, has no arrow buttons, and
	takes no layout width. The native controls this shell hosts (a TreeView in the
	Explorer, an owner-drawn LISTBOX in Source Control) each keep a real SB_VERT
	scroll state, so the overlay is a separate child window that reads that state
	and drives it back through one callback.

	One implementation serves every such view. A per-view copy is what let the
	Source Control list keep the platform scrollbar while the Explorer had the
	themed one, which is exactly the kind of divergence this component removes.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <functional>

namespace workbench::controls {

//! Theme tokens the overlay paints with, mapped by the owning view from its own
//! palette so this component never depends on a particular palette type.
struct OverlayScrollbarColors {
	COLORREF background = RGB(0x20, 0x23, 0x2A);
	COLORREF trackHover = RGB(0x2A, 0x2E, 0x36);
	COLORREF thumb = RGB(0x38, 0x3E, 0x49);
	COLORREF thumbHover = RGB(0x8B, 0x91, 0x9B);
};

//! Which axis the overlay scrolls. VS Code paints the same thin bar on both.
enum class OverlayScrollbarOrientation {
	Vertical,
	Horizontal,
};

//! Where the overlay reads its scroll state, and how it places itself.
enum class OverlayScrollbarSource {
	//! `SB_VERT` of the target window, placed over the target's client area.
	//! The target keeps its own scrolling; the overlay hides the platform bar.
	TargetWindowBar,
	/*!
		@brief `SB_CTL` of a standalone `WC_SCROLLBAR` control the owner keeps

		The editor view owns a real scrollbar control that already carries the
		layout-line range, page, and position. Reusing that state keeps one model
		while the platform control itself stops being drawn: the owner hides it
		and gives the overlay the same rectangle through SetBounds().
	*/
	ScrollbarControl,
};

/*!
	@brief An overlay scrollbar bound to one scrolling child control

	The target control remains the source of truth: its SB_VERT SCROLLINFO gives
	the range, page, and position, and `setTopRow` is how the overlay asks it to
	scroll. The owner calls Update() whenever the target's content, size, scroll
	position, or visibility may have changed.
*/
class COverlayScrollbar final {
	using Me = COverlayScrollbar;

public:
	//! Scrolls the target so that `topRow` becomes its first visible row.
	using SetTopRowCallback = std::function<void(int topRow)>;

	COverlayScrollbar() = default;
	~COverlayScrollbar();
	COverlayScrollbar(const Me&) = delete;
	Me& operator=(const Me&) = delete;
	COverlayScrollbar(Me&&) = delete;
	Me& operator=(Me&&) = delete;

	/*!
		@brief Creates the overlay over `target` as a child of `parent`

		@param parent The window the overlay is positioned in, normally the same
		       parent the target control has.
		@param target The scrolling control whose SB_VERT state the overlay reads.
		@param setTopRow Applied when the user drags the thumb or clicks the track.
	*/
	[[nodiscard]] bool Create(HWND parent, HWND target, SetTopRowCallback setTopRow,
		OverlayScrollbarSource source = OverlayScrollbarSource::TargetWindowBar,
		OverlayScrollbarOrientation orientation = OverlayScrollbarOrientation::Vertical);

	//! Destroys the overlay window. Safe to call when it was never created.
	void Destroy() noexcept;

	//! Forgets the overlay window without destroying it, for a parent-destroyed path.
	void Detach() noexcept;

	[[nodiscard]] HWND Get() const noexcept { return m_window; }

	void SetDpi(unsigned int dpi) noexcept { m_dpi = dpi == 0 ? 96u : dpi; }

	/*!
		@brief Places the overlay explicitly, in parent client coordinates

		Required for OverlayScrollbarSource::ScrollbarControl, whose target is a
		hidden control rather than the scrolled surface. Once set, Update() uses
		this rectangle instead of deriving one from the target.
	*/
	void SetBounds(const RECT& parentRect) noexcept { m_bounds = parentRect; m_hasBounds = true; }
	void SetColors(const OverlayScrollbarColors& colors) noexcept { m_colors = colors; }

	//! Repositions, shows or hides, and repaints the overlay from the target's state.
	void Update();

	//! Repaints without recomputing placement, for a palette change.
	void Invalidate() const noexcept;

private:
	struct Layout {
		RECT track{};
		RECT thumb{};
		int totalRows{};
		int visibleRows{};
		int topRow{};
		int maximumTop{};
		bool scrollable{};
	};

	[[nodiscard]] int ScaleDip(int value) const noexcept;
	//! The window that owns the scrolled content, for focus and wheel routing.
	[[nodiscard]] HWND ScrolledWindow() const noexcept;
	[[nodiscard]] Layout GetLayout() const noexcept;
	void Paint(HDC dc) const;
	void UpdateHover(POINT point);
	void DragTo(int pointerPosition);
	//! True while the overlay scrolls the horizontal axis; the layout swaps axes.
	[[nodiscard]] bool IsHorizontal() const noexcept {
		return m_orientation == OverlayScrollbarOrientation::Horizontal;
	}
	void EndDrag(bool releaseCapture) noexcept;
	void ScrollToTopRow(int topRow);

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
	[[nodiscard]] static bool EnsureClass(HINSTANCE instance);

	HWND m_window{};
	HWND m_parent{};
	HWND m_target{};
	OverlayScrollbarSource m_source = OverlayScrollbarSource::TargetWindowBar;
	OverlayScrollbarOrientation m_orientation = OverlayScrollbarOrientation::Vertical;
	RECT m_bounds{};
	bool m_hasBounds{};
	SetTopRowCallback m_setTopRow;
	OverlayScrollbarColors m_colors;
	unsigned int m_dpi = 96;
	bool m_hover{};
	bool m_dragging{};
	bool m_trackingMouseLeave{};
	int m_thumbGrabOffset{};
};

} // namespace workbench::controls
