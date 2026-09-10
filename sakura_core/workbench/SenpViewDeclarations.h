/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpDeclaredTreeViews.h"
#include <map>

namespace workbench {

enum class SenpViewDeclarationStatus : std::uint8_t {
	Registered, Unchanged, Invalid, Conflict, Failed, Stopped,
};

//! A declaration's `view/title` actions, keyed by View ID.
using SenpViewTitleActions = std::map<std::string, std::vector<viewcontainer::SenpViewTitleAction>, std::less<>>;

//! UI-thread owner of persistent native declaration cohorts and their catalog
//! and page registrations. Runtime generations borrow Bind; they never own this
//! catalog. The window stops activation/runtime ownership before Remove/Close.
class CSenpViewDeclarations final {
public:
	CSenpViewDeclarations(layout::WorkbenchContributionRegistry& catalog,
		viewcontainer::CViewContainerPages& pages, HWND parkingParent,
		SenpDeclaredViewActivation requestActivation,
		std::function<bool(std::string_view)> requestFocus);
	~CSenpViewDeclarations();
	CSenpViewDeclarations(const CSenpViewDeclarations&) = delete;
	CSenpViewDeclarations& operator=(const CSenpViewDeclarations&) = delete;

	//! Prepare all native bodies and both revision-fenced catalog/page candidates
	//! before publication. An unchanged declaration retains its native lifetime;
	//! structural replacement returns Conflict and preserves the current cohort.
	[[nodiscard]] SenpViewDeclarationStatus Register(
		layout::WorkbenchContributionOwner owner,
		std::vector<layout::WorkbenchViewContainerDescriptor> containers,
		std::vector<layout::WorkbenchViewDescriptor> views,
		SenpViewTitleActions titleActions = {}) noexcept;
	[[nodiscard]] std::unique_ptr<ISenpDeclaredTreePublication> Bind(
		const senp::ContributionOwnerIdentity& runtimeOwner,
		std::vector<SenpOwnerBoundTree> trees) noexcept;
	//! Called after activation Poll, including when there is no active runtime.
	[[nodiscard]] bool Pump(std::wstring_view extensionId,
		SenpExtensionActivationState state) noexcept;
	[[nodiscard]] bool ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept;
	[[nodiscard]] bool FocusView(std::string_view viewId) noexcept;
	[[nodiscard]] bool Remove(std::wstring_view extensionId) noexcept;
	void Close() noexcept;
private:
	class Entry;
	layout::WorkbenchContributionRegistry& m_catalog;
	viewcontainer::CViewContainerPages& m_pages;
	HWND m_parkingParent{};
	SenpDeclaredViewActivation m_requestActivation;
	std::function<bool(std::string_view)> m_requestFocus;
	std::map<std::wstring, std::unique_ptr<Entry>, std::less<>> m_entries;
	bool m_entered{}, m_closed{};
};

} // namespace workbench
