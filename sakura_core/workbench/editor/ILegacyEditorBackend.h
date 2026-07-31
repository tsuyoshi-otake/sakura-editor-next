/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreTypes.h"

#include <optional>

namespace workbench::editor {

/*!
	@brief Read boundary between the authoritative workbench model and the legacy editor.

	A legacy CEditDoc can exist as an inert backing object while the workbench has no
	open editor input. Implementations therefore return std::nullopt unless a real,
	supported legacy input has explicitly been adopted by the workbench.

	Imperative open/save/close methods intentionally do not belong to this first
	boundary: the current legacy operations have no prepare/commit/rollback contract.
*/
class ILegacyEditorBackend {
public:
	virtual ~ILegacyEditorBackend() = default;

	[[nodiscard]] virtual std::optional<ResolvedEditorDocument> TryGetCurrentDocument() const = 0;
};

} // namespace workbench::editor
