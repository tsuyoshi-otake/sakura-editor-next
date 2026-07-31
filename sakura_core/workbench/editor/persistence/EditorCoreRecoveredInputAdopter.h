/*! @file
 * @brief Inactive recovered-input adapter for the one-group Editor Core V1 bridge.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/EditorCoreService.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleTypes.h"

#include <cstdint>
#include <string>

namespace workbench::editor::persistence {

/*! 
	@brief Adopts a recovered session descriptor into Editor Core without selecting it.

	V1 has one logical Editor Core group. This adapter always targets that primary
	group and never performs a show, activation, or focus operation. Editor Core does
	not model inputTypeId; the adapter preserves that type-specific descriptor at the
	persistence boundary by validating it before mapping its inputId and the exact
	supplied EditorDocumentIdentity into the core request.
*/
class EditorCoreRecoveredInputAdopter final : public IEditorWorkingCopyRecoveredInputAdopter {
public:
	explicit EditorCoreRecoveredInputAdopter(EditorCoreService& core) noexcept;

	[[nodiscard]] bool AdoptInactive(const EditorSessionInputDescriptor& input,
		const EditorDocumentIdentity& identity, std::uint64_t contentVersion) override;
	[[nodiscard]] bool RollbackInactive(const EditorSessionInputDescriptor& input,
		const EditorDocumentIdentity& identity, std::uint64_t contentVersion) noexcept override;

private:
	//! Returns a bounded opaque ID with no resource, identity, or content diagnostic data.
	[[nodiscard]] bool TryNextOperationId(std::string& operationId) noexcept;

	EditorCoreService& m_core;
};

} // namespace workbench::editor::persistence
