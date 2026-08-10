/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#pragma once

#include "env/ShareDataTestSuite.hpp"
#include "window/UiaTestSuite.hpp"

#include "doc/CEditDoc.h"
#include "window/CEditWnd.h"

#include "CEditApp.h"

#include "agent/CGrepAgent.h"
#include "agent/CLoadAgent.h"
#include "agent/CSaveAgent.h"
#include "env/CPropertyManager.h"
#include "macro/CSMacroMgr.h"
#include "recent/CMruListener.h"
#include "uiparts/CVisualProgress.h"

namespace window {

struct EditorTestSuite : public env::ShareDataTestSuite
{
	// CEditApp owns these objects. The fixture borrows them for legacy tests.
	static inline CEditDoc* pcEditDoc = nullptr;
	static inline CEditWnd* pcEditWnd = nullptr;
	static inline CSMacroMgr* pcSMacroMgr = nullptr;

	static inline CLoadAgent* pcLoadAgent = nullptr;
	static inline CSaveAgent* pcSaveAgent = nullptr;
	static inline CVisualProgress* pcVisualProgress = nullptr;
	static inline CGrepAgent* pcGrepAgent = nullptr;
	static inline CMruListener* pcMruListener = nullptr;
	static inline CPropertyManager* pcPropertyManager = nullptr;

	static void SetUpEditor();
	static void TearDownEditor();
};

} // namespace env
