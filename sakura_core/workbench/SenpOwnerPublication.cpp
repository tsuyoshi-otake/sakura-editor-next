/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/SenpOwnerPublication.h"

#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/tree/SenpTreeView.h"

#include <algorithm>
#include <utility>

namespace workbench {
namespace {
using CatalogStatus = layout::EWorkbenchContributionChangeStatus;
using ProjectionStatus = ESenpOwnerProjectionStatus;
constexpr std::string_view kTreeProvider = "senp.tree";

std::optional<std::string> ToUtf8Strict(const std::wstring_view value)
{
	auto bytes = commands::json::ToUtf8(value);
	if (!value.empty() && bytes.empty()) return std::nullopt;
	const auto roundTrip = commands::json::ToWideStrict(bytes);
	return roundTrip && *roundTrip == value ? std::optional<std::string>(std::move(bytes)) : std::nullopt;
}

bool SameOwnerContext(const senp::ContributionOwnerIdentity& owner,
	const senp::effect::OperationContext& context) noexcept
{
	return context.ownerGeneration == owner.generation
		&& context.workspaceRevision == owner.workspaceRevision
		&& context.accountGeneration == owner.accountGeneration;
}
}

class CSenpOwnerPublicationHub::State final {
public:
	State(CSenpOwnerPublicationHub& hub, senp::ContributionOwnerIdentity owner,
		layout::WorkbenchContributionOwner catalogOwner,
		std::unique_ptr<ISenpOwnerProjectionTarget> target) noexcept
		: m_hub(hub), m_owner(std::move(owner)), m_catalogOwner(std::move(catalogOwner)),
		m_target(std::move(target)) {}

	~State() { Close(senp::effect::StopReason::Shutdown); }
	State(const State&) = delete;
	State& operator=(const State&) = delete;

	bool Initialize(SenpOwnerPublicationOptions options) noexcept
	{
		try {
			if (!m_target || !::IsWindow(options.ParkingParent()) || options.Containers().empty()) return false;
			m_projection = std::make_unique<CSenpOwnerProjection>(m_hub.m_owners, m_owner, *m_target);

			auto trees = options.TakeTrees();
			std::vector<layout::WorkbenchViewDescriptor> views;
			std::vector<viewcontainer::SenpNativeViewDefinition> nativeViews;
			views.reserve(trees.size());
			nativeViews.reserve(trees.size());
			for (auto& contribution : trees) {
				const auto descriptor = contribution.Descriptor();
				if (descriptor.provider != kTreeProvider) return false;
				const auto viewId = commands::json::ToWideStrict(descriptor.id);
				const auto title = commands::json::ToWideStrict(descriptor.title);
				if (!viewId || !title) return false;
				std::vector<std::wstring> commands;
				for (const auto& command : contribution.TakeCommands()) {
					auto converted = commands::json::ToWideStrict(command);
					if (!converted) return false;
					commands.push_back(std::move(*converted));
				}
				if (!m_projection->RegisterTree(*viewId, std::move(commands))) return false;
				auto provider = m_projection->Tree(*viewId);
				if (!provider) return false;
				m_viewIds.push_back(*viewId);
				auto bodyFactory = options.TreeBodyFactory();
				nativeViews.push_back({ descriptor,
					[bodyFactory = std::move(bodyFactory), provider, title = *title](
						viewcontainer::SenpViewBodyHost host) -> std::unique_ptr<viewcontainer::ISenpViewBody> {
						if (bodyFactory) return bodyFactory(std::move(host), provider, title);
						return tree::CSenpTreeView::Create({ std::move(host), provider, title });
					}, {}, false });
				views.push_back(std::move(descriptor));
			}

			for (const auto& container : options.Containers()) m_containerIds.push_back(container.id);

			viewcontainer::SenpViewContainerOptions containerOptions{
				options.ParkingParent(), m_catalogOwner, options.Containers(), std::move(nativeViews),
				options.TakeRequestFocus(),
				[](std::string_view, std::string_view) { return false; },
			};
			m_views = viewcontainer::CSenpViewContainers::Create(std::move(containerOptions));
			if (!m_views) return false;

			auto catalog = m_hub.m_contributions.PrepareOwnerReplacement(
				m_catalogOwner, 0, options.Containers(), views);
			if (catalog.status != CatalogStatus::Prepared || !catalog.change) return false;
			m_catalogChange = std::move(catalog.change);
			m_pageChange.emplace(m_hub.m_pages.PrepareContributedPages(m_views->PageDescriptors()));
			return m_pageChange->Succeeded();
		} catch (...) {
			return false;
		}
	}

	bool Validate(const senp::InvocationResult& result) const noexcept
	{
		if (m_closed || !SameOwnerContext(m_owner, result.context)
			|| (result.status != senp::InvocationStatus::EffectsReady && !result.effects.empty())) return false;
		return std::ranges::all_of(result.effects, [&](const auto& effect) {
			return std::visit([&](const auto& value) {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, senp::effect::StartToolRead>) return !result.activation;
				else if constexpr (std::is_same_v<T, senp::effect::PublishTreePage>
					|| std::is_same_v<T, senp::effect::InvalidateTree>) return OwnsView(value.viewId);
				else return !result.activation;
			}, effect);
		});
	}

	bool Commit() noexcept
	{
		if (m_closed || m_committed || !m_catalogChange || !m_pageChange
			|| !m_hub.m_contributions.CanCommit(*m_catalogChange)
			|| !m_hub.m_pages.CanCommit(*m_pageChange)) return false;
		const auto pages = m_hub.m_pages.Commit(std::move(*m_pageChange));
		if (!pages.Succeeded()) return false;
		const auto catalog = m_hub.m_contributions.Commit(std::move(m_catalogChange));
		if (catalog != CatalogStatus::Committed) return false;
		m_committed = true;
		return true;
	}

