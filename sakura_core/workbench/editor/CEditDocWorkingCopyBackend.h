/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/IEditorWorkingCopyBackend.h"

#include <cstdint>
#include <optional>

class CEditDoc;
enum class EDocFileOperationResult : unsigned char;
enum ECodeType : int;
enum class EEolType : char;

namespace workbench::editor {

/*!
	@brief Imperative bridge from a single legacy CEditDoc to the working-copy coordinator.

	The coordinator validates the input identity and core revision before calling this
	bridge. Revert is deliberately unsupported until the legacy document/window pair
	can apply, roll back, and project native state as one complete transaction. Close
	is split so the core commits the input removal before legacy document/view cleanup runs.
*/
class CEditDocWorkingCopyBackend final : public IEditorWorkingCopyBackend {
public:
	explicit CEditDocWorkingCopyBackend(CEditDoc& document) noexcept;

	[[nodiscard]] EditorWorkingCopyBackendResult Save(const EditorWorkingCopyBackendRequest& request) override;
	[[nodiscard]] EditorWorkingCopyBackendResult SaveAs(const EditorWorkingCopySaveAsBackendRequest& request) override;
	[[nodiscard]] EditorWorkingCopyBackendRevertPrepareResult PrepareRevert(
		const EditorWorkingCopyBackendRequest& request) override;
	[[nodiscard]] EEditorWorkingCopyBackendRevertApplyStatus ApplyPreparedRevert(
		EditorWorkingCopyRevertTransaction transaction) noexcept override;
	void FinalizePreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept override;
	void RollbackPreparedRevert(EditorWorkingCopyRevertTransaction transaction) noexcept override;
	[[nodiscard]] EditorWorkingCopyBackendResult PrepareClose(const EditorWorkingCopyBackendRequest& request) override;
	void CommitClose(const EditorWorkingCopyBackendRequest& request) noexcept override;

private:
	[[nodiscard]] static EditorWorkingCopyBackendResult FromLegacyResult(EDocFileOperationResult result,
		std::uint64_t successfulVersion) noexcept;
	[[nodiscard]] std::optional<EditorDocumentIdentity> CurrentFileIdentity() const;
	[[nodiscard]] static std::optional<ECodeType> ToLegacyEncoding(const std::optional<std::string>& encodingId) noexcept;
	[[nodiscard]] static EEolType ToLegacyLineEnding(EEditorWorkingCopyLineEnding lineEnding) noexcept;
	[[nodiscard]] static bool Utf8ToWideStrict(const std::string& value, std::wstring& converted) noexcept;

	CEditDoc& m_document;
};

} // namespace workbench::editor
