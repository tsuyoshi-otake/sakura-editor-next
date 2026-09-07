/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/editor/EditorCoreService.h"

#include <memory>

namespace workbench::editor {

//! Composition supplies this exact authorized scope; labels never authorize an owner.
struct SenpReadonlyScope final {
	std::string extensionId;
	std::int64_t ownerGeneration{}, workspaceRevision{}, accountGeneration{};
	bool operator==(const SenpReadonlyScope&) const = default;
};
struct SenpReadonlyInput final {
	std::string inputId;
	SenpReadonlyScope scope;
	std::wstring resourceId;
	std::wstring title;
};
enum class SenpReadonlyStatus : std::uint8_t {
	Succeeded, Reused, NotFound, Invalid, LimitReached, Conflict, Closed, Failed
};
struct SenpReadonlyOpenResult final {
	SenpReadonlyStatus status{ SenpReadonlyStatus::Failed };
	std::string inputId;
};
enum class SenpEditorCommandRoute : std::uint8_t {
	Legacy, Workbench, ReadonlySurface, CloseReadonly, NotApplicable, NoActiveInput
};

//! One UI-thread owner of readonly registrations within the existing EditorCoreService.
//! This is not a second group or active-input model. It never touches a working-copy
//! backend. Opens are inactive until a prepared native surface is bound and selected.
//! Input contents and view state belong to their separately retained native surfaces.
class SenpReadonlyWorkbench final {
public:
	static constexpr std::size_t kMaximumInputs = 16;
	explicit SenpReadonlyWorkbench(EditorCoreService& core);
	~SenpReadonlyWorkbench();
	SenpReadonlyWorkbench(const SenpReadonlyWorkbench&) = delete;
	SenpReadonlyWorkbench& operator=(const SenpReadonlyWorkbench&) = delete;
	[[nodiscard]] SenpReadonlyOpenResult Open(SenpReadonlyScope scope,
		std::wstring resourceId, std::wstring title);
	[[nodiscard]] SenpReadonlyStatus Show(std::string_view inputId);
	[[nodiscard]] SenpReadonlyStatus Close(std::string_view inputId);
	//! Revoke only the exact cohort. A failed close retains ownership for an explicit retry.
	[[nodiscard]] SenpReadonlyStatus Revoke(const SenpReadonlyScope& scope);
	[[nodiscard]] SenpReadonlyStatus Shutdown();
	[[nodiscard]] const SenpReadonlyInput* Find(std::string_view inputId) const noexcept;
	[[nodiscard]] std::vector<SenpReadonlyInput> Inputs() const;
	[[nodiscard]] EditorCoreSnapshot Snapshot() const;
	//! Unknown editor commands fail closed while a readonly input is active.
	[[nodiscard]] SenpEditorCommandRoute Route(std::string_view commandId) const;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::editor
