/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpExtensionActivation.h"
#include "workbench/SenpViewDeclarations.h"

namespace workbench {

enum class SenpWindowExtensionsStatus : std::uint8_t {
	Synchronized, Invalid, Unsupported, Conflict, Failed, Stopped,
};

using SenpWindowOwnerTargetFactory = std::function<std::unique_ptr<ISenpOwnerProjectionTarget>(
	const senp::ExtensionDescriptor&, const senp::ContributionOwnerIdentity&)>;

//! Window-local package, declaration and runtime composition. The window owns
//! scheduling and layout reconciliation; this object owns no timers or I/O.
//! Injected native callbacks must not pump messages or destroy this object.
class CSenpWindowExtensions final {
public:
	CSenpWindowExtensions(layout::WorkbenchContributionRegistry& catalog,
		viewcontainer::CViewContainerPages& pages,
		std::wstring hostExecutable, SenpWindowOwnerTargetFactory createTarget,
		std::function<bool(std::string_view)> requestFocus,
		senp::EffectRuntimeFactory runtimeFactory);
	~CSenpWindowExtensions();
	CSenpWindowExtensions(const CSenpWindowExtensions&) = delete;
	CSenpWindowExtensions& operator=(const CSenpWindowExtensions&) = delete;

	[[nodiscard]] SenpWindowExtensionsStatus Synchronize(const senp::ManagementSnapshot& snapshot,
		std::int64_t workspaceRevision, std::int64_t accountGeneration,
		senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] SenpExtensionActivationState RequestView(std::wstring_view viewId,
		bool explicitRetry, senp::CSenpRuntimeSession::Time now) noexcept;
	//! The window's current repositories, as the packages are allowed to see
	//! them. Retained by the composition, so a package that activates later is
	//! told the same thing rather than starting blind.
	[[nodiscard]] bool PublishWorkspace(const senp::effect::WorkspaceChanged& workspace) noexcept;
	[[nodiscard]] bool Poll(senp::CSenpRuntimeSession::Time now) noexcept;
	[[nodiscard]] bool ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept;
	[[nodiscard]] bool FocusView(std::string_view viewId) noexcept;
	void RefreshStrings() noexcept;
	[[nodiscard]] std::optional<SenpExtensionActivationState> State(std::wstring_view extensionId) const noexcept;
	//! Admission and native callbacks end immediately. False retains failed
	//! runtime cleanup; explicit repeated Close owns the next join attempt.
	[[nodiscard]] bool Close() noexcept;
private:
	class Entry;
	[[nodiscard]] bool CloseEntered() noexcept;
	[[nodiscard]] std::optional<SenpOwnerPublicationOptions> PreparePublication(
		const senp::ExtensionDescriptor& descriptor, const senp::ContributionOwnerIdentity& owner);
	layout::WorkbenchContributionRegistry& m_catalog;
	SenpWindowOwnerTargetFactory m_createTarget;
	CSenpOwnerComposition m_composition;
	CSenpViewDeclarations m_declarations;
	CSenpExtensionActivation m_activation;
	std::map<std::wstring, std::unique_ptr<Entry>, std::less<>> m_entries;
	std::uint64_t m_revision{};
	std::int64_t m_workspaceRevision{}, m_accountGeneration{};
	bool m_synchronized{}, m_entered{}, m_closed{};
};

} // namespace workbench
