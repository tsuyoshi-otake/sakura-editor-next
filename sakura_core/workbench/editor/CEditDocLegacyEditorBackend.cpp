/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "workbench/editor/CEditDocLegacyEditorBackend.h"

#include "doc/CEditDoc.h"

namespace workbench::editor {

CEditDocLegacyEditorBackend::CEditDocLegacyEditorBackend(CEditDoc& document) noexcept
	: m_document(document)
{
}

bool CEditDocLegacyEditorBackend::PrepareFileInput() noexcept
{
	ClearInput();
	try {
		const auto& file = m_document.m_cDocFile;
		if (!file.GetFilePathClass().IsValidPath()) return false;

		std::wstring path(file.GetFilePath());
		auto uri = platform::uri::Uri::FromWindowsPath(path);
		if (!uri) return false;

		m_preparedInput = PreparedInput{
			.kind = EPreparedInputKind::File,
			.identity = EditorDocumentIdentity{ .resource = std::move(*uri.value) },
			.filePath = std::move(path),
		};
		return true;
	}
	catch (...) {
		// A failed preparation is terminal: never leave an older path/identity available for adoption.
		ClearInput();
		return false;
	}
}

bool CEditDocLegacyEditorBackend::PrepareUntitledInput(std::string opaqueId) noexcept
{
	ClearInput();
	if (!IsValidEditorExternalId(opaqueId, kMaxEditorOpaqueDocumentIdLength)) return false;
	try {
		m_preparedInput = PreparedInput{
			.kind = EPreparedInputKind::Untitled,
			.identity = EditorDocumentIdentity{ .opaqueId = std::move(opaqueId) },
		};
		return true;
	}
	catch (...) {
		ClearInput();
		return false;
	}
}

void CEditDocLegacyEditorBackend::ClearInput() noexcept
{
	m_preparedInput.reset();
}

bool CEditDocLegacyEditorBackend::HasPreparedInput() const noexcept
{
	return m_preparedInput.has_value();
}

std::optional<ResolvedEditorDocument> CEditDocLegacyEditorBackend::TryGetCurrentDocument() const
{
	try {
		if (!m_preparedInput) return std::nullopt;
		const auto& prepared = *m_preparedInput;
		if (prepared.kind == EPreparedInputKind::File) {
			const auto& file = m_document.m_cDocFile;
			if (!file.GetFilePathClass().IsValidPath()
				|| std::wstring(file.GetFilePath()) != prepared.filePath) {
				// A legacy path replacement without a new successful preparation cannot adopt stale identity.
				m_preparedInput.reset();
				return std::nullopt;
			}
		}
		return ResolvedEditorDocument{
			.identity = prepared.identity,
			// CEditDoc has no stable public monotonic text revision yet. First adoption is deliberately 0.
			.documentRevision = 0,
			.dirty = m_document.m_cDocEditor.IsModified(),
		};
	}
	catch (...) {
		// This read boundary must not expose an old candidate after a failed validation/read.
		m_preparedInput.reset();
		throw;
	}
}

} // namespace workbench::editor
