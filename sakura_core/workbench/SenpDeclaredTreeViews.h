/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "workbench/SenpOwnerPublication.h"

namespace workbench {

enum class SenpExtensionActivationState : std::uint8_t;

//! Called on a posted UI gesture, never from layout, paint or runtime Commit.
//! The window maps this request to its activation controller and returns the
//! actual state. The callback must not pump messages or destroy the declaration.
using SenpDeclaredViewActivation = std::function<SenpExtensionActivationState(std::wstring_view, bool)>;

//! Persistent native View bodies for a single extension declaration. A body
//! exists before runtime activation and owns the real activation/error/retry UI.
//! Runtime providers bind through a prepared transaction without replacing the
//! outer body HWND or the separately owned workbench declaration catalog.
class CSenpDeclaredTreeViews final {
public:
	[[nodiscard]] static std::shared_ptr<CSenpDeclaredTreeViews> Create(
		std::wstring extensionId, std::vector<layout::WorkbenchViewDescriptor> views,
		SenpDeclaredViewActivation requestActivation) noexcept;
	~CSenpDeclaredTreeViews();
	CSenpDeclaredTreeViews(const CSenpDeclaredTreeViews&) = delete;
	CSenpDeclaredTreeViews& operator=(const CSenpDeclaredTreeViews&) = delete;
	[[nodiscard]] std::unique_ptr<viewcontainer::ISenpViewBody> CreateBody(
		std::wstring_view viewId, viewcontainer::SenpViewBodyHost host) noexcept;
	[[nodiscard]] std::unique_ptr<ISenpDeclaredTreePublication> PrepareBinding(
		const senp::ContributionOwnerIdentity& owner, std::vector<SenpOwnerBoundTree> trees) noexcept;
	//! The window calls this after activation Poll, including after runtime revoke.
	//! Commit/Close only change authority; this method owns native body replacement.
	[[nodiscard]] bool Pump(SenpExtensionActivationState state) noexcept;
	//! A View title action. It reaches only the currently bound runtime: SENP
	//! activates on `onView:` alone, so a click before activation is refused
	//! rather than starting the extension the way VS Code's `onCommand:` would.
	[[nodiscard]] bool ExecuteTitleCommand(std::string_view viewId, std::string_view commandId) noexcept;
	[[nodiscard]] bool IsUsable() const noexcept;
	void Close() noexcept;
private:
	struct Impl;
	class Body;
	class Publication;
	explicit CSenpDeclaredTreeViews(std::shared_ptr<Impl> impl) noexcept;
	std::shared_ptr<Impl> m_impl;
};

} // namespace workbench
