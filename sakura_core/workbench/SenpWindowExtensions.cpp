/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpWindowExtensions.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/icons/GitHubActionsStatusIcons.h"
#include <algorithm>
#include <map>
#include <set>

namespace workbench {
namespace {
using Status = SenpWindowExtensionsStatus;
class WindowExtensionsCall final {
public:
	explicit WindowExtensionsCall(bool& entered) noexcept : m_entered(entered) { m_entered = true; }
	~WindowExtensionsCall() { m_entered = false; }
private:
	bool& m_entered;
};

std::optional<std::string> Utf8(std::wstring_view value)
{
	if (value.size() > 4096) return {};
	auto text = commands::json::ToUtf8(value);
	const auto roundtrip = commands::json::ToWideStrict(text);
	return roundtrip && *roundtrip == value ? std::optional(std::move(text)) : std::nullopt;
}

//! A title button draws a bundled codicon by name. The manifest's `$(name)`
//! ThemeIcon allows upper case, which names no codicon, so it is unsupported here.
std::optional<std::wstring> TitleActionIcon(std::wstring_view icon)
{
	if (icon.size() < 4 || !icon.starts_with(L"$(") || !icon.ends_with(L')')) return {};
	const auto name = icon.substr(2, icon.size() - 3);
	if (name.size() > 160 || !std::ranges::all_of(name, [](const wchar_t ch) {
		return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-';
	})) return {};
	return std::wstring(name);
}
}

class CSenpWindowExtensions::Entry final {
public:
	explicit Entry(senp::ExtensionDescriptor descriptor) : m_descriptor(std::move(descriptor)) {}
private:
	friend class CSenpWindowExtensions;
	//! A failure carries a short untranslated token so the excluded package's
	//! Extensions row can say what this window could not compose.
	Status Fail(const Status status, const wchar_t* reason)
	{
		m_reason = reason;
		return status;
	}
	Status Prepare()
	{
		const auto id = Utf8(m_descriptor.id);
		if (!id || !layout::WorkbenchContributionRegistry::IsValidStableId(*id)) return Fail(Status::Invalid, L"id");
		m_ownerId = *id;
		if (m_descriptor.runtime.schemaVersion != 2 || !m_descriptor.runtime.compatible)
			return Fail(Status::Unsupported, L"runtime");
		if (m_descriptor.viewContainers.empty() || m_descriptor.viewContainers.size() > 16
			|| m_descriptor.views.empty() || m_descriptor.views.size() > 64
			|| m_descriptor.runtime.commands.size() > 64) return Fail(Status::Unsupported, L"limits");
		std::set<std::string, std::less<>> containers, views, commands;
		for (const auto& value : m_descriptor.viewContainers) {
			const auto containerId = Utf8(value.id), title = Utf8(value.title);
			std::optional<std::string> iconName = std::string{};
			if (!value.icon.empty()) {
				const auto manifestIcon = Utf8(value.icon);
				if (!manifestIcon) return Fail(Status::Invalid, L"icon");
				iconName = layout::WorkbenchContributionRegistry::ContainerIconName(*manifestIcon);
				if (!iconName) return Fail(Status::Unsupported, L"icon");
			}
			if (!containerId || !title || !iconName || !containers.insert(*containerId).second)
				return Fail(Status::Invalid, L"container");
			m_containers.push_back({ *containerId, *title, layout::EViewContainerLocation::Sidebar,
				value.order, *iconName, false,
				{ layout::EViewContainerLocation::Sidebar, layout::EViewContainerLocation::AuxiliaryBar } });
			if (!layout::WorkbenchContributionRegistry::IsValidViewContainerDescriptor(m_containers.back()))
				return Fail(Status::Invalid, L"container");
		}
		for (const auto& value : m_descriptor.views) {
			if (value.provider != L"senp.tree") return Fail(Status::Unsupported, L"provider");
			const auto viewId = Utf8(value.id), containerId = Utf8(value.containerId), title = Utf8(value.title);
			if (!viewId || !containerId || !title || title->empty()
				|| !layout::WorkbenchContributionRegistry::IsValidStableId(*viewId)
				|| !views.insert(*viewId).second) return Fail(Status::Invalid, L"view");
			// Native declaration cohorts currently retain only their own containers.
			if (!containers.contains(*containerId)) return Fail(Status::Unsupported, L"container");
			m_views.push_back({ *viewId, *containerId, *title, value.order, true, true, "senp.tree" });
		}
		for (const auto& value : m_descriptor.runtime.commands) {
			const auto command = Utf8(value.Command());
			if (!command || !layout::WorkbenchContributionRegistry::IsValidStableId(*command)
				|| !commands.insert(*command).second) return Fail(Status::Invalid, L"command");
			m_commands.push_back(*command);
		}
		for (const auto& menu : m_descriptor.runtime.ViewTitle()) {
			const auto declared = std::ranges::find(m_descriptor.runtime.commands, menu.Command(),
				&senp::CommandContribution::Command);
			const auto command = Utf8(menu.Command());
			if (declared == m_descriptor.runtime.commands.end() || !command || !commands.contains(*command))
				return Fail(Status::Invalid, L"menu");
			const auto icon = TitleActionIcon(declared->Icon());
			if (!icon) return Fail(Status::Unsupported, L"icon");
			for (const auto& view : menu.Views()) {
				const auto viewId = Utf8(view);
				if (!viewId || !views.contains(*viewId)) return Fail(Status::Invalid, L"menu");
				auto& actions = m_titleActions[*viewId];
				if (std::ranges::any_of(actions, [&](const auto& action) { return action.CommandId() == *command; }))
					return Fail(Status::Invalid, L"menu");
				// A View title bar has eight native action slots.
				if (actions.size() >= 8) return Fail(Status::Unsupported, L"actions");
				actions.push_back({ *command, declared->Title(), *icon });
			}
		}
		for (const auto& menu : m_descriptor.runtime.ViewItemContext()) {
			const auto declared = std::ranges::find(m_descriptor.runtime.commands, menu.Command(),
				&senp::CommandContribution::Command);
			const auto command = Utf8(menu.Command());
			if (declared == m_descriptor.runtime.commands.end() || !command || !commands.contains(*command))
				return Fail(Status::Invalid, L"menu");
			// A row action draws a bundled codicon or a compiled-in extension icon
			// path; the package's own image files are never read.
			auto icon = TitleActionIcon(declared->Icon());
			if (!icon && !icons::github_actions::FindStatusIcon(declared->Icon()).empty()) icon = declared->Icon();
			if (!icon) return Fail(Status::Unsupported, L"icon");
			std::vector<std::string> targets;
			if (menu.Views().empty()) {
				for (const auto& view : m_views) targets.push_back(view.id);
			}
			for (const auto& view : menu.Views()) {
				const auto viewId = Utf8(view);
				if (!viewId || !views.contains(*viewId)) return Fail(Status::Invalid, L"menu");
				targets.push_back(*viewId);
			}
			for (const auto& viewId : targets) {
				auto& actions = m_itemActions[viewId];
				if (std::ranges::any_of(actions, [&](const auto& action) { return action.CommandId() == menu.Command(); }))
					return Fail(Status::Invalid, L"menu");
				// A row has room for eight inline actions, as a View title bar does.
				if (actions.size() >= 8) return Fail(Status::Unsupported, L"actions");
				actions.push_back({ menu.Command(), declared->Title(), *icon, menu.Contains(), menu.Equals() });
			}
		}
		return Status::Synchronized;
	}
	senp::ExtensionDescriptor m_descriptor;
	std::wstring m_reason;
	std::string m_ownerId;
	std::vector<layout::WorkbenchViewContainerDescriptor> m_containers;
	std::vector<layout::WorkbenchViewDescriptor> m_views;
	std::vector<std::string> m_commands;
	SenpViewTitleActions m_titleActions;
	//! Inline row actions per View. They reach the provider of each runtime
	//! generation and are not a structural declaration field.
	std::map<std::string, std::vector<tree::SenpTreeItemAction>, std::less<>> m_itemActions;
};

CSenpWindowExtensions::CSenpWindowExtensions(layout::WorkbenchContributionRegistry& catalog,
	viewcontainer::CViewContainerPages& pages, std::wstring hostExecutable,
	SenpWindowOwnerTargetFactory createTarget, std::function<bool(std::string_view)> requestFocus,
	senp::EffectRuntimeFactory runtimeFactory, SenpGitHubCliDiagnosticQuery githubCliDiagnostic)
	: m_catalog(catalog), m_createTarget(std::move(createTarget)),
	m_composition(catalog, pages, std::move(runtimeFactory)),
	m_declarations(catalog, pages,
		[this](std::wstring_view id, bool retry) { return RequestView(id, retry, std::chrono::steady_clock::now()); },
		std::move(requestFocus), std::move(githubCliDiagnostic)),
	m_activation(m_composition, std::move(hostExecutable),
		[this](const auto& descriptor, const auto& owner) { return PreparePublication(descriptor, owner); }) {}

CSenpWindowExtensions::~CSenpWindowExtensions() { (void)Close(); }

std::optional<SenpOwnerPublicationOptions> CSenpWindowExtensions::PreparePublication(
	const senp::ExtensionDescriptor& descriptor, const senp::ContributionOwnerIdentity& owner)
{
	if (m_closed || !m_createTarget) return {};
	Entry entry(descriptor);
	if (entry.Prepare() != Status::Synchronized) return {};
	auto target = m_createTarget(descriptor, owner);
	if (!target) return {};
	std::vector<SenpOwnerTreeContribution> trees;
	for (auto& view : entry.m_views) {
		auto actions = entry.m_itemActions.find(view.id);
		auto itemActions = actions == entry.m_itemActions.end()
			? std::vector<tree::SenpTreeItemAction>{} : std::move(actions->second);
		trees.emplace_back(std::move(view), entry.m_commands, std::move(itemActions));
	}
	return SenpOwnerPublicationOptions{ std::move(trees), std::move(target),
		[this](const auto& candidate, auto bindings) { return m_declarations.Bind(candidate, std::move(bindings)); } };
}

SenpWindowExtensionsStatus CSenpWindowExtensions::Synchronize(const senp::ManagementSnapshot& snapshot,
	std::int64_t workspaceRevision, std::int64_t accountGeneration, senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed) return Status::Stopped;
	if (m_entered) return Status::Conflict;
	if (workspaceRevision < 0 || accountGeneration < 0 || (m_synchronized && snapshot.revision < m_revision)) return Status::Invalid;
	if (snapshot.state != senp::EManagementState::Ready && snapshot.state != senp::EManagementState::ReadyWithDiagnostics) {
		(void)Close(); return Status::Failed;
	}
	WindowExtensionsCall call(m_entered);
	if (m_synchronized && snapshot.revision == m_revision
		&& workspaceRevision == m_workspaceRevision && accountGeneration == m_accountGeneration) return Status::Synchronized;
	try {
		// One bad package excludes only itself, with a recorded reason; it is
		// never registered, activated or granted, and the rest of the snapshot
		// still composes (#352). A structural update can preserve the old
		// package only while it remains enabled in the same scope. Never let a
		// declaration conflict suppress a disable, uninstall, workspace or
		// account revocation.
		const bool scopeUnchanged = m_synchronized && workspaceRevision == m_workspaceRevision
			&& accountGeneration == m_accountGeneration;
		decltype(m_entries) desired;
		decltype(m_exclusions) exclusions;
		// ViewContainer and View ids are global to the catalog: a package that
		// claims an id a live cohort already holds would conflict at
		// registration, so it is excluded before it gets that far.
		std::set<std::string, std::less<>> cohortContainers, cohortViews;
		for (const auto& [id, entry] : m_entries) {
			for (const auto& container : entry->m_containers) cohortContainers.insert(container.id);
			for (const auto& view : entry->m_views) cohortViews.insert(view.id);
		}
		const auto composable = [](const senp::ExtensionDescriptor& descriptor) {
			return descriptor.installed && descriptor.enabled && descriptor.runtime.schemaVersion == 2;
		};
		// An ID that several enabled packages claim is ambiguous, so no claimant
		// composes: neither the row status nor the activation/authority path may
		// silently pick one of them by snapshot order.
		std::map<std::wstring_view, std::size_t, std::less<>> claims;
		for (const auto& descriptor : snapshot.extensions) if (composable(descriptor)) ++claims[descriptor.id];
		for (const auto& descriptor : snapshot.extensions) {
			if (!composable(descriptor)) continue;
			if (claims.find(descriptor.id)->second > 1) {
				exclusions[descriptor.id] = { SenpWindowExtensionExclusionKind::Invalid, L"duplicate" };
				continue;
			}
			if (desired.size() >= 64) {
				exclusions[descriptor.id] = { SenpWindowExtensionExclusionKind::Unsupported, L"limit" };
				continue;
			}
			auto entry = std::make_unique<Entry>(descriptor);
			const auto status = entry->Prepare();
			if (status != Status::Synchronized) {
				exclusions[descriptor.id] = {
					!descriptor.runtime.compatible ? SenpWindowExtensionExclusionKind::Incompatible
						: status == Status::Invalid ? SenpWindowExtensionExclusionKind::Invalid
						: SenpWindowExtensionExclusionKind::Unsupported,
					std::move(entry->m_reason) };
				continue;
			}
			const auto previous = m_entries.find(descriptor.id);
			if (previous != m_entries.end() && (previous->second->m_containers != entry->m_containers
				|| previous->second->m_views != entry->m_views
				|| previous->second->m_titleActions != entry->m_titleActions)) {
				exclusions[descriptor.id] = { SenpWindowExtensionExclusionKind::Conflict, L"structure" };
				if (scopeUnchanged) desired.insert(m_entries.extract(previous));
				continue;
			}
			// The seed sets hold this package's own live ids too; only an id a
			// different package owns is a conflict.
			const auto containerTaken = [&](const std::string& id) {
				if (!cohortContainers.contains(id)) return false;
				return previous == m_entries.end() || std::ranges::none_of(previous->second->m_containers,
					[&](const auto& container) { return container.id == id; });
			};
			const auto viewTaken = [&](const std::string& id) {
				if (!cohortViews.contains(id)) return false;
				return previous == m_entries.end() || std::ranges::none_of(previous->second->m_views,
					[&](const auto& view) { return view.id == id; });
			};
			if (std::ranges::any_of(entry->m_containers, [&](const auto& c) { return containerTaken(c.id); })
				|| std::ranges::any_of(entry->m_views, [&](const auto& v) { return viewTaken(v.id); })) {
				exclusions[descriptor.id] = { SenpWindowExtensionExclusionKind::Conflict, L"registration" };
				continue;
			}
			for (const auto& container : entry->m_containers) cohortContainers.insert(container.id);
			for (const auto& view : entry->m_views) cohortViews.insert(view.id);
			desired.emplace(descriptor.id, std::move(entry));
		}
		// Allocate rollback storage first. Registration may publish a new hidden
		// cohort, but no activation/layout runs until the whole batch is ready.
		std::vector<std::wstring_view> added;
		added.reserve(desired.size());
		std::vector<std::wstring> refused;
		for (const auto& [id, entry] : desired) {
			const auto status = m_declarations.Register({ entry->m_ownerId, m_catalog.NextOwnerGeneration() },
				entry->m_containers, entry->m_views, entry->m_titleActions);
			if (status == SenpViewDeclarationStatus::Registered) added.push_back(id);
			else if (status == SenpViewDeclarationStatus::Unchanged) continue;
			else if (status == SenpViewDeclarationStatus::Conflict
				|| status == SenpViewDeclarationStatus::Invalid
				|| status == SenpViewDeclarationStatus::Failed) {
				// A refused registration is only this package's exclusion. Register
				// commits nothing when it refuses; a live cohort of the same package
				// leaves below, after activation has revoked its runtime.
				exclusions[id] = { status == SenpViewDeclarationStatus::Conflict
					? SenpWindowExtensionExclusionKind::Conflict
					: status == SenpViewDeclarationStatus::Invalid
						? SenpWindowExtensionExclusionKind::Invalid
						: SenpWindowExtensionExclusionKind::Unsupported,
					L"registration" };
				refused.push_back(id);
			} else {
				for (const auto rollback : added) (void)m_declarations.Remove(rollback);
				(void)CloseEntered(); return Status::Failed;
			}
		}
		for (const auto& id : refused) desired.erase(id);
		senp::ManagementSnapshot accepted{ snapshot.state, snapshot.revision };
		accepted.extensions.reserve(desired.size());
		for (const auto& [id, entry] : desired) accepted.extensions.push_back(entry->m_descriptor);
		// Synchronize first revokes removed/disabled runtime authority and signals
		// process Stop. Its retained owner slots continue to own physical cleanup.
		if (!m_activation.Synchronize(accepted, workspaceRevision, accountGeneration, now)) {
			(void)CloseEntered();
			return Status::Failed;
		}
		for (const auto& [id, entry] : m_entries) if (!desired.contains(id)) (void)m_declarations.Remove(id);
		// A refused package retained for a structural conflict was moved out of
		// m_entries above, so it needs its own removal.
		for (const auto& id : refused) (void)m_declarations.Remove(id);
		m_entries = std::move(desired);
		m_exclusions = std::move(exclusions);
		m_revision = snapshot.revision; m_workspaceRevision = workspaceRevision; m_accountGeneration = accountGeneration;
		m_synchronized = true;
		return Status::Synchronized;
	} catch (const std::exception&) {
		// m_declarations.Register/Remove and m_activation.Synchronize are all
		// noexcept; CSenpExtensionActivation's own Activate boundary already
		// swallows anything m_createTarget/PreparePublication could throw. Only
		// std container/allocation work in this block can throw here.
		(void)CloseEntered();
		return Status::Failed;
	}
}

