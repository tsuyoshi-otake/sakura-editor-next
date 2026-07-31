/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/ILegacyEditorBackend.h"

#include <memory>
#include <string>

namespace workbench::editor {

/*!
	@brief Strangler boundary used while CEditDoc/CEditWnd are replaced by workbench services.

	EditorCoreService remains authoritative. The adapter never infers that an input
	is open merely because the legacy backing document exists, and it preserves the
	core operation result without adding a second replay or revision model.
*/
class CEditorServiceLegacyAdapter final {
public:
	CEditorServiceLegacyAdapter(EditorCoreService& core, ILegacyEditorBackend& legacy) noexcept;

	[[nodiscard]] EditorCoreSnapshot Snapshot() const;
	//! Reads one explicitly prepared legacy candidate and transactionally makes it resolved, visible, and active.
	[[nodiscard]] EditorOperationResult AdoptCurrentDocument(EditorOperationMetadata operation, std::string inputId);
	/*! 
		@brief Atomically retargets an existing visible input to the explicitly prepared legacy document.

		This is the legacy bridge for a completed native replacement (for example Save As).
		It deliberately does not open or select another input: EditorCoreService preserves the
		existing input's group position and active selection in its one replacement commit.
	*/
	[[nodiscard]] EditorOperationResult ReplaceInputDocumentWithCurrent(EditorOperationMetadata operation, std::string inputId);
	[[nodiscard]] EditorOperationResult ResolveCurrentDocument(const EditorOperationMetadata& operation);
	[[nodiscard]] EditorOperationResult ReleaseDocument(const ReleaseDocumentRequest& request);
	[[nodiscard]] EditorOperationResult OpenResolvedInput(const OpenResolvedInputRequest& request);
	[[nodiscard]] EditorOperationResult ShowInput(const ShowInputRequest& request);
	[[nodiscard]] EditorOperationResult SetDocumentState(const SetDocumentStateRequest& request);
	[[nodiscard]] EditorOperationResult CloseInput(const CloseInputRequest& request);
	[[nodiscard]] std::unique_ptr<IEditorCoreSubscription> Subscribe(EditorCoreChangeCallback callback);

private:
	EditorCoreService& m_core;
	ILegacyEditorBackend& m_legacy;
};

} // namespace workbench::editor
