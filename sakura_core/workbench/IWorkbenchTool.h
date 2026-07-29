/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/WorkbenchLayout.h"

#include <Windows.h>

#include <cstdint>

namespace workbench {

//! The physical edge occupied by a workbench tool.
enum class WorkbenchEdge : std::uint8_t {
	Left,
	Right,
	Bottom,
};

//! A deliberately small boundary between the editor frame and a workbench tool.
//!
//! Implementations only receive their host HWND.  In particular, they cannot depend on
//! CEditWnd or CSplitterWnd, which keeps terminal and explorer lifetime window-local.
class IWorkbenchTool {
public:
	virtual ~IWorkbenchTool() = default;

	virtual bool Create(HWND parent) = 0;
	virtual void Layout(const RECT& contentRect, unsigned int dpi) = 0;
	virtual void Activate() = 0;
	virtual void Deactivate() = 0;
	virtual bool PreTranslateMessage(MSG& message) = 0;
	virtual void Close() = 0;
};

} // namespace workbench
