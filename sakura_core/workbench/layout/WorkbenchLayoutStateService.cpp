/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/layout/WorkbenchLayoutStateService.h"

#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <string_view>
#include <utility>

namespace workbench::layout {
namespace {

struct SubscriptionSlot {
	explicit SubscriptionSlot(WorkbenchLayoutChangeCallback value)
		: callback(std::move(value)) {}
	std::atomic_bool active = true;
	WorkbenchLayoutChangeCallback callback;
};

class WorkbenchLayoutSubscription final : public IWorkbenchLayoutSubscription {
public:
	WorkbenchLayoutSubscription(std::weak_ptr<WorkbenchLayoutSubscriptionState> state, std::uint64_t id) noexcept
		: m_state(std::move(state)), m_id(id) {}
	~WorkbenchLayoutSubscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;
private:
	std::weak_ptr<WorkbenchLayoutSubscriptionState> m_state;
	std::uint64_t m_id = 0;
};

void AppendField(std::string& output, std::string_view field)
{
	output.append(std::to_string(field.size()));
	output.push_back(':');
	output.append(field);
}

void AppendUnsigned(std::string& output, std::uint64_t value)
{
	AppendField(output, std::to_string(value));
}

void AppendOperation(std::string& output, const WorkbenchLayoutOperationMetadata& operation)
{
	AppendField(output, operation.expectedRevision ? "expected" : "any");
	if (operation.expectedRevision) AppendUnsigned(output, *operation.expectedRevision);
}

[[nodiscard]] EWorkbenchViewContainerLocation ToStateLocation(EViewContainerLocation value) noexcept
{
	switch (value) {
	case EViewContainerLocation::Sidebar: return EWorkbenchViewContainerLocation::SideBar;
	case EViewContainerLocation::Panel: return EWorkbenchViewContainerLocation::Panel;
	case EViewContainerLocation::AuxiliaryBar: return EWorkbenchViewContainerLocation::AuxiliaryBar;
	}
	return EWorkbenchViewContainerLocation::SideBar;
}

[[nodiscard]] EWorkbenchPartPosition DefaultPartPosition(std::string_view id) noexcept
{
	if (id == ids::part::Titlebar || id == ids::part::Banner) return EWorkbenchPartPosition::Top;
	if (id == ids::part::Activitybar || id == ids::part::Sidebar) return EWorkbenchPartPosition::Left;
	if (id == ids::part::Panel || id == ids::part::Statusbar) return EWorkbenchPartPosition::Bottom;
	if (id == ids::part::Auxiliarybar) return EWorkbenchPartPosition::Right;
	return EWorkbenchPartPosition::Center;
}

[[nodiscard]] bool DefaultPartVisibility(std::string_view id) noexcept
{
	// Match VS Code's fresh-workspace visibility: the primary side bar is shown,
	// while the bottom panel and secondary side bar open only on demand. Extension
	// parts remain visible by default unless their owner contributes another policy.
	// The Restricted Mode banner is the same "starts closed" shape: VS Code never
	// shows it until a live trust computation says the window is restricted, so a
	// fresh layout with no computed answer yet must start it hidden, not visible.
	return id != ids::part::Panel && id != ids::part::Auxiliarybar && id != ids::part::Banner;
}

[[nodiscard]] bool IsValidFocus(const WorkbenchFocusState& focus) noexcept
{
	return (!focus.partId || WorkbenchContributionRegistry::IsValidStableId(*focus.partId))
		&& (!focus.containerId || WorkbenchContributionRegistry::IsValidStableId(*focus.containerId))
		&& (!focus.viewId || WorkbenchContributionRegistry::IsValidStableId(*focus.viewId));
}

[[nodiscard]] bool IsValidLocation(EWorkbenchViewContainerLocation value) noexcept
{
	return value == EWorkbenchViewContainerLocation::SideBar || value == EWorkbenchViewContainerLocation::Panel
		|| value == EWorkbenchViewContainerLocation::AuxiliaryBar;
}

constexpr std::array<EWorkbenchViewContainerLocation, 3> kWorkbenchContainerLocations{
	EWorkbenchViewContainerLocation::SideBar,
	EWorkbenchViewContainerLocation::Panel,
	EWorkbenchViewContainerLocation::AuxiliaryBar,
};

[[nodiscard]] std::optional<std::string>& ActiveContainerFor(
	WorkbenchActiveContainerState& state, EWorkbenchViewContainerLocation location) noexcept
{
	switch (location) {
	case EWorkbenchViewContainerLocation::SideBar: return state.sideBar;
	case EWorkbenchViewContainerLocation::Panel: return state.panel;
	case EWorkbenchViewContainerLocation::AuxiliaryBar: return state.auxiliaryBar;
	}
	return state.sideBar;
}

[[nodiscard]] const std::optional<std::string>& ActiveContainerFor(
	const WorkbenchActiveContainerState& state, EWorkbenchViewContainerLocation location) noexcept
{
	switch (location) {
	case EWorkbenchViewContainerLocation::SideBar: return state.sideBar;
	case EWorkbenchViewContainerLocation::Panel: return state.panel;
	case EWorkbenchViewContainerLocation::AuxiliaryBar: return state.auxiliaryBar;
	}
	return state.sideBar;
}

[[nodiscard]] std::string_view PartIdForLocation(EWorkbenchViewContainerLocation location) noexcept
{
	switch (location) {
	case EWorkbenchViewContainerLocation::SideBar: return ids::part::Sidebar;
	case EWorkbenchViewContainerLocation::Panel: return ids::part::Panel;
	case EWorkbenchViewContainerLocation::AuxiliaryBar: return ids::part::Auxiliarybar;
	}
	return {};
}

[[nodiscard]] std::optional<std::string> FirstVisibleContainer(
	const std::map<std::string, WorkbenchViewContainerState>& containers,
	EWorkbenchViewContainerLocation location)
{
	std::optional<std::pair<std::int32_t, std::string>> first;
	for (const auto& [containerId, container] : containers) {
		if (!container.visible || container.location != location) continue;
		const auto candidate = std::pair{ container.order, containerId };
		if (!first || candidate < *first) first = candidate;
	}
	return first ? std::optional<std::string>{ std::move(first->second) } : std::nullopt;
}

[[nodiscard]] EWorkbenchLayoutOperationReason ValidateLiveFocus(
	const WorkbenchFocusState& focus,
	const std::map<std::string, WorkbenchPartState>& parts,
	const std::map<std::string, WorkbenchViewContainerState>& containers,
	const std::map<std::string, WorkbenchViewState>& views,
	const WorkbenchActiveContainerState& activeContainers)
{
	const WorkbenchPartState* explicitPart = nullptr;
	if (focus.partId) {
		const auto found = parts.find(*focus.partId);
		if (found == parts.end()) return EWorkbenchLayoutOperationReason::UnknownPart;
		explicitPart = &found->second;
		if (!explicitPart->visible) return EWorkbenchLayoutOperationReason::TargetNotVisible;
	}

	const WorkbenchViewContainerState* resolvedContainer = nullptr;
	if (focus.containerId) {
		const auto found = containers.find(*focus.containerId);
		if (found == containers.end()) return EWorkbenchLayoutOperationReason::UnknownContainer;
		resolvedContainer = &found->second;
	}

	const WorkbenchViewState* resolvedView = nullptr;
	if (focus.viewId) {
		const auto found = views.find(*focus.viewId);
		if (found == views.end()) return EWorkbenchLayoutOperationReason::UnknownView;
		resolvedView = &found->second;
		const auto parent = containers.find(resolvedView->containerId);
		if (parent == containers.end()) return EWorkbenchLayoutOperationReason::UnknownContainer;
		if (resolvedContainer && resolvedContainer->containerId != resolvedView->containerId)
			return EWorkbenchLayoutOperationReason::InconsistentHierarchy;
		resolvedContainer = &parent->second;
		if (!resolvedView->visible) return EWorkbenchLayoutOperationReason::TargetNotVisible;
	}

	if (resolvedContainer) {
		if (!resolvedContainer->visible) return EWorkbenchLayoutOperationReason::TargetNotVisible;
		const auto requiredPartId = PartIdForLocation(resolvedContainer->location);
		if (explicitPart && explicitPart->partId != requiredPartId)
			return EWorkbenchLayoutOperationReason::InconsistentHierarchy;
		const auto parentPart = parts.find(std::string(requiredPartId));
		if (parentPart == parts.end()) return EWorkbenchLayoutOperationReason::UnknownPart;
		if (!parentPart->second.visible) return EWorkbenchLayoutOperationReason::TargetNotVisible;
		const auto& active = ActiveContainerFor(activeContainers, resolvedContainer->location);
		if (!active || *active != resolvedContainer->containerId)
			return EWorkbenchLayoutOperationReason::TargetNotActive;
	}
	if (resolvedView && (!resolvedContainer->activeViewId
		|| *resolvedContainer->activeViewId != resolvedView->viewId)) {
		return EWorkbenchLayoutOperationReason::TargetNotActive;
	}
	return EWorkbenchLayoutOperationReason::None;
}

[[nodiscard]] WorkbenchFocusState FallbackFocus(
	const std::map<std::string, WorkbenchPartState>& parts,
	const std::map<std::string, WorkbenchViewContainerState>& containers,
	const std::map<std::string, WorkbenchViewState>& views,
	const WorkbenchActiveContainerState& activeContainers)
{
	const auto editor = parts.find(std::string(ids::part::Editor));
	if (editor != parts.end() && editor->second.visible)
		return { .partId = editor->first };

	for (const auto location : kWorkbenchContainerLocations) {
		const auto& activeId = ActiveContainerFor(activeContainers, location);
		if (!activeId) continue;
		const auto container = containers.find(*activeId);
		if (container == containers.end() || !container->second.visible
			|| container->second.location != location) continue;
		const auto partId = std::string(PartIdForLocation(location));
		const auto part = parts.find(partId);
		if (part == parts.end() || !part->second.visible) continue;
		WorkbenchFocusState focus{ .partId = partId, .containerId = container->first };
		if (container->second.activeViewId) {
			const auto view = views.find(*container->second.activeViewId);
			if (view != views.end() && view->second.visible
				&& view->second.containerId == container->first) {
				focus.viewId = view->first;
			}
		}
		return focus;
	}
	for (const auto& [partId, part] : parts)
		if (part.visible) return { .partId = partId };
	return {};
}

[[nodiscard]] bool IsValidPartPosition(EWorkbenchPartPosition value) noexcept
{
	return value == EWorkbenchPartPosition::Top || value == EWorkbenchPartPosition::Left
		|| value == EWorkbenchPartPosition::Center || value == EWorkbenchPartPosition::Right
		|| value == EWorkbenchPartPosition::Bottom;
}

[[nodiscard]] bool IsValidPanelAlignment(EWorkbenchPanelAlignment value) noexcept
{
	return value == EWorkbenchPanelAlignment::Left || value == EWorkbenchPanelAlignment::Center
		|| value == EWorkbenchPanelAlignment::Right || value == EWorkbenchPanelAlignment::Justify;
}

void Deliver(const std::shared_ptr<WorkbenchLayoutSubscriptionState>& state,
	const WorkbenchLayoutChangeBatch& batch);

} // namespace

struct WorkbenchLayoutSubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::uint64_t nextId = 1;
	std::map<std::uint64_t, std::shared_ptr<SubscriptionSlot>> slots;
};

