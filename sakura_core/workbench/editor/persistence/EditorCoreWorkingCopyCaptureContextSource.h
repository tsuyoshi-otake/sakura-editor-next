/*! @file
 * @brief Editor Core snapshot source for native working-copy capture metadata.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"

namespace workbench::editor::persistence {

/*! @brief Projects one coherent active Editor Core snapshot into native capture metadata.
 *
 * This source owns no editor state and does not read native document, UI, IPC, or
 * filesystem state. A missing active text input, a detached document, or a zero
 * document revision produces no context instead of an approximate identity.
 */
class EditorCoreWorkingCopyCaptureContextSource final
	: public ICEditDocWorkingCopyCaptureContextSource
	, public IEditorWorkingCopyCurrentChangeSource {
public:
	explicit EditorCoreWorkingCopyCaptureContextSource(const EditorCoreService& editorCore) noexcept;

	[[nodiscard]] std::optional<CEditDocWorkingCopyCaptureContext> CurrentCaptureContext() const override;
	[[nodiscard]] std::optional<EditorWorkingCopyCurrentChange> CurrentChange() const override;

private:
	const EditorCoreService& m_editorCore;
};

} // namespace workbench::editor::persistence