SenpExtensionActivationState CSenpWindowExtensions::RequestView(std::wstring_view viewId,
	bool explicitRetry, senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed) return SenpExtensionActivationState::Stopped;
	if (m_entered) return SenpExtensionActivationState::Busy;
	WindowExtensionsCall call(m_entered);
	(void)m_activation.RequestView(viewId, now, explicitRetry);
	for (const auto& [id, entry] : m_entries) {
		if (std::ranges::any_of(entry->m_descriptor.views, [&](const auto& view) { return view.id == viewId; }))
			return m_activation.State(id).value_or(SenpExtensionActivationState::Unsupported);
	}
	return SenpExtensionActivationState::Unsupported;
}

bool CSenpWindowExtensions::PublishWorkspace(const senp::effect::WorkspaceChanged& workspace) noexcept
{
	if (m_closed || m_entered) return false;
	WindowExtensionsCall call(m_entered);
	return m_composition.PublishWorkspace(workspace);
}

bool CSenpWindowExtensions::Poll(senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed || m_entered) return false;
	WindowExtensionsCall call(m_entered);
	bool result = m_activation.Poll(now);
	for (const auto& [id, entry] : m_entries)
		result = m_declarations.Pump(id, m_activation.State(id).value_or(SenpExtensionActivationState::Unsupported)) && result;
	if (!result) (void)CloseEntered();
	return result;
}

