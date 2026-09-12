/*! @file
	@brief Seam that builds the SENP tool-read broker for a user-data profile.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace workbench::editor {

class ISenpOwnerToolReads;

/*!
	@brief Creates the SENP tool-read broker for one user-data profile.

	Only the composition root that froze this process's control-platform
	authority identity can build the broker, so the editor window is handed
	this seam instead of reaching back into the process object for it. An
	empty result is a normal answer: the owner target then keeps its tool
	reads fail-closed rather than inventing an unauthenticated route.
*/
using SenpOwnerToolReadsFactory =
	std::function<std::unique_ptr<ISenpOwnerToolReads>(const std::wstring& userDataProfileId)>;

} // namespace workbench::editor
