/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpViewDeclarations.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include <algorithm>
#include <utility>

namespace workbench {
namespace {
using Status = SenpViewDeclarationStatus;
using CatalogStatus = layout::EWorkbenchContributionChangeStatus;
class DeclarationCall final {
public:
	explicit DeclarationCall(bool& entered) noexcept : m_entered(entered) { m_entered = true; }
	~DeclarationCall() { m_entered = false; }
private:
	bool& m_entered;
};
}

class CSenpViewDeclarations::Entry final {
public:
	Entry(CSenpViewDeclarations& declarations, layout::WorkbenchContributionOwner owner,
		std::vector<layout::WorkbenchViewContainerDescriptor> containers,
		std::vector<layout::WorkbenchViewDescriptor> views) noexcept
		: m_declarations(declarations), m_owner(std::move(owner)),
		m_containers(std::move(containers)), m_views(std::move(views)) {}
	~Entry() { Close(); }
private:
	friend class CSenpViewDeclarations;
	bool Prepare(const std::wstring& extensionId)
	{
		m_bodies = CSenpDeclaredTreeViews::Create(extensionId, m_views, m_declarations.m_requestActivation);
		if (!m_bodies) return false;
		std::vector<viewcontainer::SenpNativeViewDefinition> native;
		native.reserve(m_views.size()); m_containerIds.reserve(m_containers.size());
		for (const auto& view : m_views) {
			const auto id = commands::json::ToWideStrict(view.id);
			if (!id) return false;
			native.push_back({ view, [bodies = m_bodies, id = *id](viewcontainer::SenpViewBodyHost host) {
				return bodies->CreateBody(id, std::move(host));
			} });
		}
		for (const auto& container : m_containers) m_containerIds.push_back(container.id);
		m_native = viewcontainer::CSenpViewContainers::Create({ m_declarations.m_parkingParent,
			m_owner, m_containers, std::move(native), m_declarations.m_requestFocus,
			[](std::string_view, std::string_view) { return false; } });
		if (!m_native) return false;
		auto catalog = m_declarations.m_catalog.PrepareOwnerReplacement(m_owner, 0, m_containers, m_views);
		if (catalog.status != CatalogStatus::Prepared || !catalog.change) return false;
		m_catalogChange = std::move(catalog.change);
		m_pageChange.emplace(m_declarations.m_pages.PrepareContributedPages(m_native->PageDescriptors()));
		return m_pageChange->Succeeded();
	}
	bool Commit() noexcept
	{
		if (!m_catalogChange || !m_pageChange || !m_native->IsUsable() || !m_bodies->IsUsable()
			|| !m_declarations.m_catalog.CanCommit(*m_catalogChange)
			|| !m_declarations.m_pages.CanCommit(*m_pageChange)) return false;
		if (!m_declarations.m_pages.Commit(std::move(*m_pageChange)).Succeeded()) return false;
		if (m_declarations.m_catalog.Commit(std::move(m_catalogChange)) != CatalogStatus::Committed) return false;
		m_committed = true; return true;
	}
	void Close() noexcept
	{
		if (m_closed) return;
		m_closed = true;
		if (m_committed && m_declarations.m_catalog.IsOwnerCurrent(m_owner)) {
			(void)m_declarations.m_pages.RemoveContributedPages(m_containerIds);
			(void)m_declarations.m_catalog.DisposeOwner(m_owner);
		}
		if (m_native) m_native->Close();
		if (m_bodies) m_bodies->Close();
	}
	CSenpViewDeclarations& m_declarations;
	layout::WorkbenchContributionOwner m_owner;
	std::vector<layout::WorkbenchViewContainerDescriptor> m_containers;
	std::vector<layout::WorkbenchViewDescriptor> m_views;
	std::vector<std::string> m_containerIds;
	std::shared_ptr<CSenpDeclaredTreeViews> m_bodies;
	std::shared_ptr<viewcontainer::CSenpViewContainers> m_native;
	std::unique_ptr<layout::PreparedWorkbenchContributions> m_catalogChange;
	std::optional<viewcontainer::CViewContainerPages::PreparedContributedPages> m_pageChange;
	bool m_committed{}, m_closed{};
};

CSenpViewDeclarations::CSenpViewDeclarations(layout::WorkbenchContributionRegistry& catalog,
	viewcontainer::CViewContainerPages& pages, HWND parkingParent,
	SenpDeclaredViewActivation requestActivation, std::function<bool(std::string_view)> requestFocus)
	: m_catalog(catalog), m_pages(pages), m_parkingParent(parkingParent),
	m_requestActivation(std::move(requestActivation)), m_requestFocus(std::move(requestFocus)) {}
CSenpViewDeclarations::~CSenpViewDeclarations() { Close(); }

SenpViewDeclarationStatus CSenpViewDeclarations::Register(layout::WorkbenchContributionOwner owner,
	std::vector<layout::WorkbenchViewContainerDescriptor> containers,
	std::vector<layout::WorkbenchViewDescriptor> views) noexcept
{
	if (m_closed) return Status::Stopped;
	if (m_entered) return Status::Conflict;
	DeclarationCall call(m_entered);
	try {
		const auto id = commands::json::ToWideStrict(owner.ownerId);
		if (!id || !layout::WorkbenchContributionRegistry::IsValidStableId(owner.ownerId)
			|| !owner.generation || containers.empty() || containers.size() > 64 || views.empty() || views.size() > 64
			|| !::IsWindow(m_parkingParent) || !m_requestActivation || !m_requestFocus) return Status::Invalid;
		const auto found = m_entries.find(*id);
		if (found != m_entries.end()) return found->second->m_containers == containers && found->second->m_views == views
			&& m_catalog.IsOwnerCurrent(found->second->m_owner) && found->second->m_native->IsUsable()
			? Status::Unchanged : Status::Conflict;
		if (m_entries.size() >= 64) return Status::Invalid;
		// Allocate the map node before either authority commits. Insertion after
		// both commits transfers this prepared node and cannot allocate or call UI.
		decltype(m_entries) prepared;
		auto entry = std::make_unique<Entry>(*this, std::move(owner), std::move(containers), std::move(views));
		if (!entry->Prepare(*id)) return Status::Failed;
		auto inserted = prepared.emplace(*id, std::move(entry));
		if (!inserted.first->second->Commit()) return Status::Conflict;
		m_entries.insert(prepared.extract(inserted.first));
		return Status::Registered;
	} catch (...) { return Status::Failed; }
}

std::unique_ptr<ISenpDeclaredTreePublication> CSenpViewDeclarations::Bind(
	const senp::ContributionOwnerIdentity& runtimeOwner, std::vector<SenpOwnerBoundTree> trees) noexcept
{
	if (m_closed || m_entered) return {};
	DeclarationCall call(m_entered);
	const auto found = m_entries.find(runtimeOwner.extensionId);
	return found != m_entries.end() && m_catalog.IsOwnerCurrent(found->second->m_owner)
		&& found->second->m_native->IsUsable()
		? found->second->m_bodies->PrepareBinding(runtimeOwner, std::move(trees)) : nullptr;
}

bool CSenpViewDeclarations::Pump(std::wstring_view extensionId, SenpExtensionActivationState state) noexcept
{
	if (m_closed || m_entered) return false;
	DeclarationCall call(m_entered);
	const auto found = m_entries.find(extensionId);
	return found != m_entries.end() && m_catalog.IsOwnerCurrent(found->second->m_owner)
		&& found->second->m_native->IsUsable() && found->second->m_bodies->Pump(state);
}

bool CSenpViewDeclarations::ApplyLayout(const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept
{
	if (m_closed || m_entered) return false;
	DeclarationCall call(m_entered);
	for (const auto& [id, entry] : m_entries) {
		if (!m_catalog.IsOwnerCurrent(entry->m_owner)
			|| entry->m_native->ApplyLayout(snapshot) != viewcontainer::ESenpViewProjectionStatus::Applied) return false;
	}
	return true;
}

bool CSenpViewDeclarations::FocusView(std::string_view viewId) noexcept
{
	if (m_closed || m_entered) return false;
	DeclarationCall call(m_entered);
	for (const auto& [id, entry] : m_entries) {
		if (std::ranges::any_of(entry->m_views, [&](const auto& view) { return view.id == viewId; }))
			return m_catalog.IsOwnerCurrent(entry->m_owner) && entry->m_native->FocusView(viewId);
	}
	return false;
}

bool CSenpViewDeclarations::Remove(std::wstring_view extensionId) noexcept
{
	if (m_closed || m_entered) return false;
	DeclarationCall call(m_entered);
	const auto found = m_entries.find(extensionId);
	if (found == m_entries.end()) return true;
	auto removed = m_entries.extract(found);
	removed.mapped()->Close(); return true;
}

void CSenpViewDeclarations::Close() noexcept
{
	if (m_closed) return;
	m_closed = true; m_requestActivation = {}; m_requestFocus = {};
	for (auto& [id, entry] : m_entries) entry->Close();
	m_entries.clear();
}

} // namespace workbench