struct WorkbenchLayoutStateService::ContributionIndex {
	struct Part { bool supportsVisibility = false; EWorkbenchPartPosition position = EWorkbenchPartPosition::Center; };
	struct Container { EWorkbenchViewContainerLocation location = EWorkbenchViewContainerLocation::SideBar; std::int32_t order = 0; bool hideIfEmpty = false; bool canMove = false; };
	struct View { std::string defaultContainerId; std::int32_t order = 0; bool canToggleVisibility = false; bool canMove = false; };
	std::map<std::string, Part> parts;
	std::map<std::string, Container> containers;
	std::map<std::string, View> views;
};

struct WorkbenchLayoutStateService::CompletedOperation {
	std::string fingerprint;
	WorkbenchLayoutOperationResult result;
};

namespace {

void WorkbenchLayoutSubscription::Unsubscribe() noexcept
{
	try {
		auto state = m_state.lock();
		if (!state || m_id == 0) { m_id = 0; return; }
		std::scoped_lock lock(state->mutex);
		if (const auto found = state->slots.find(m_id); found != state->slots.end()) {
			found->second->active.store(false, std::memory_order_release);
			state->slots.erase(found);
		}
		m_id = 0;
	} catch (...) { m_id = 0; }
}

bool WorkbenchLayoutSubscription::IsSubscribed() const noexcept
{
	try {
		auto state = m_state.lock();
		if (!state || m_id == 0) return false;
		std::scoped_lock lock(state->mutex);
		const auto found = state->slots.find(m_id);
		return !state->closed && found != state->slots.end()
			&& found->second->active.load(std::memory_order_acquire);
	} catch (...) { return false; }
}

void Deliver(const std::shared_ptr<WorkbenchLayoutSubscriptionState>& state,
	const WorkbenchLayoutChangeBatch& batch)
{
	std::vector<std::shared_ptr<SubscriptionSlot>> listeners;
	{
		std::scoped_lock lock(state->mutex);
		if (state->closed) return;
		listeners.reserve(state->slots.size());
		for (const auto& [id, slot] : state->slots) { (void)id; listeners.push_back(slot); }
	}
	for (const auto& listener : listeners) {
		if (!listener->active.load(std::memory_order_acquire)) continue;
		try { listener->callback(batch); } catch (...) { /* Observer failures are isolated. */ }
	}
}

[[nodiscard]] std::string FingerprintPrefix(std::string_view operation, const WorkbenchLayoutOperationMetadata& metadata)
{
	std::string result;
	AppendField(result, operation);
	AppendOperation(result, metadata);
	return result;
}

} // namespace

std::unique_ptr<WorkbenchLayoutStateService::ContributionIndex> WorkbenchLayoutStateService::MakeContributionIndex(
	const WorkbenchContributionSnapshot& contributions)
{
	auto index = std::make_unique<ContributionIndex>();
	for (const auto& registered : contributions.parts) index->parts.emplace(registered.descriptor.id,
		ContributionIndex::Part{ .supportsVisibility = registered.descriptor.supportsVisibility,
			.position = DefaultPartPosition(registered.descriptor.id) });
	for (const auto& registered : contributions.viewContainers) index->containers.emplace(registered.descriptor.id,
		ContributionIndex::Container{ .location = ToStateLocation(registered.descriptor.location),
			.order = registered.descriptor.order, .hideIfEmpty = registered.descriptor.hideIfEmpty,
			.canMove = registered.descriptor.canMove });
	for (const auto& registered : contributions.views) index->views.emplace(registered.descriptor.id,
		ContributionIndex::View{ .defaultContainerId = registered.descriptor.containerId,
			.order = registered.descriptor.order, .canToggleVisibility = registered.descriptor.canToggleVisibility,
			.canMove = registered.descriptor.canMove });
	return index;
}