bool CSenpWindowExtensions::ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept
{
	if (m_closed || m_entered) return false;
	WindowExtensionsCall call(m_entered);
	const bool result = m_declarations.ApplyLayout(snapshot);
	if (!result) (void)CloseEntered();
	return result;
}

bool CSenpWindowExtensions::FocusView(std::string_view viewId) noexcept
{
	if (m_closed || m_entered) return false;
	WindowExtensionsCall call(m_entered);
	return m_declarations.FocusView(viewId);
}

void CSenpWindowExtensions::RefreshStrings() noexcept
{
	if (m_closed || m_entered) return;
	WindowExtensionsCall call(m_entered);
	m_declarations.RefreshStrings();
}

std::optional<SenpExtensionActivationState> CSenpWindowExtensions::State(std::wstring_view extensionId) const noexcept
{
	return m_closed ? std::optional(SenpExtensionActivationState::Stopped) : m_activation.State(extensionId);
}

std::optional<SenpWindowExtensionExclusion> CSenpWindowExtensions::Exclusion(
	std::wstring_view extensionId) const noexcept
{
	const auto found = m_exclusions.find(extensionId);
	return found == m_exclusions.end() ? std::nullopt : std::optional(found->second);
}

bool CSenpWindowExtensions::Close() noexcept
{
	if (m_entered) return false;
	WindowExtensionsCall call(m_entered);
	return CloseEntered();
}

bool CSenpWindowExtensions::CloseEntered() noexcept
{
	m_closed = true;
	const bool stopped = m_activation.Close();
	m_declarations.Close(); m_entries.clear(); m_exclusions.clear(); m_createTarget = {};
	return stopped;
}

} // namespace workbench
