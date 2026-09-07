/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/editor/SenpReadonlyWorkbench.h"

#include <windows.h>

namespace workbench::editor {

enum class SenpSurfaceProjection : std::uint8_t { Applied, Empty, Unavailable, Closed };

//! Native projection of one existing editor group, with borrowed, retained surfaces.
//! Bind before Show. Unbind/Close before destroying the borrowed HWND. The frame owns
//! tabs and surface construction; this adapter owns visibility, placement and focus.
//! It never creates, clears, saves, or destroys a CEditDoc or any borrowed surface.
//! Call Apply after a coalesced core notification, including closes outside this API.
class SenpEditorSurfaceSwitcher final {
public:
	SenpEditorSurfaceSwitcher(SenpReadonlyWorkbench& workbench, HWND parent);
	~SenpEditorSurfaceSwitcher();
	SenpEditorSurfaceSwitcher(const SenpEditorSurfaceSwitcher&) = delete;
	SenpEditorSurfaceSwitcher& operator=(const SenpEditorSurfaceSwitcher&) = delete;
	[[nodiscard]] bool Bind(std::string inputId, HWND surface, HWND initialFocus = nullptr);
	//! False during a synchronous projection callback: the caller retains its HWND
	//! and retries from the next UI message rather than destroying it in that callback.
	[[nodiscard]] bool Unbind(std::string_view inputId) noexcept;
	[[nodiscard]] SenpSurfaceProjection Show(std::string_view inputId, bool focus = true) noexcept;
	[[nodiscard]] SenpSurfaceProjection Apply(bool focus = false) noexcept;
	[[nodiscard]] SenpSurfaceProjection Layout(RECT bounds) noexcept;
	//! Hide the complete editor part for Panel maximization without changing active input.
	void SetVisible(bool visible) noexcept;
	void Close() noexcept;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::editor