WorkbenchLayoutStateService::WorkbenchLayoutStateService(const WorkbenchContributionSnapshot& contributions,
	std::uint64_t generation, std::size_t maxCompletedOperations)
	: m_generation(generation == 0 ? 1 : generation)
	, m_maxCompletedOperations(std::clamp<std::size_t>(maxCompletedOperations, 1, kMaxWorkbenchLayoutCompletedOperations))
	, m_contributionIndex(MakeContributionIndex(contributions))
	, m_subscriptionState(std::make_shared<WorkbenchLayoutSubscriptionState>())
{
	for (const auto& [id, descriptor] : m_contributionIndex->parts) {
		m_parts.emplace(id, WorkbenchPartState{
			.partId = id,
			.visible = DefaultPartVisibility(id),
			.position = descriptor.position,
		});
	}
	for (const auto& [id, descriptor] : m_contributionIndex->containers) {
		m_containers.emplace(id, WorkbenchViewContainerState{ .containerId = id, .location = descriptor.location,
			.order = descriptor.order, .visible = !descriptor.hideIfEmpty });
	}
	for (const auto& [id, descriptor] : m_contributionIndex->views) {
		m_views.emplace(id, WorkbenchViewState{ .viewId = id, .containerId = descriptor.defaultContainerId,
			.order = descriptor.order, .visible = true });
		m_viewsByContainer[descriptor.defaultContainerId].emplace(descriptor.order, id);
	}
	for (auto& [containerId, container] : m_containers) {
		if (const auto found = m_viewsByContainer.find(containerId); found != m_viewsByContainer.end()) {
			if (m_contributionIndex->containers.at(containerId).hideIfEmpty && !found->second.empty()) container.visible = true;
			if (!found->second.empty()) container.activeViewId = found->second.begin()->second;
		}
	}
	for (const auto location : kWorkbenchContainerLocations) {
		std::optional<std::pair<std::int32_t, std::string>> first;
		for (const auto& [containerId, container] : m_containers) {
			if (!container.visible || container.location != location) continue;
			const auto candidate = std::pair{ container.order, containerId };
			if (!first || candidate < *first) first = candidate;
		}
		if (first) ActiveContainerFor(m_activeContainers, location) = std::move(first->second);
	}
	m_lastStableSnapshot = SnapshotLocked();
}

WorkbenchLayoutStateService::~WorkbenchLayoutStateService()
{
	{
		std::scoped_lock lock(m_subscriptionState->mutex);
		m_subscriptionState->closed = true;
		for (const auto& [id, slot] : m_subscriptionState->slots) { (void)id; slot->active.store(false, std::memory_order_release); }
		m_subscriptionState->slots.clear();
	}
	std::scoped_lock notificationLock(m_notificationMutex);
	m_notificationQueue.clear();
}

WorkbenchLayoutStateSnapshot WorkbenchLayoutStateService::SnapshotLocked() const
{
	WorkbenchLayoutStateSnapshot snapshot{ .generation = m_generation, .revision = m_revision,
		.activeContainers = m_activeContainers, .panelAlignment = m_panelAlignment, .focus = m_focus };
	snapshot.parts.reserve(m_parts.size() + m_deferredParts.size());
	snapshot.containers.reserve(m_containers.size() + m_deferredContainers.size());
	snapshot.views.reserve(m_views.size() + m_deferredViews.size());
	for (const auto& [id, value] : m_parts) { (void)id; snapshot.parts.push_back(value); }
	for (const auto& [id, value] : m_containers) { (void)id; snapshot.containers.push_back(value); }
	for (const auto& [id, value] : m_views) { (void)id; snapshot.views.push_back(value); }
	for (const auto& [id, value] : m_deferredParts) { (void)id; snapshot.parts.push_back(value); }
	for (const auto& [id, value] : m_deferredContainers) { (void)id; snapshot.containers.push_back(value); }
	for (const auto& [id, value] : m_deferredViews) { (void)id; snapshot.views.push_back(value); }
	return snapshot;
}

WorkbenchLayoutStateSnapshot WorkbenchLayoutStateService::Snapshot() const
{
	std::scoped_lock lock(m_mutex);
	return SnapshotLocked();
}

WorkbenchLayoutStateSnapshot WorkbenchLayoutStateService::MementoSnapshot() const
{
	std::scoped_lock lock(m_mutex);
	auto snapshot = SnapshotLocked();
	for (const auto location : kWorkbenchContainerLocations) {
		if (const auto& deferred = ActiveContainerFor(m_deferredActiveContainers, location)) {
			ActiveContainerFor(snapshot.activeContainers, location) = *deferred;
		}
	}
	if (m_deferredFocus) snapshot.focus = *m_deferredFocus;
	for (auto& view : snapshot.views) {
		if (const auto deferred = m_deferredViewPlacements.find(view.viewId);
			deferred != m_deferredViewPlacements.end()) view = deferred->second;
	}
	for (auto& container : snapshot.containers) {
		if (const auto deferred = m_deferredActiveViews.find(container.containerId);
			deferred != m_deferredActiveViews.end()) container.activeViewId = deferred->second;
	}
	// The Restricted Mode banner is derived, not chosen: its visibility comes from
	// the workspace trust answer and security.workspace.trust.banner, both of which
	// are recomputed from scratch on every startup. Persisting it would store a
	// value that the next launch immediately overwrites, and restoring it would
	// briefly assert a trust verdict this process has not computed yet. VS Code
	// likewise persists no banner visibility. Keep it in the live snapshot -- the
	// native projection needs it there -- and out of the durable one.
	std::erase_if(snapshot.parts, [](const WorkbenchPartState& part) {
		return part.partId == ids::part::Banner;
	});
	return snapshot;
}

bool WorkbenchLayoutStateService::IsValidPersistedSnapshot(const WorkbenchLayoutStateSnapshot& persisted) noexcept
{
	if (persisted.schemaVersion != kWorkbenchLayoutStateSchemaVersion
		|| !IsValidPanelAlignment(persisted.panelAlignment) || !IsValidFocus(persisted.focus)) return false;
	std::set<std::string, std::less<>> activeContainerIds;
	for (const auto location : kWorkbenchContainerLocations) {
		const auto& active = ActiveContainerFor(persisted.activeContainers, location);
		if (active && (!IsValidWorkbenchLayoutId(*active)
			|| !activeContainerIds.emplace(*active).second)) return false;
	}
	for (std::size_t index = 0; index < persisted.parts.size(); ++index) {
		const auto& part = persisted.parts[index];
		if (!IsValidWorkbenchLayoutId(part.partId) || !IsValidPartPosition(part.position)
			|| (part.committedExtentDip && (*part.committedExtentDip == 0
				|| *part.committedExtentDip > kMaximumWorkbenchLayoutCommittedExtentDip))) return false;
		for (std::size_t prior = 0; prior < index; ++prior)
			if (persisted.parts[prior].partId == part.partId) return false;
	}
	for (std::size_t index = 0; index < persisted.containers.size(); ++index) {
		const auto& container = persisted.containers[index];
		if (!IsValidWorkbenchLayoutId(container.containerId) || !IsValidLocation(container.location)
			|| (container.activeViewId && !IsValidWorkbenchLayoutId(*container.activeViewId))) return false;
		for (std::size_t prior = 0; prior < index; ++prior)
			if (persisted.containers[prior].containerId == container.containerId) return false;
	}
	for (std::size_t index = 0; index < persisted.views.size(); ++index) {
		const auto& view = persisted.views[index];
		if (!IsValidWorkbenchLayoutId(view.viewId) || !IsValidWorkbenchLayoutId(view.containerId)) return false;
		for (std::size_t prior = 0; prior < index; ++prior)
			if (persisted.views[prior].viewId == view.viewId) return false;
	}
	return true;
}

