/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "senp/SenpContributionOwners.h"
#include "workbench/SenpOwnerProjection.h"
#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/viewcontainer/SenpViewContainer.h"

#include <functional>
#include <optional>

namespace workbench {

class SenpOwnerTreeContribution final {
public:
	SenpOwnerTreeContribution(layout::WorkbenchViewDescriptor descriptor,
		std::vector<std::string> commands) noexcept
		: m_descriptor(std::move(descriptor)), m_commands(std::move(commands)) {}
	[[nodiscard]] const layout::WorkbenchViewDescriptor& Descriptor() const noexcept { return m_descriptor; }
	[[nodiscard]] std::vector<std::string> TakeCommands() noexcept { return std::move(m_commands); }
private:
	layout::WorkbenchViewDescriptor m_descriptor;
	std::vector<std::string> m_commands;
};

using SenpTreeBodyFactory = std::function<std::unique_ptr<viewcontainer::ISenpViewBody>(
	viewcontainer::SenpViewBodyHost, std::shared_ptr<tree::SenpTreeProvider>, std::wstring)>;

//! A runtime binding is distinct from the persistent declarative View body.
class SenpOwnerBoundTree final {
public:
	SenpOwnerBoundTree(std::wstring viewId, std::shared_ptr<tree::SenpTreeProvider> provider) noexcept
		: m_viewId(std::move(viewId)), m_provider(std::move(provider)) {}
	[[nodiscard]] std::wstring_view ViewId() const noexcept { return m_viewId; }
	[[nodiscard]] const std::shared_ptr<tree::SenpTreeProvider>& Provider() const noexcept { return m_provider; }
private:
	std::wstring m_viewId;
	std::shared_ptr<tree::SenpTreeProvider> m_provider;
};

//! Prepared binding into already-published native declarations. Preparation
//! owns allocation; Commit changes binding authority without runtime submissions
//! or message pumping. Pump projects the committed binding after owner Poll.
//! Close revokes only its exact runtime generation and retains declaration UI.
class ISenpDeclaredTreePublication {
public:
	virtual ~ISenpDeclaredTreePublication() = default;
	[[nodiscard]] virtual bool CanCommit() const noexcept = 0;
	[[nodiscard]] virtual bool Commit() noexcept = 0;
	[[nodiscard]] virtual bool Pump() noexcept = 0;
	virtual void Close() noexcept = 0;
};
using SenpDeclaredTreeFactory = std::function<std::unique_ptr<ISenpDeclaredTreePublication>(
	const senp::ContributionOwnerIdentity&, std::vector<SenpOwnerBoundTree>)>;

class SenpOwnerPublicationOptions final {
public:
	//! Binds a runtime into declarations owned independently by the window.
	SenpOwnerPublicationOptions(std::vector<SenpOwnerTreeContribution> trees,
		std::unique_ptr<ISenpOwnerProjectionTarget> target,
		SenpDeclaredTreeFactory bindDeclaredTrees) noexcept
		: m_trees(std::move(trees)), m_target(std::move(target)),
		m_bindDeclaredTrees(std::move(bindDeclaredTrees)) {}
	SenpOwnerPublicationOptions(HWND parkingParent,
		std::vector<layout::WorkbenchViewContainerDescriptor> containers,
		std::vector<SenpOwnerTreeContribution> trees,
		std::unique_ptr<ISenpOwnerProjectionTarget> target,
		std::function<bool(std::string_view)> requestFocus,
		SenpTreeBodyFactory createTreeBody = {}) noexcept
		: m_parkingParent(parkingParent), m_containers(std::move(containers)),
		m_trees(std::move(trees)), m_target(std::move(target)),
		m_requestFocus(std::move(requestFocus)), m_createTreeBody(std::move(createTreeBody)) {}
	[[nodiscard]] const SenpDeclaredTreeFactory& DeclaredTreeFactory() const noexcept { return m_bindDeclaredTrees; }
	[[nodiscard]] HWND ParkingParent() const noexcept { return m_parkingParent; }
	[[nodiscard]] const std::vector<layout::WorkbenchViewContainerDescriptor>& Containers() const noexcept { return m_containers; }
	[[nodiscard]] std::vector<SenpOwnerTreeContribution> TakeTrees() noexcept { return std::move(m_trees); }
	[[nodiscard]] std::unique_ptr<ISenpOwnerProjectionTarget> TakeTarget() noexcept { return std::move(m_target); }
	[[nodiscard]] std::function<bool(std::string_view)> TakeRequestFocus() noexcept { return std::move(m_requestFocus); }
	[[nodiscard]] const SenpTreeBodyFactory& TreeBodyFactory() const noexcept { return m_createTreeBody; }
private:
	HWND m_parkingParent{};
	std::vector<layout::WorkbenchViewContainerDescriptor> m_containers;
	std::vector<SenpOwnerTreeContribution> m_trees;
	std::unique_ptr<ISenpOwnerProjectionTarget> m_target;
	std::function<bool(std::string_view)> m_requestFocus;
	SenpTreeBodyFactory m_createTreeBody;
	SenpDeclaredTreeFactory m_bindDeclaredTrees;
};

//! UI-thread registry for owner publications retained by CSenpContributionOwners.
//! Apply only queues work while the owner service is entered; Pump runs after
//! owner Poll and is the sole place that may submit follow-up tree/document work.
class CSenpOwnerPublicationHub final {
public:
	CSenpOwnerPublicationHub(senp::CSenpContributionOwners& owners,
		layout::WorkbenchContributionRegistry& contributions,
		viewcontainer::CViewContainerPages& pages) noexcept;
	~CSenpOwnerPublicationHub();
	CSenpOwnerPublicationHub(const CSenpOwnerPublicationHub&) = delete;
	CSenpOwnerPublicationHub& operator=(const CSenpOwnerPublicationHub&) = delete;

	[[nodiscard]] std::unique_ptr<senp::ISenpOwnerPublication> Prepare(
		const senp::ContributionOwnerIdentity& candidate,
		const senp::ContributionOwnerIdentity* previous,
		SenpOwnerPublicationOptions options) noexcept;
	//! Hands one workspace snapshot to every publication that has committed, and
	//! retains it for the ones that have not: a package activated later would
	//! otherwise start with no repositories and stay that way until the workspace
	//! next changed. Nothing is submitted here - Pump owns submission - so this is
	//! safe to call while the owner service is between turns. False means the
	//! payload is not one the wire accepts, or that a committed publication
	//! refused it.
	[[nodiscard]] bool PublishWorkspace(const senp::effect::WorkspaceChanged& workspace) noexcept;
	//! Must run after CSenpContributionOwners::Poll on the same UI thread.
	[[nodiscard]] bool Pump(senp::CSenpRuntimeSession::Time now) noexcept;
	void Close() noexcept;

private:
	class State;
	class Publication;
	senp::CSenpContributionOwners& m_owners;
	layout::WorkbenchContributionRegistry& m_contributions;
	viewcontainer::CViewContainerPages& m_pages;
	std::vector<std::weak_ptr<State>> m_publications;
	//! The last workspace the window published, replayed into each publication as
	//! it commits. It is state rather than a one-shot notification: the wire event
	//! is a complete list, so the newest one is the whole truth.
	std::optional<senp::effect::WorkspaceChanged> m_workspace;
	bool m_closed{};
};

} // namespace workbench
