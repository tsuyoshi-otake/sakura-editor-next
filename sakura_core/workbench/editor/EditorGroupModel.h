/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::editor {

//! The sole first-slice editor group. It always exists, even with no inputs and no active editor.
class EditorGroupModel final {
public:
	[[nodiscard]] bool Contains(std::string_view inputId) const noexcept;
	//! Adds an input and, when requested, selects it. Inactive opens preserve the current selection.
	[[nodiscard]] bool Open(EditorInputDescriptor input, std::wstring documentKey, bool activate = true);
	//! Returns false when absent or already active. The selected input must already exist.
	[[nodiscard]] bool Show(std::string_view inputId);
	//! Replaces only the canonical document binding, preserving input ID, order, and active selection.
	//! All potentially allocating work completes before the in-place no-throw swap.
	[[nodiscard]] bool ReplaceDocument(std::string_view inputId, EditorDocumentIdentity documentIdentity, std::wstring documentKey);
	//! Removes one input and returns it. An active close selects the next input, then the prior one at the end.
	[[nodiscard]] std::optional<EditorInputSnapshot> Close(std::string_view inputId);
	[[nodiscard]] std::optional<std::wstring> DocumentKeyFor(std::string_view inputId) const;
	[[nodiscard]] EditorGroupSnapshot Snapshot() const;

private:
	struct Input {
		EditorInputDescriptor descriptor;
		std::wstring documentKey;
	};

	std::vector<Input> m_inputs;
	std::optional<std::string> m_activeInputId;
};

} // namespace workbench::editor