WorkbenchLayoutHydrationResult WorkbenchLayoutStateService::HydrateInitialState(
	const WorkbenchLayoutStateSnapshot& persisted)
{
	std::scoped_lock lock(m_mutex);
	if (m_initialHydrationCompleted)
		return { .status = EWorkbenchLayoutHydrationStatus::AlreadyHydrated, .snapshot = SnapshotLocked() };
	if (!IsValidPersistedSnapshot(persisted))
		return { .status = EWorkbenchLayoutHydrationStatus::InvalidSnapshot, .snapshot = SnapshotLocked() };

	try {
		auto nextParts = m_parts;
		auto nextContainers = m_containers;
		auto nextViews = m_views;
		std::map<std::string, WorkbenchPartState> deferredParts;
		std::map<std::string, WorkbenchViewContainerState> deferredContainers;
		std::map<std::string, WorkbenchViewState> deferredViews;
		std::map<std::string, WorkbenchViewState> deferredViewPlacements;
		std::map<std::string, std::string> deferredActiveViews;
		for (const auto& part : persisted.parts) {
			if (m_contributionIndex->parts.contains(part.partId)) nextParts.at(part.partId) = part;
			else deferredParts.emplace(part.partId, part);
		}
		for (const auto& container : persisted.containers) {
			if (m_contributionIndex->containers.contains(container.containerId)) {
				nextContainers.at(container.containerId) = container;
			} else deferredContainers.emplace(container.containerId, container);
		}
		for (const auto& view : persisted.views) {
			if (m_contributionIndex->views.contains(view.viewId)) {
				if (nextContainers.contains(view.containerId))
					nextViews.at(view.viewId) = view;
				else
					deferredViewPlacements.emplace(view.viewId, view);
			} else {
				deferredViews.emplace(view.viewId, view);
			}
		}
		std::map<std::string, std::set<std::pair<std::int32_t, std::string>>> nextIndex;
		for (auto& [id, view] : nextViews) {
			if (!nextContainers.contains(view.containerId)) {
				const auto& descriptor = m_contributionIndex->views.at(id);
				view = { .viewId = id, .containerId = descriptor.defaultContainerId, .order = descriptor.order, .visible = true };
			}
			nextIndex[view.containerId].emplace(view.order, id);
		}
		for (auto& [id, container] : nextContainers) {
			if (container.activeViewId && (!nextViews.contains(*container.activeViewId)
				|| nextViews.at(*container.activeViewId).containerId != id
				|| !nextViews.at(*container.activeViewId).visible)) {
				deferredActiveViews.emplace(id, *container.activeViewId);
				container.activeViewId.reset();
			}
			if (!container.activeViewId) {
				const auto found = nextIndex.find(id);
				if (found != nextIndex.end()) for (const auto& [order, viewId] : found->second) {
					(void)order;
					if (nextViews.at(viewId).visible) { container.activeViewId = viewId; break; }
				}
			}
		}
		WorkbenchActiveContainerState nextActiveContainers;
		WorkbenchActiveContainerState deferredActiveContainers;
		for (const auto location : kWorkbenchContainerLocations) {
			const auto& persistedActive = ActiveContainerFor(persisted.activeContainers, location);
			if (persistedActive) {
				const auto found = nextContainers.find(*persistedActive);
				if (found != nextContainers.end() && found->second.location == location
					&& found->second.visible) {
					ActiveContainerFor(nextActiveContainers, location) = *persistedActive;
				} else {
					ActiveContainerFor(deferredActiveContainers, location) = *persistedActive;
				}
			}
			if (!ActiveContainerFor(nextActiveContainers, location)) {
				ActiveContainerFor(nextActiveContainers, location) =
					FirstVisibleContainer(nextContainers, location);
			}
		}
		const bool focusLive = ValidateLiveFocus(persisted.focus, nextParts, nextContainers,
			nextViews, nextActiveContainers) == EWorkbenchLayoutOperationReason::None;
		WorkbenchFocusState nextFocus = focusLive ? persisted.focus : WorkbenchFocusState{};
		std::optional<WorkbenchFocusState> deferredFocus;
		if (!focusLive) deferredFocus = persisted.focus;

		WorkbenchLayoutStateSnapshot nextStable{ .generation = m_generation, .revision = m_revision,
			.activeContainers = nextActiveContainers,
			.panelAlignment = persisted.panelAlignment, .focus = nextFocus };
		nextStable.parts.reserve(nextParts.size() + deferredParts.size());
		nextStable.containers.reserve(nextContainers.size() + deferredContainers.size());
		nextStable.views.reserve(nextViews.size() + deferredViews.size());
		for (const auto& [id, value] : nextParts) { (void)id; nextStable.parts.push_back(value); }
		for (const auto& [id, value] : nextContainers) { (void)id; nextStable.containers.push_back(value); }
		for (const auto& [id, value] : nextViews) { (void)id; nextStable.views.push_back(value); }
		for (const auto& [id, value] : deferredParts) { (void)id; nextStable.parts.push_back(value); }
		for (const auto& [id, value] : deferredContainers) { (void)id; nextStable.containers.push_back(value); }
		for (const auto& [id, value] : deferredViews) { (void)id; nextStable.views.push_back(value); }
		// Stage every potentially throwing copy, including the return value, before
		// changing service members.  The commit below consists only of moves of
		// standard-allocator containers/strings and scalar assignments.
		auto nextLastStableDeferredFocus = deferredFocus;
		auto nextLastStableDeferredActiveViews = deferredActiveViews;
		auto nextLastStableDeferredViewPlacements = deferredViewPlacements;
		auto nextLastStableDeferredActiveContainers = deferredActiveContainers;
		WorkbenchLayoutHydrationResult succeeded{ .status = EWorkbenchLayoutHydrationStatus::Succeeded,
			.snapshot = nextStable };

		m_parts = std::move(nextParts);
		m_containers = std::move(nextContainers);
		m_views = std::move(nextViews);
		m_viewsByContainer = std::move(nextIndex);
		m_deferredParts = std::move(deferredParts);
		m_deferredContainers = std::move(deferredContainers);
		m_deferredViews = std::move(deferredViews);
		m_deferredViewPlacements = std::move(deferredViewPlacements);
		m_deferredActiveViews = std::move(deferredActiveViews);
		m_activeContainers = std::move(nextActiveContainers);
		m_deferredActiveContainers = std::move(deferredActiveContainers);
		m_deferredFocus = std::move(deferredFocus);
		m_focus = std::move(nextFocus);
		m_panelAlignment = persisted.panelAlignment;
		m_lastStableSnapshot = std::move(nextStable);
		m_lastStableDeferredFocus = std::move(nextLastStableDeferredFocus);
		m_lastStableDeferredActiveViews = std::move(nextLastStableDeferredActiveViews);
		m_lastStableDeferredViewPlacements = std::move(nextLastStableDeferredViewPlacements);
		m_lastStableDeferredActiveContainers = std::move(nextLastStableDeferredActiveContainers);
		m_initialHydrationCompleted = true;
		return succeeded;
	} catch (...) {
		return { .status = EWorkbenchLayoutHydrationStatus::Failed, .snapshot = SnapshotLocked() };
	}
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::ResultLocked(EWorkbenchLayoutOperationStatus status,
	EWorkbenchLayoutOperationReason reason) const
{
	return { .status = status, .reason = reason, .revision = m_revision, .snapshot = SnapshotLocked() };
}

bool WorkbenchLayoutStateService::IsExpectedRevisionCurrentLocked(const WorkbenchLayoutOperationMetadata& operation) const noexcept
{
	return !operation.expectedRevision || *operation.expectedRevision == m_revision;
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::CheckOperationLocked(
	const WorkbenchLayoutOperationMetadata& operation, const std::string& fingerprint, bool& handled) const
{
	handled = false;
	const auto found = m_completedOperations.find(operation.operationId);
	if (found == m_completedOperations.end()) return {};
	handled = true;
	if (found->second.fingerprint == fingerprint) {
		auto result = found->second.result;
		result.replayed = true;
		return result;
	}
	return ResultLocked(EWorkbenchLayoutOperationStatus::Conflict, EWorkbenchLayoutOperationReason::OperationIdConflict);
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::CommitLocked(std::vector<WorkbenchLayoutChange> changes)
{
	if (m_revision == std::numeric_limits<std::uint64_t>::max())
		return ResultLocked(EWorkbenchLayoutOperationStatus::Conflict, EWorkbenchLayoutOperationReason::RevisionExhausted);
	const auto previous = m_revision;
	++m_revision;
	try {
		WorkbenchLayoutChangeBatch batch{ .generation = m_generation, .baseRevision = previous, .revision = m_revision, .changes = std::move(changes) };
		auto snapshot = SnapshotLocked();
		auto stableSnapshot = snapshot;
		auto stableDeferredFocus = m_deferredFocus;
		auto stableDeferredActiveViews = m_deferredActiveViews;
		auto stableDeferredViewPlacements = m_deferredViewPlacements;
		auto stableDeferredActiveContainers = m_deferredActiveContainers;
		WorkbenchLayoutOperationResult result{
			.status = EWorkbenchLayoutOperationStatus::Succeeded,
			.revision = m_revision,
			.changeBatch = std::move(batch),
			.snapshot = std::move(snapshot),
		};
		m_lastStableSnapshot = std::move(stableSnapshot);
		m_lastStableDeferredFocus = std::move(stableDeferredFocus);
		m_lastStableDeferredActiveViews = std::move(stableDeferredActiveViews);
		m_lastStableDeferredViewPlacements = std::move(stableDeferredViewPlacements);
		m_lastStableDeferredActiveContainers = std::move(stableDeferredActiveContainers);
		return result;
	} catch (...) {
		m_revision = previous;
		throw;
	}
}

void WorkbenchLayoutStateService::RestoreLastStableLocked() noexcept
{
	try {
		auto deferredFocus = m_lastStableDeferredFocus;
		auto deferredActiveViews = m_lastStableDeferredActiveViews;
		auto deferredViewPlacements = m_lastStableDeferredViewPlacements;
		auto deferredActiveContainers = m_lastStableDeferredActiveContainers;
		auto activeContainers = m_lastStableSnapshot.activeContainers;
		auto focus = m_lastStableSnapshot.focus;
		std::map<std::string, WorkbenchPartState> parts;
		std::map<std::string, WorkbenchViewContainerState> containers;
		std::map<std::string, WorkbenchViewState> views;
		std::map<std::string, WorkbenchPartState> deferredParts;
		std::map<std::string, WorkbenchViewContainerState> deferredContainers;
		std::map<std::string, WorkbenchViewState> deferredViews;
		std::map<std::string, std::set<std::pair<std::int32_t, std::string>>> index;
		for (const auto& value : m_lastStableSnapshot.parts) {
			if (m_contributionIndex->parts.contains(value.partId)) parts.emplace(value.partId, value);
			else deferredParts.emplace(value.partId, value);
		}
		for (const auto& value : m_lastStableSnapshot.containers) {
			if (m_contributionIndex->containers.contains(value.containerId)) containers.emplace(value.containerId, value);
			else deferredContainers.emplace(value.containerId, value);
		}
		for (const auto& value : m_lastStableSnapshot.views) {
			if (m_contributionIndex->views.contains(value.viewId)) {
				views.emplace(value.viewId, value);
				index[value.containerId].emplace(value.order, value.viewId);
			} else deferredViews.emplace(value.viewId, value);
		}
		m_parts = std::move(parts);
		m_containers = std::move(containers);
		m_views = std::move(views);
		m_deferredParts = std::move(deferredParts);
		m_deferredContainers = std::move(deferredContainers);
		m_deferredViews = std::move(deferredViews);
		m_deferredViewPlacements = std::move(deferredViewPlacements);
		m_deferredFocus = std::move(deferredFocus);
		m_deferredActiveViews = std::move(deferredActiveViews);
		m_deferredActiveContainers = std::move(deferredActiveContainers);
		m_viewsByContainer = std::move(index);
		m_activeContainers = std::move(activeContainers);
		m_panelAlignment = m_lastStableSnapshot.panelAlignment;
		m_focus = std::move(focus);
		m_revision = m_lastStableSnapshot.revision;
	} catch (...) {
		// Allocation failure during recovery is unrecoverable at this boundary; do not publish a new revision.
	}
}

void WorkbenchLayoutStateService::RememberCompletedLocked(const std::string& operationId, std::string fingerprint,
	const WorkbenchLayoutOperationResult& result)
{
	if (m_completedOperations.contains(operationId)) return;
	m_completedOperations.emplace(operationId, CompletedOperation{ .fingerprint = std::move(fingerprint), .result = result });
	m_completedOperationOrder.push_back(operationId);
	while (m_completedOperationOrder.size() > m_maxCompletedOperations) {
		m_completedOperations.erase(m_completedOperationOrder.front());
		m_completedOperationOrder.pop_front();
	}
}

bool WorkbenchLayoutStateService::EnqueueNotification(const WorkbenchLayoutChangeBatch& batch)
{
	std::scoped_lock lock(m_notificationMutex);
	m_notificationQueue.push_back(batch);
	if (m_dispatchingNotifications) return false;
	m_dispatchingNotifications = true;
	return true;
}

void WorkbenchLayoutStateService::DrainNotifications()
{
	for (;;) {
		WorkbenchLayoutChangeBatch batch;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) { m_dispatchingNotifications = false; return; }
			batch = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		Deliver(m_subscriptionState, batch);
	}
}

struct WorkbenchLayoutStateMutator {
	template <class TRequest, class TAction>
	static WorkbenchLayoutOperationResult Run(WorkbenchLayoutStateService& service, const TRequest& request,
		std::string fingerprint, TAction&& action)
	{
	std::optional<WorkbenchLayoutChangeBatch> batch;
	WorkbenchLayoutOperationResult result;
	bool drain = false;
	{
		std::scoped_lock lock(service.m_mutex);
		if (!WorkbenchContributionRegistry::IsValidOperationId(request.operation.operationId))
			return service.ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidOperationId);
		bool handled = false;
		result = service.CheckOperationLocked(request.operation, fingerprint, handled);
		if (handled) return result;
		if (!service.IsExpectedRevisionCurrentLocked(request.operation))
			result = service.ResultLocked(EWorkbenchLayoutOperationStatus::Conflict, EWorkbenchLayoutOperationReason::RevisionConflict);
		else if (service.m_revision == std::numeric_limits<std::uint64_t>::max())
			result = service.ResultLocked(EWorkbenchLayoutOperationStatus::Conflict, EWorkbenchLayoutOperationReason::RevisionExhausted);
		else {
			try { result = action(); } catch (...) {
				service.RestoreLastStableLocked();
				result = service.ResultLocked(EWorkbenchLayoutOperationStatus::Failed, EWorkbenchLayoutOperationReason::InternalFailure);
			}
		}
		service.RememberCompletedLocked(request.operation.operationId, std::move(fingerprint), result);
		if (result.changeBatch) { batch = result.changeBatch; drain = service.EnqueueNotification(*batch); }
	}
	if (drain) service.DrainNotifications();
		return result;
	}

	[[nodiscard]] static std::optional<std::string> FirstVisibleView(const WorkbenchLayoutStateService& service, const std::string& containerId)
	{
	const auto indexed = service.m_viewsByContainer.find(containerId);
	if (indexed == service.m_viewsByContainer.end()) return std::nullopt;
	for (const auto& [order, id] : indexed->second) {
		(void)order;
		const auto view = service.m_views.find(id);
		if (view != service.m_views.end() && view->second.visible) return id;
	}
		return std::nullopt;
	}
};

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::SetPartVisibility(const SetWorkbenchPartVisibilityRequest& request)
{
	auto fingerprint = FingerprintPrefix("part-visibility", request.operation); AppendField(fingerprint, request.partId); AppendField(fingerprint, request.visible ? "1" : "0");
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.partId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		const auto descriptor = m_contributionIndex->parts.find(request.partId);
		if (descriptor == m_contributionIndex->parts.end()) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownPart);
		if (!descriptor->second.supportsVisibility) return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		auto& part = m_parts.at(request.partId);
		if (part.visible == request.visible) return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		part.visible = request.visible;
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::PartVisibilityChanged, .partId = request.partId },
		};
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::SetPartExtent(const SetWorkbenchPartExtentRequest& request)
{
	auto fingerprint = FingerprintPrefix("part-extent", request.operation); AppendField(fingerprint, request.partId); AppendField(fingerprint, request.committedExtentDip ? std::to_string(*request.committedExtentDip) : "none");
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.partId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (request.committedExtentDip && (*request.committedExtentDip == 0 || *request.committedExtentDip > kMaximumWorkbenchLayoutCommittedExtentDip))
			return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (!m_contributionIndex->parts.contains(request.partId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownPart);
		auto& part = m_parts.at(request.partId);
		if (part.committedExtentDip == request.committedExtentDip) return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		part.committedExtentDip = request.committedExtentDip;
		return CommitLocked({ { .kind = EWorkbenchLayoutChangeKind::PartExtentChanged, .partId = request.partId } });
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::RevealContainer(const RevealWorkbenchContainerRequest& request)
{
	auto fingerprint = FingerprintPrefix("reveal-container", request.operation); AppendField(fingerprint, request.containerId);
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (!m_contributionIndex->containers.contains(request.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownContainer);
		auto& container = m_containers.at(request.containerId);
		auto& active = ActiveContainerFor(m_activeContainers, container.location);
		auto& deferred = ActiveContainerFor(m_deferredActiveContainers, container.location);
		const bool reveal = !container.visible;
		const bool activate = !active || (deferred && *deferred == request.containerId);
		if (!reveal && !activate)
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable,
				EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		std::vector<WorkbenchLayoutChange> changes;
		if (reveal) {
			container.visible = true;
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerRevealed,
				.containerId = request.containerId });
		}
		if (activate) {
			active = request.containerId;
			if (deferred && *deferred == request.containerId) deferred.reset();
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = request.containerId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::ActivateContainer(const ActivateWorkbenchContainerRequest& request)
{
	auto fingerprint = FingerprintPrefix("activate-container", request.operation); AppendField(fingerprint, request.containerId);
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (!m_contributionIndex->containers.contains(request.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownContainer);
		auto& container = m_containers.at(request.containerId);
		auto& active = ActiveContainerFor(m_activeContainers, container.location);
		auto& deferred = ActiveContainerFor(m_deferredActiveContainers, container.location);
		const bool selectedViewIsLive = container.activeViewId
			&& m_views.contains(*container.activeViewId)
			&& m_views.at(*container.activeViewId).containerId == request.containerId
			&& m_views.at(*container.activeViewId).visible;
		const auto selectedView = selectedViewIsLive
			? container.activeViewId
			: WorkbenchLayoutStateMutator::FirstVisibleView(*this, request.containerId);
		const bool alreadyActive = active && *active == request.containerId;
		const bool hasDeferredIntent = deferred.has_value()
			|| m_deferredActiveViews.contains(request.containerId);
		if (container.visible && container.activeViewId == selectedView
			&& alreadyActive && !hasDeferredIntent) {
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable,
				EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		}
		const bool revealed = !container.visible;
		container.visible = true;
		container.activeViewId = selectedView;
		active = request.containerId;
		deferred.reset();
		m_deferredActiveViews.erase(request.containerId);
		std::vector<WorkbenchLayoutChange> changes;
		if (revealed) changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerRevealed,
			.containerId = request.containerId });
		if (!alreadyActive || !selectedViewIsLive || hasDeferredIntent)
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = request.containerId });
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::RevealView(const RevealWorkbenchViewRequest& request)
{
	auto fingerprint = FingerprintPrefix("reveal-view", request.operation); AppendField(fingerprint, request.viewId);
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.viewId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		const auto meta = m_contributionIndex->views.find(request.viewId);
		if (meta == m_contributionIndex->views.end()) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownView);
		auto& view = m_views.at(request.viewId); auto& container = m_containers.at(view.containerId);
		auto& active = ActiveContainerFor(m_activeContainers, container.location);
		auto& deferredContainer = ActiveContainerFor(m_deferredActiveContainers, container.location);
		const auto deferredView = m_deferredActiveViews.find(view.containerId);
		const bool materializeView = deferredView != m_deferredActiveViews.end()
			&& deferredView->second == request.viewId;
		const bool materializeContainer = !active
			|| (deferredContainer && *deferredContainer == view.containerId);
		if (view.visible && container.visible && !materializeView && !materializeContainer)
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		if (!view.visible && !meta->second.canToggleVisibility) return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		view.visible = true; container.visible = true;
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::ViewRevealed,
				.containerId = view.containerId, .viewId = request.viewId },
		};
		if (materializeView) {
			container.activeViewId = request.viewId;
			m_deferredActiveViews.erase(deferredView);
		}
		if (materializeContainer) {
			active = view.containerId;
			if (deferredContainer && *deferredContainer == view.containerId)
				deferredContainer.reset();
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = view.containerId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::SetViewVisibility(const SetWorkbenchViewVisibilityRequest& request)
{
	auto fingerprint = FingerprintPrefix("view-visibility", request.operation); AppendField(fingerprint, request.viewId); AppendField(fingerprint, request.visible ? "1" : "0");
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.viewId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		const auto meta = m_contributionIndex->views.find(request.viewId);
		if (meta == m_contributionIndex->views.end()) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownView);
		if (!meta->second.canToggleVisibility) return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		auto& view = m_views.at(request.viewId);
		if (view.visible == request.visible) return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		view.visible = request.visible;
		auto& container = m_containers.at(view.containerId);
		if (!view.visible && container.activeViewId && *container.activeViewId == request.viewId)
			container.activeViewId = WorkbenchLayoutStateMutator::FirstVisibleView(*this, view.containerId);
		if (view.visible) {
			if (const auto deferred = m_deferredActiveViews.find(view.containerId);
				deferred != m_deferredActiveViews.end() && deferred->second == request.viewId) {
				container.activeViewId = request.viewId;
				m_deferredActiveViews.erase(deferred);
			}
		}
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::ViewRevealed,
				.containerId = view.containerId, .viewId = request.viewId },
		};
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::ActivateView(const ActivateWorkbenchViewRequest& request)
{
	auto fingerprint = FingerprintPrefix("activate-view", request.operation); AppendField(fingerprint, request.viewId);
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.viewId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		const auto meta = m_contributionIndex->views.find(request.viewId);
		if (meta == m_contributionIndex->views.end()) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownView);
		auto& view = m_views.at(request.viewId); auto& container = m_containers.at(view.containerId);
		auto& active = ActiveContainerFor(m_activeContainers, container.location);
		auto& deferred = ActiveContainerFor(m_deferredActiveContainers, container.location);
		const bool alreadyActive = active && *active == view.containerId;
		const bool hasDeferredIntent = deferred.has_value()
			|| m_deferredActiveViews.contains(view.containerId);
		if (container.visible && view.visible && container.activeViewId
			&& *container.activeViewId == request.viewId && alreadyActive && !hasDeferredIntent)
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		if (!view.visible && !meta->second.canToggleVisibility) return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		view.visible = true; container.visible = true; container.activeViewId = request.viewId;
		active = view.containerId;
		deferred.reset();
		m_deferredActiveViews.erase(view.containerId);
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::ViewRevealed,
				.containerId = view.containerId, .viewId = request.viewId },
			{ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = view.containerId },
		};
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::MoveContainer(const MoveWorkbenchContainerRequest& request)
{
	auto fingerprint = FingerprintPrefix("move-container", request.operation); AppendField(fingerprint, request.containerId); AppendUnsigned(fingerprint, static_cast<std::uint8_t>(request.location)); AppendField(fingerprint, std::to_string(request.order));
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.containerId) || !IsValidLocation(request.location)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (!m_contributionIndex->containers.contains(request.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownContainer);
		if (!m_contributionIndex->containers.at(request.containerId).canMove)
			return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		auto& container = m_containers.at(request.containerId);
		if (container.location == request.location && container.order == request.order)
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		const auto formerLocation = container.location;
		const bool wasActive = ActiveContainerFor(m_activeContainers, formerLocation)
			== std::optional<std::string>{ request.containerId };
		container.location = request.location;
		container.order = request.order;
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::ContainerMoved, .containerId = request.containerId },
		};
		if (wasActive && formerLocation != request.location) {
			const auto fallback = FirstVisibleContainer(m_containers, formerLocation);
			ActiveContainerFor(m_activeContainers, formerLocation) = fallback;
			ActiveContainerFor(m_activeContainers, request.location) = request.containerId;
			ActiveContainerFor(m_deferredActiveContainers, formerLocation).reset();
			ActiveContainerFor(m_deferredActiveContainers, request.location).reset();
			if (fallback) changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = *fallback });
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = request.containerId });
		} else if (!ActiveContainerFor(m_activeContainers, request.location) && container.visible) {
			ActiveContainerFor(m_activeContainers, request.location) = request.containerId;
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::ContainerActivated,
				.containerId = request.containerId });
		}
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::MoveView(const MoveWorkbenchViewRequest& request)
{
	auto fingerprint = FingerprintPrefix("move-view", request.operation); AppendField(fingerprint, request.viewId); AppendField(fingerprint, request.targetContainerId); AppendField(fingerprint, std::to_string(request.order));
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!WorkbenchContributionRegistry::IsValidStableId(request.viewId) || !WorkbenchContributionRegistry::IsValidStableId(request.targetContainerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		const auto meta = m_contributionIndex->views.find(request.viewId);
		if (meta == m_contributionIndex->views.end()) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownView);
		if (!m_contributionIndex->containers.contains(request.targetContainerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownContainer);
		if (!meta->second.canMove) return ResultLocked(EWorkbenchLayoutOperationStatus::Unsupported, EWorkbenchLayoutOperationReason::CapabilityNotSupported);
		auto& view = m_views.at(request.viewId);
		if (view.containerId == request.targetContainerId && view.order == request.order) return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		const auto formerContainer = view.containerId;
		m_viewsByContainer.at(formerContainer).erase({ view.order, request.viewId });
		m_viewsByContainer[request.targetContainerId].emplace(request.order, request.viewId);
		view.containerId = request.targetContainerId; view.order = request.order;
		m_deferredViewPlacements.erase(request.viewId);
		auto& prior = m_containers.at(formerContainer);
		if (prior.activeViewId && *prior.activeViewId == request.viewId) prior.activeViewId = WorkbenchLayoutStateMutator::FirstVisibleView(*this, formerContainer);
		auto& target = m_containers.at(request.targetContainerId);
		if (!target.activeViewId && view.visible) target.activeViewId = request.viewId;
		std::vector<WorkbenchLayoutChange> changes{
			{ .kind = EWorkbenchLayoutChangeKind::ViewMoved,
				.containerId = request.targetContainerId, .viewId = request.viewId },
		};
		if (ValidateLiveFocus(m_focus, m_parts, m_containers, m_views, m_activeContainers)
			!= EWorkbenchLayoutOperationReason::None) {
			m_focus = FallbackFocus(m_parts, m_containers, m_views, m_activeContainers);
			changes.push_back({ .kind = EWorkbenchLayoutChangeKind::FocusChanged,
				.partId = m_focus.partId, .containerId = m_focus.containerId, .viewId = m_focus.viewId });
		}
		return CommitLocked(std::move(changes));
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::SetFocus(const SetWorkbenchFocusRequest& request)
{
	auto fingerprint = FingerprintPrefix("focus", request.operation); AppendField(fingerprint, request.focus.partId.value_or("")); AppendField(fingerprint, request.focus.containerId.value_or("")); AppendField(fingerprint, request.focus.viewId.value_or(""));
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!IsValidFocus(request.focus)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (request.focus.partId && !m_contributionIndex->parts.contains(*request.focus.partId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownPart);
		if (request.focus.containerId && !m_contributionIndex->containers.contains(*request.focus.containerId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownContainer);
		if (request.focus.viewId && !m_contributionIndex->views.contains(*request.focus.viewId)) return ResultLocked(EWorkbenchLayoutOperationStatus::UnknownId, EWorkbenchLayoutOperationReason::UnknownView);
		const auto liveReason = ValidateLiveFocus(request.focus, m_parts, m_containers, m_views,
			m_activeContainers);
		if (liveReason != EWorkbenchLayoutOperationReason::None)
			return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, liveReason);
		if (m_focus.partId == request.focus.partId
			&& m_focus.containerId == request.focus.containerId
			&& m_focus.viewId == request.focus.viewId && !m_deferredFocus) {
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable,
				EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		}
		m_focus = request.focus;
		m_deferredFocus.reset();
		return CommitLocked({ { .kind = EWorkbenchLayoutChangeKind::FocusChanged, .partId = request.focus.partId, .containerId = request.focus.containerId, .viewId = request.focus.viewId } });
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::SetPanelAlignment(const SetWorkbenchPanelAlignmentRequest& request)
{
	auto fingerprint = FingerprintPrefix("panel-alignment", request.operation); AppendUnsigned(fingerprint, static_cast<std::uint8_t>(request.alignment));
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		if (!IsValidPanelAlignment(request.alignment)) return ResultLocked(EWorkbenchLayoutOperationStatus::Invalid, EWorkbenchLayoutOperationReason::InvalidRequest);
		if (m_panelAlignment == request.alignment) return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		m_panelAlignment = request.alignment;
		return CommitLocked({ { .kind = EWorkbenchLayoutChangeKind::Reconciled } });
	});
}

WorkbenchLayoutOperationResult WorkbenchLayoutStateService::Reconcile(const WorkbenchContributionSnapshot& contributions,
	const ReconcileWorkbenchContributionsRequest& request)
{
	auto fingerprint = FingerprintPrefix("reconcile", request.operation); AppendUnsigned(fingerprint, contributions.revision);
	return WorkbenchLayoutStateMutator::Run(*this, request, std::move(fingerprint), [&]() {
		std::unique_ptr<ContributionIndex> next;
		try { next = MakeContributionIndex(contributions); } catch (...) { return ResultLocked(EWorkbenchLayoutOperationStatus::Failed, EWorkbenchLayoutOperationReason::InternalFailure); }
		auto nextParts = m_parts; auto nextContainers = m_containers; auto nextViews = m_views;
		auto nextDeferredParts = m_deferredParts;
		auto nextDeferredContainers = m_deferredContainers;
		auto nextDeferredViews = m_deferredViews;
		auto nextDeferredViewPlacements = m_deferredViewPlacements;
		auto nextDeferredActiveViews = m_deferredActiveViews;
		auto nextActiveContainers = m_activeContainers;
		auto nextDeferredActiveContainers = m_deferredActiveContainers;
		auto nextDeferredFocus = m_deferredFocus;
		const auto partWasDisposed = [&](const std::string& id) {
			return m_contributionIndex->parts.contains(id) && !next->parts.contains(id);
		};
		const auto containerWasDisposed = [&](const std::string& id) {
			return m_contributionIndex->containers.contains(id) && !next->containers.contains(id);
		};
		const auto viewWasDisposed = [&](const std::string& id) {
			return m_contributionIndex->views.contains(id) && !next->views.contains(id);
		};
		for (auto it = nextDeferredViewPlacements.begin();
			it != nextDeferredViewPlacements.end();) {
			if (viewWasDisposed(it->first))
				it = nextDeferredViewPlacements.erase(it);
			else
				++it;
		}
		for (const auto location : kWorkbenchContainerLocations) {
			auto& deferred = ActiveContainerFor(nextDeferredActiveContainers, location);
			if (deferred && containerWasDisposed(*deferred)) deferred.reset();
		}
		for (auto it = nextDeferredActiveViews.begin(); it != nextDeferredActiveViews.end();) {
			if (containerWasDisposed(it->first) || viewWasDisposed(it->second))
				it = nextDeferredActiveViews.erase(it);
			else
				++it;
		}
		if (nextDeferredFocus
			&& ((nextDeferredFocus->partId && partWasDisposed(*nextDeferredFocus->partId))
				|| (nextDeferredFocus->containerId
					&& containerWasDisposed(*nextDeferredFocus->containerId))
				|| (nextDeferredFocus->viewId && viewWasDisposed(*nextDeferredFocus->viewId)))) {
			nextDeferredFocus.reset();
		}
		// Removing an entry that was live in the previous contribution index is a
		// disposal, not an unregistered memento entry.  Never move it into the
		// deferred maps: a later owner generation must not resurrect stale state.
		for (auto it = nextParts.begin(); it != nextParts.end();) { if (!next->parts.contains(it->first)) it = nextParts.erase(it); else ++it; }
		for (const auto& [id, descriptor] : next->parts) {
			if (!nextParts.contains(id)) {
				if (const auto deferred = nextDeferredParts.find(id); deferred != nextDeferredParts.end()) {
					nextParts.emplace(id, deferred->second);
					nextDeferredParts.erase(deferred);
				} else nextParts.emplace(id, WorkbenchPartState{ .partId = id, .visible = true, .position = descriptor.position });
			}
			else nextParts.at(id).position = descriptor.position;
		}
		for (auto it = nextContainers.begin(); it != nextContainers.end();) { if (!next->containers.contains(it->first)) it = nextContainers.erase(it); else ++it; }
		for (const auto& [id, descriptor] : next->containers) if (!nextContainers.contains(id)) {
			if (const auto deferred = nextDeferredContainers.find(id); deferred != nextDeferredContainers.end()) {
				nextContainers.emplace(id, deferred->second);
				nextDeferredContainers.erase(deferred);
			} else nextContainers.emplace(id,
				WorkbenchViewContainerState{ .containerId = id, .location = descriptor.location, .order = descriptor.order, .visible = !descriptor.hideIfEmpty });
		}
		for (auto it = nextViews.begin(); it != nextViews.end();) { if (!next->views.contains(it->first)) it = nextViews.erase(it); else ++it; }
		for (const auto& [id, descriptor] : next->views) {
			if (!nextViews.contains(id)) {
				if (const auto deferred = nextDeferredViews.find(id); deferred != nextDeferredViews.end()
					&& nextContainers.contains(deferred->second.containerId)) {
					nextViews.emplace(id, deferred->second);
					nextDeferredViews.erase(deferred);
				} else nextViews.emplace(id, WorkbenchViewState{ .viewId = id, .containerId = descriptor.defaultContainerId, .order = descriptor.order, .visible = true });
			}
			if (const auto placement = nextDeferredViewPlacements.find(id);
				placement != nextDeferredViewPlacements.end()
				&& nextContainers.contains(placement->second.containerId)) {
				nextViews.at(id) = placement->second;
				nextDeferredViewPlacements.erase(placement);
			}
			else if (!next->containers.contains(nextViews.at(id).containerId)) { nextViews.at(id).containerId = descriptor.defaultContainerId; nextViews.at(id).order = descriptor.order; }
		}
		std::map<std::string, std::set<std::pair<std::int32_t, std::string>>> nextIndex;
		for (const auto& [id, view] : nextViews) nextIndex[view.containerId].emplace(view.order, id);
		for (auto& [id, container] : nextContainers) {
			if (const auto deferred = nextDeferredActiveViews.find(id); deferred != nextDeferredActiveViews.end()
				&& nextViews.contains(deferred->second)
				&& nextViews.at(deferred->second).containerId == id
				&& nextViews.at(deferred->second).visible) {
				container.activeViewId = deferred->second;
				nextDeferredActiveViews.erase(deferred);
			}
			if (container.activeViewId && (!nextViews.contains(*container.activeViewId)
				|| nextViews.at(*container.activeViewId).containerId != id
				|| !nextViews.at(*container.activeViewId).visible)) {
				// A hydrated memento may select a registered view that remains hidden
				// until its owner or user reveals it.  It is not a disposed target:
				// retain the selection intent for MementoSnapshot() and materialize it
				// only after the view is live and visible.  Runtime visibility changes
				// clear activeViewId before Reconcile(), so this never resurrects a
				// deliberately hidden current selection.
				if (nextViews.contains(*container.activeViewId)
					&& nextViews.at(*container.activeViewId).containerId == id) {
					nextDeferredActiveViews.insert_or_assign(id, *container.activeViewId);
				}
				container.activeViewId.reset();
			}
			if (!container.activeViewId) {
				const auto found = nextIndex.find(id);
				if (found != nextIndex.end()) for (const auto& [order, viewId] : found->second) { (void)order; if (nextViews.at(viewId).visible) { container.activeViewId = viewId; break; } }
			}
		}
		for (const auto location : kWorkbenchContainerLocations) {
			auto& active = ActiveContainerFor(nextActiveContainers, location);
			auto& deferred = ActiveContainerFor(nextDeferredActiveContainers, location);
			if (deferred) {
				const auto found = nextContainers.find(*deferred);
				if (found != nextContainers.end() && found->second.location == location
					&& found->second.visible) {
					active = *deferred;
					deferred.reset();
				}
			}
			if (active) {
				const auto found = nextContainers.find(*active);
				if (found == nextContainers.end() || found->second.location != location
					|| !found->second.visible) {
					active.reset();
				}
			}
			if (!active) active = FirstVisibleContainer(nextContainers, location);
		}
		WorkbenchFocusState nextFocus = m_focus;
		const auto isLiveFocus = [&](const WorkbenchFocusState& focus) {
			return ValidateLiveFocus(focus, nextParts, nextContainers, nextViews,
				nextActiveContainers) == EWorkbenchLayoutOperationReason::None;
		};
		if (!isLiveFocus(nextFocus)) {
			nextFocus = FallbackFocus(nextParts, nextContainers, nextViews, nextActiveContainers);
		}
		if (nextDeferredFocus && isLiveFocus(*nextDeferredFocus)) {
			nextFocus = *nextDeferredFocus;
			nextDeferredFocus.reset();
		}
		const bool unchanged = nextParts == m_parts && nextContainers == m_containers && nextViews == m_views
			&& nextDeferredParts == m_deferredParts && nextDeferredContainers == m_deferredContainers
			&& nextDeferredViews == m_deferredViews
			&& nextDeferredViewPlacements == m_deferredViewPlacements
			&& nextDeferredActiveViews == m_deferredActiveViews
			&& nextActiveContainers == m_activeContainers
			&& nextDeferredActiveContainers == m_deferredActiveContainers
			&& nextDeferredFocus == m_deferredFocus && nextFocus == m_focus;
		if (unchanged) {
			m_contributionIndex = std::move(next);
			return ResultLocked(EWorkbenchLayoutOperationStatus::NotApplicable, EWorkbenchLayoutOperationReason::AlreadyInRequestedState);
		}
		m_parts = std::move(nextParts); m_containers = std::move(nextContainers); m_views = std::move(nextViews); m_viewsByContainer = std::move(nextIndex); m_focus = std::move(nextFocus);
		m_deferredParts = std::move(nextDeferredParts); m_deferredContainers = std::move(nextDeferredContainers); m_deferredViews = std::move(nextDeferredViews);
		m_deferredViewPlacements = std::move(nextDeferredViewPlacements);
		m_deferredActiveViews = std::move(nextDeferredActiveViews);
		m_activeContainers = std::move(nextActiveContainers);
		m_deferredActiveContainers = std::move(nextDeferredActiveContainers);
		m_deferredFocus = std::move(nextDeferredFocus);
		auto result = CommitLocked({ { .kind = EWorkbenchLayoutChangeKind::Reconciled } });
		m_contributionIndex = std::move(next);
		return result;
	});
}

std::unique_ptr<IWorkbenchLayoutSubscription> WorkbenchLayoutStateService::Subscribe(WorkbenchLayoutChangeCallback callback)
{
	if (!callback) return nullptr;
	try {
		std::scoped_lock lock(m_subscriptionState->mutex);
		if (m_subscriptionState->closed || m_subscriptionState->nextId == 0) return nullptr;
		const auto id = m_subscriptionState->nextId++;
		m_subscriptionState->slots.emplace(id, std::make_shared<SubscriptionSlot>(std::move(callback)));
		return std::make_unique<WorkbenchLayoutSubscription>(m_subscriptionState, id);
	} catch (...) { return nullptr; }
}

} // namespace workbench::layout