	bool Apply(senp::InvocationResult result) noexcept
	{
		if (!m_committed || !Validate(result)) return false;
		try {
			if (result.activation) {
				for (auto& effect : result.effects)
					m_invalidations.push_back(std::move(std::get<senp::effect::InvalidateTree>(effect).viewId));
				return true;
			}
			return m_projection->Publish(std::move(result));
		} catch (...) {
			return false;
		}
	}

	ProjectionStatus Pump(const senp::CSenpRuntimeSession::Time now) noexcept
	{
		if (m_closed || !m_committed) return ProjectionStatus::Closed;
		try {
			for (const auto& id : m_invalidations) {
				const auto provider = m_projection->Tree(id);
				if (!provider) return ProjectionStatus::Rejected;
				provider->Refresh(now);
			}
			m_invalidations.clear();
			return m_projection->Pump(now);
		} catch (...) {
			return ProjectionStatus::Rejected;
		}
	}

	void Close(const senp::effect::StopReason) noexcept
	{
		if (m_closed) return;
		m_closed = true;
		m_invalidations.clear();
		if (m_committed && m_hub.m_contributions.IsOwnerCurrent(m_catalogOwner)) {
			(void)m_hub.m_pages.RemoveContributedPages(m_containerIds);
		}
		if (m_views) m_views->Close();
		if (m_projection) m_projection->Close();
		if (m_committed) (void)m_hub.m_contributions.DisposeOwner(m_catalogOwner);
		m_committed = false;
	}

	[[nodiscard]] std::wstring_view ExtensionId() const noexcept { return m_owner.extensionId; }

private:
	bool OwnsView(const std::wstring_view id) const noexcept
	{
		return std::ranges::find(m_viewIds, id) != m_viewIds.end();
	}

	CSenpOwnerPublicationHub& m_hub;
	senp::ContributionOwnerIdentity m_owner;
	layout::WorkbenchContributionOwner m_catalogOwner;
	std::unique_ptr<ISenpOwnerProjectionTarget> m_target;
	std::unique_ptr<CSenpOwnerProjection> m_projection;
	std::shared_ptr<viewcontainer::CSenpViewContainers> m_views;
	std::unique_ptr<layout::PreparedWorkbenchContributions> m_catalogChange;
	std::optional<viewcontainer::CViewContainerPages::PreparedContributedPages> m_pageChange;
	std::vector<std::string> m_containerIds;
	std::vector<std::wstring> m_viewIds;
	std::vector<std::wstring> m_invalidations;
	bool m_committed{};
	bool m_closed{};
};

class CSenpOwnerPublicationHub::Publication final : public senp::ISenpOwnerPublication {
public:
	explicit Publication(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}
	bool Validate(const senp::InvocationResult& result) const noexcept override { return m_state->Validate(result); }
	bool Commit() noexcept override { return m_state->Commit(); }
	bool Apply(senp::InvocationResult result) noexcept override { return m_state->Apply(std::move(result)); }
	void Revoke(const senp::effect::StopReason reason) noexcept override { m_state->Close(reason); }
private:
	std::shared_ptr<State> m_state;
};

CSenpOwnerPublicationHub::CSenpOwnerPublicationHub(senp::CSenpContributionOwners& owners,
	layout::WorkbenchContributionRegistry& contributions,
	viewcontainer::CViewContainerPages& pages) noexcept
	: m_owners(owners), m_contributions(contributions), m_pages(pages) {}

CSenpOwnerPublicationHub::~CSenpOwnerPublicationHub() { Close(); }

std::unique_ptr<senp::ISenpOwnerPublication> CSenpOwnerPublicationHub::Prepare(
	const senp::ContributionOwnerIdentity& candidate,
	const senp::ContributionOwnerIdentity* previous,
	SenpOwnerPublicationOptions options) noexcept
{
	if (m_closed || previous || candidate.generation <= 0) return {};
	const auto ownerId = ToUtf8Strict(candidate.extensionId);
	if (!ownerId) return {};
	try {
		auto state = std::make_shared<State>(*this, candidate,
			layout::WorkbenchContributionOwner{ *ownerId, static_cast<std::uint64_t>(candidate.generation) },
			options.TakeTarget());
		if (!state->Initialize(std::move(options))) return {};
		m_publications.push_back(state);
		return std::make_unique<Publication>(std::move(state));
	} catch (...) {
		return {};
	}
}

bool CSenpOwnerPublicationHub::Pump(const senp::CSenpRuntimeSession::Time now) noexcept
{
	if (m_closed) return false;
	bool accepted = true;
	for (auto it = m_publications.begin(); it != m_publications.end();) {
		auto state = it->lock();
		if (!state) { it = m_publications.erase(it); continue; }
		const auto status = state->Pump(now);
		if (status == ProjectionStatus::Rejected) {
			const auto id = std::wstring(state->ExtensionId());
			state->Close(senp::effect::StopReason::ProtocolError);
			(void)m_owners.Revoke(id, senp::effect::StopReason::ProtocolError);
			accepted = false;
		}
		++it;
	}
	return accepted;
}

void CSenpOwnerPublicationHub::Close() noexcept
{
	if (m_closed) return;
	m_closed = true;
	for (auto& weak : m_publications) if (auto state = weak.lock())
		state->Close(senp::effect::StopReason::Shutdown);
	m_publications.clear();
}

} // namespace workbench
