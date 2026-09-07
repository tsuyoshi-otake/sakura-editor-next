/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "theme/CThemeService.h"
#include <Windows.h>

namespace workbench::viewcontainer {

//! Native companion to the retained page contract, independent of built-in
//! Explorer/SCM/Search tools. Every contributed page must implement this port.
class IViewContainerPageProjection {
public:
	virtual ~IViewContainerPageProjection() = default;
	virtual void ActivateProjection() noexcept = 0;
	virtual void DeactivateProjection() noexcept = 0;
	[[nodiscard]] virtual bool PreTranslateProjection(MSG& message) noexcept = 0;
	//! Host bounds and root-local content are applied together; neither half may
	//! be omitted when reparenting between physical workbench hosts.
	virtual void LayoutProjection(const RECT& hostBounds, const RECT& contentBounds,
		unsigned int dpi) noexcept = 0;
	virtual void SetProjectionVisible(bool visible) noexcept = 0;
	virtual void SetProjectionPalette(const theme::ThemePalette&) noexcept {}
	virtual void RefreshProjectionStrings() noexcept {}
	//! Explicit semantic invalidation. Never poll or activate an inactive page.
	virtual void RefreshProjectionContent() noexcept {}
};

} // namespace workbench::viewcontainer
