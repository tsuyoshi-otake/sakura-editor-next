/*! @file
 * @brief Native CEditDoc snapshot and non-destructive Hot Exit recovery adapter.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/persistence/EditorWorkingCopyLifecycleTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class CEditDoc;

namespace workbench::editor::persistence {

/*!
	@brief Core-owned metadata needed to identify the single native CEditDoc.

	CEditDoc does not own a workbench input ID, exact Editor Core identity, or a
	monotonic document revision. The composition root supplies those values
	through the narrow, presentation-neutral source below.
*/
struct CEditDocWorkingCopyCaptureContext final {
	std::string inputId;
	std::string inputTypeId;
	//! Exact identity from one EditorCoreService snapshot; never infer this from CEditDoc state.
	EditorDocumentIdentity documentIdentity;
	std::uint64_t documentRevision = 0;
};

class ICEditDocWorkingCopyCaptureContextSource {
public:
	virtual ~ICEditDocWorkingCopyCaptureContextSource() = default;
	[[nodiscard]] virtual std::optional<CEditDocWorkingCopyCaptureContext> CurrentCaptureContext() const = 0;
};

/*!
	@brief UI-independent bridge between Hot Exit contracts and one CEditDoc.

	Capture walks the native line model directly; it does not consult CEditWnd,
	tabs, extension snapshots, or the filesystem.  Prepare strictly validates
	and decodes a complete backup into an off-to-the-side CDocLineMgr that uses
	the target document's allocator.  Commit is a guarded, no-throw ownership
	transfer and only accepts the exact request prepared for an inert target.

	The V1 DTO can losslessly represent UTF-8 (with or without BOM), BOM-bearing
	UTF-16 LE/BE, and Windows-1252. Other native encodings, including Shift-JIS
	and BOM-less UTF-16, complete as Unsupported instead of changing metadata.
*/
class CEditDocWorkingCopyPersistenceAdapter final
	: public IEditorWorkingCopySnapshotSource
	, public IEditorWorkingCopyRecoveryApplier {
public:
	CEditDocWorkingCopyPersistenceAdapter(
		CEditDoc& document, const ICEditDocWorkingCopyCaptureContextSource& contextSource) noexcept;
	~CEditDocWorkingCopyPersistenceAdapter() override;

	CEditDocWorkingCopyPersistenceAdapter(const CEditDocWorkingCopyPersistenceAdapter&) = delete;
	CEditDocWorkingCopyPersistenceAdapter& operator=(const CEditDocWorkingCopyPersistenceAdapter&) = delete;

	[[nodiscard]] EditorWorkingCopySnapshotResult Capture() override;
	[[nodiscard]] EditorWorkingCopyRecoveryPrepareResult Prepare(
		const EditorWorkingCopyRecoveryRequest& request) override;
	void AbortPrepared() noexcept override;
	[[nodiscard]] EEditorWorkingCopyRecoveryCommitStatus Commit(
		const EditorWorkingCopyRecoveryRequest& request) noexcept override;

private:
	struct PreparedDocument;

	[[nodiscard]] bool IsInertTarget() const noexcept;

	CEditDoc& m_document;
	const ICEditDocWorkingCopyCaptureContextSource& m_contextSource;
	std::unique_ptr<PreparedDocument> m_prepared;
};

} // namespace workbench::editor::persistence
