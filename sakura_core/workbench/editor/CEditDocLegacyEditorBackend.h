/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/ILegacyEditorBackend.h"

#include <optional>
#include <string>

class CEditDoc;

namespace workbench::editor {

/*!
	@brief Explicit, read-only adoption boundary for the one legacy CEditDoc backing object.

	CEditDoc is allocated for the lifetime of an editor process.  It does not by
	itself represent an open workbench input: callers prepare a candidate only after
	the corresponding legacy open/new transaction has succeeded, then ask the
	adapter to adopt that candidate into EditorCoreService.
*/
class CEditDocLegacyEditorBackend final : public ILegacyEditorBackend {
public:
	explicit CEditDocLegacyEditorBackend(CEditDoc& document) noexcept;

	//! Prepares the current successfully loaded legacy file as a file URI candidate.
	[[nodiscard]] bool PrepareFileInput() noexcept;
	//! Prepares a successfully created legacy untitled buffer under a bounded opaque identity.
	[[nodiscard]] bool PrepareUntitledInput(std::string opaqueId) noexcept;
	//! Removes the candidate; it never modifies the legacy document.
	void ClearInput() noexcept;
	[[nodiscard]] bool HasPreparedInput() const noexcept;

	//! Reads only an explicitly prepared, still-current legacy input. No legacy state or event is mutated.
	[[nodiscard]] std::optional<ResolvedEditorDocument> TryGetCurrentDocument() const override;

private:
	enum class EPreparedInputKind : unsigned char {
		File,
		Untitled,
	};

	struct PreparedInput {
		EPreparedInputKind kind = EPreparedInputKind::File;
		EditorDocumentIdentity identity;
		//! Used only to reject a file candidate after the backing document has changed paths.
		std::wstring filePath;
	};

	CEditDoc& m_document;
	mutable std::optional<PreparedInput> m_preparedInput;
};

} // namespace workbench::editor
