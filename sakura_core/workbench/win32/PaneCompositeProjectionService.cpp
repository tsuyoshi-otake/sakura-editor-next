/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/win32/PaneCompositeProjectionService.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <set>
#include <utility>

namespace workbench::win32 {
namespace {

using Location = layout::EWorkbenchViewContainerLocation;

constexpr std::array<Location, kPaneCompositeHostCount> kLocations{
	Location::SideBar,
	Location::Panel,
	Location::AuxiliaryBar,
};

[[nodiscard]] constexpr std::size_t LocationIndex(const Location location) noexcept
{
	switch (location) {
	case Location::SideBar: return 0;
	case Location::Panel: return 1;
	case Location::AuxiliaryBar: return 2;
	}
	return kPaneCompositeHostCount;
}

[[nodiscard]] constexpr std::string_view PartIdForLocation(const Location location) noexcept
{
	switch (location) {
	case Location::SideBar: return layout::ids::part::Sidebar;
	case Location::Panel: return layout::ids::part::Panel;
	case Location::AuxiliaryBar: return layout::ids::part::Auxiliarybar;
	}
	return {};
}

[[nodiscard]] constexpr layout::EWorkbenchPartPosition PartPositionForLocation(
	const Location location) noexcept
{
	switch (location) {
	case Location::SideBar: return layout::EWorkbenchPartPosition::Left;
	case Location::Panel: return layout::EWorkbenchPartPosition::Bottom;
	case Location::AuxiliaryBar: return layout::EWorkbenchPartPosition::Right;
	}
	return layout::EWorkbenchPartPosition::Center;
}

[[nodiscard]] const std::optional<std::string>& ActiveContainerForLocation(
	const layout::WorkbenchActiveContainerState& state, const Location location) noexcept
{
	switch (location) {
	case Location::SideBar: return state.sideBar;
	case Location::Panel: return state.panel;
	case Location::AuxiliaryBar: return state.auxiliaryBar;
	}
	return state.sideBar;
}

[[nodiscard]] bool IsValidExtent(const std::optional<std::uint32_t>& extent) noexcept
{
	return !extent || (*extent > 0
		&& *extent <= layout::kMaximumWorkbenchLayoutCommittedExtentDip);
}

} // namespace

bool PaneCompositeSupportedLocationsResult::Contains(const Location location) const noexcept
{
	return std::ranges::find(locations, location) != locations.end();
}

class PaneCompositeProjectionService::Impl final {
public:
	explicit Impl(std::array<PaneCompositeHostBinding, kPaneCompositeHostCount> bindings)
		: m_bindings(std::move(bindings))
	{
	}

	struct PreparedState final {
		PaneCompositePreparationToken token;
		PaneCompositeProjection projection;
		std::array<PaneCompositeHostState, kPaneCompositeHostCount> previous;
	};

	[[nodiscard]] bool BindingsValid() const noexcept
	{
		std::array<bool, kPaneCompositeHostCount> seen{};
		for (const auto& binding : m_bindings) {
			const auto index = LocationIndex(binding.location);
			if (index >= seen.size() || seen[index] || !binding.supportsContainer
				|| !binding.canApply || !binding.readState || !binding.applyState
				|| !binding.closeProjection) {
				return false;
			}
			seen[index] = true;
		}
		return std::ranges::all_of(seen, [](const bool value) { return value; });
	}

	[[nodiscard]] const PaneCompositeHostBinding* BindingFor(const Location location) const noexcept
	{
		const auto found = std::ranges::find(m_bindings, location,
			&PaneCompositeHostBinding::location);
		return found == m_bindings.end() ? nullptr : &*found;
	}

	[[nodiscard]] PaneCompositeHostBinding* BindingFor(const Location location) noexcept
	{
		const auto found = std::ranges::find(m_bindings, location,
			&PaneCompositeHostBinding::location);
		return found == m_bindings.end() ? nullptr : &*found;
	}

	[[nodiscard]] std::array<std::optional<PaneCompositeHostState>, kPaneCompositeHostCount>
		ReadStates() const noexcept
	{
		std::array<std::optional<PaneCompositeHostState>, kPaneCompositeHostCount> states;
		for (const auto location : kLocations) {
			const auto* binding = BindingFor(location);
			if (binding == nullptr) continue;
			try {
				states[LocationIndex(location)] = binding->readState();
			} catch (...) {
				states[LocationIndex(location)].reset();
			}
		}
		return states;
	}

	[[nodiscard]] static std::optional<Location> FirstStateMismatch(
		const std::array<std::optional<PaneCompositeHostState>, kPaneCompositeHostCount>& actual,
		const std::array<PaneCompositeHostState, kPaneCompositeHostCount>& expected) noexcept
	{
		for (const auto location : kLocations) {
			const auto index = LocationIndex(location);
			if (!actual[index] || *actual[index] != expected[index]) return location;
		}
		return std::nullopt;
	}

	[[nodiscard]] PaneCompositePrepareResult Prepare(
		const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept
	{
		PaneCompositePrepareResult result;
		result.finalStates = ReadStates();
		if (m_closed) {
			result.status = EPaneCompositePrepareStatus::Closed;
			return result;
		}
		if (m_faulted) {
			result.status = EPaneCompositePrepareStatus::Faulted;
			return result;
		}
		if (m_prepared) {
			result.status = EPaneCompositePrepareStatus::Busy;
			return result;
		}
		if (!BindingsValid()) {
			result.status = EPaneCompositePrepareStatus::HostUnavailable;
			return result;
		}
		try {
			if (snapshot.schemaVersion != layout::kWorkbenchLayoutStateSchemaVersion
				|| snapshot.generation == 0) {
				result.status = EPaneCompositePrepareStatus::InvalidSnapshot;
				return result;
			}

			PreparedState prepared;
			prepared.projection.generation = snapshot.generation;
			prepared.projection.revision = snapshot.revision;
			std::set<std::string, std::less<>> activeIds;

			for (const auto location : kLocations) {
				const auto index = LocationIndex(location);
				const auto partId = PartIdForLocation(location);
				const auto first = std::ranges::find(snapshot.parts, partId,
					&layout::WorkbenchPartState::partId);
				if (first == snapshot.parts.end()) {
					result.status = EPaneCompositePrepareStatus::MissingRequiredPart;
					result.failedLocation = location;
					return result;
				}
				if (std::ranges::count(snapshot.parts, partId,
					&layout::WorkbenchPartState::partId) != 1) {
					result.status = EPaneCompositePrepareStatus::DuplicateRequiredPart;
					result.failedLocation = location;
					return result;
				}
				if (first->position != PartPositionForLocation(location)) {
					result.status = EPaneCompositePrepareStatus::UnsupportedPartPosition;
					result.failedLocation = location;
					return result;
				}
				if (!IsValidExtent(first->committedExtentDip)) {
					result.status = EPaneCompositePrepareStatus::InvalidExtent;
					result.failedLocation = location;
					return result;
				}

				auto& desired = prepared.projection.hosts[index];
				desired.location = location;
				desired.partId = std::string(partId);
				desired.visible = first->visible;
				desired.committedExtentDip = first->committedExtentDip;

				const auto& activeId = ActiveContainerForLocation(snapshot.activeContainers, location);
				if (activeId) {
					if (!activeIds.emplace(*activeId).second) {
						result.status = EPaneCompositePrepareStatus::DuplicateActiveContainer;
						result.failedLocation = location;
						return result;
					}
					const auto container = std::ranges::find(snapshot.containers, *activeId,
						&layout::WorkbenchViewContainerState::containerId);
					if (container == snapshot.containers.end() || !container->visible
						|| container->location != location
						|| std::ranges::count(snapshot.containers, *activeId,
							&layout::WorkbenchViewContainerState::containerId) != 1) {
						result.status = EPaneCompositePrepareStatus::InvalidActiveContainer;
						result.failedLocation = location;
						return result;
					}
					const auto* binding = BindingFor(location);
					bool supported = false;
					try {
						supported = binding != nullptr && binding->supportsContainer(*activeId);
					} catch (...) {
						supported = false;
					}
					if (!supported) {
						result.status = EPaneCompositePrepareStatus::UnsupportedContainerLocation;
						result.failedLocation = location;
						return result;
					}
					desired.activeContainerId = *activeId;
					desired.attachedContainerId = *activeId;
				}

				const auto& current = result.finalStates[index];
				if (!current || current->location != location
					|| current->partId != partId) {
					result.status = EPaneCompositePrepareStatus::HostUnavailable;
					result.failedLocation = location;
					return result;
				}
				prepared.previous[index] = *current;
				// An omitted logical extent means that this transaction does not request
				// an extent change. Native hosts still expose their initialized positive
				// extent, so compare the final frame against that observed value instead
				// of treating absence as an observable native value.
				if (!desired.committedExtentDip) {
					desired.committedExtentDip = current->committedExtentDip;
				}
				const auto* binding = BindingFor(location);
				bool canApply = false;
				try {
					canApply = binding != nullptr && binding->canApply(desired);
				} catch (...) {
					canApply = false;
				}
				if (!canApply) {
					result.status = EPaneCompositePrepareStatus::HostPrepareFailed;
					result.failedLocation = location;
					return result;
				}
			}

			if (!ValidateFocus(snapshot, prepared.projection)) {
				result.status = EPaneCompositePrepareStatus::InvalidFocus;
				return result;
			}
			if (m_nextToken == std::numeric_limits<std::uint64_t>::max()) {
				result.status = EPaneCompositePrepareStatus::InternalFailure;
				return result;
			}
			prepared.token.value = ++m_nextToken;
			result.status = EPaneCompositePrepareStatus::Prepared;
			result.token = prepared.token;
			m_prepared.emplace(std::move(prepared));
			return result;
		} catch (...) {
			result.status = EPaneCompositePrepareStatus::InternalFailure;
			return result;
		}
	}

	[[nodiscard]] bool ValidateFocus(const layout::WorkbenchLayoutStateSnapshot& snapshot,
		PaneCompositeProjection& projection) const
	{
		const auto& focus = snapshot.focus;
		if (!focus.partId && !focus.containerId && !focus.viewId) return true;
		if (focus.partId == layout::ids::part::Editor && !focus.containerId && !focus.viewId) {
			projection.focus = PaneCompositeFocusProjection{
				.partId = std::string(layout::ids::part::Editor) };
			return true;
		}

		std::optional<Location> location;
		const layout::WorkbenchViewContainerState* container = nullptr;
		if (focus.containerId) {
			const auto found = std::ranges::find(snapshot.containers, *focus.containerId,
				&layout::WorkbenchViewContainerState::containerId);
			if (found == snapshot.containers.end() || !found->visible
				|| std::ranges::count(snapshot.containers, *focus.containerId,
					&layout::WorkbenchViewContainerState::containerId) != 1) {
				return false;
			}
			container = &*found;
			location = found->location;
			const auto index = LocationIndex(*location);
			if (index >= projection.hosts.size()
				|| projection.hosts[index].activeContainerId != focus.containerId
				|| !projection.hosts[index].visible) {
				return false;
			}
		}
		if (!location && focus.partId) {
			for (const auto candidate : kLocations) {
				if (*focus.partId == PartIdForLocation(candidate)) {
					location = candidate;
					break;
				}
			}
		}
		if (!location || (focus.partId && *focus.partId != PartIdForLocation(*location))) {
			return false;
		}
		const auto index = LocationIndex(*location);
		if (!projection.hosts[index].visible) return false;
		if (focus.viewId) {
			if (container == nullptr) return false;
			const auto view = std::ranges::find(snapshot.views, *focus.viewId,
				&layout::WorkbenchViewState::viewId);
			if (view == snapshot.views.end() || !view->visible
				|| view->containerId != container->containerId
				|| container->activeViewId != focus.viewId
				|| std::ranges::count(snapshot.views, *focus.viewId,
					&layout::WorkbenchViewState::viewId) != 1) {
				return false;
			}
		}
		projection.focus = PaneCompositeFocusProjection{
			.partId = focus.partId ? *focus.partId : std::string(PartIdForLocation(*location)),
			.location = location,
			.containerId = focus.containerId,
			.viewId = focus.viewId,
		};
		return true;
	}

	[[nodiscard]] PaneCompositeCommitResult Commit(
		const PaneCompositePreparationToken token) noexcept
	{
		PaneCompositeCommitResult result;
		result.finalStates = ReadStates();
		if (m_closed) {
			result.status = EPaneCompositeCommitStatus::Closed;
			return result;
		}
		if (m_faulted) {
			result.status = EPaneCompositeCommitStatus::Faulted;
			return result;
		}
		if (!m_prepared || m_prepared->token != token) {
			result.status = EPaneCompositeCommitStatus::StalePreparation;
			return result;
		}

		try {
			std::optional<Location> failed;
			bool verificationFailed = false;
			for (const auto location : kLocations) {
				auto* binding = BindingFor(location);
				const auto& desired = m_prepared->projection.hosts[LocationIndex(location)];
				EPaneCompositeHostApplyStatus applied = EPaneCompositeHostApplyStatus::Failed;
				try {
					applied = binding != nullptr ? binding->applyState(desired)
						: EPaneCompositeHostApplyStatus::Failed;
				} catch (...) {
					applied = EPaneCompositeHostApplyStatus::Failed;
				}
				if (applied == EPaneCompositeHostApplyStatus::Failed) {
					failed = location;
					break;
				}
			}
			if (!failed) {
				result.finalStates = ReadStates();
				std::array<PaneCompositeHostState, kPaneCompositeHostCount> desired;
				for (const auto location : kLocations) {
					desired[LocationIndex(location)] =
						m_prepared->projection.hosts[LocationIndex(location)];
				}
				failed = FirstStateMismatch(result.finalStates, desired);
				verificationFailed = failed.has_value();
				if (!failed) {
					m_lastCommitted = std::move(m_prepared->projection);
					m_prepared.reset();
					result.status = EPaneCompositeCommitStatus::Committed;
					return result;
				}
			}

			result.failedLocation = failed;
			// A shared page pool can change an unvisited host's physical owner while
			// attaching another host. Restore every staged owner, in reverse order.
			for (auto iterator = kLocations.rbegin(); iterator != kLocations.rend(); ++iterator) {
				auto* binding = BindingFor(*iterator);
				const auto& previous = m_prepared->previous[LocationIndex(*iterator)];
				EPaneCompositeHostApplyStatus restored = EPaneCompositeHostApplyStatus::Failed;
				try {
					restored = binding != nullptr ? binding->applyState(previous)
						: EPaneCompositeHostApplyStatus::Failed;
				} catch (...) {
					restored = EPaneCompositeHostApplyStatus::Failed;
				}
				if (restored == EPaneCompositeHostApplyStatus::Failed
					&& !result.compensationFailedLocation) {
					result.compensationFailedLocation = *iterator;
				}
			}
			result.finalStates = ReadStates();
			if (const auto mismatch = FirstStateMismatch(
				result.finalStates, m_prepared->previous);
				mismatch && !result.compensationFailedLocation) {
				result.compensationFailedLocation = mismatch;
			}
			m_prepared.reset();
			if (result.compensationFailedLocation) {
				m_faulted = true;
				result.status = EPaneCompositeCommitStatus::CompensationFailed;
			} else {
				result.status = verificationFailed
					? EPaneCompositeCommitStatus::HostVerificationFailedCompensated
					: EPaneCompositeCommitStatus::HostCommitFailedCompensated;
			}
			return result;
		} catch (...) {
			m_faulted = true;
			m_prepared.reset();
			result.status = EPaneCompositeCommitStatus::InternalFailure;
			result.finalStates = ReadStates();
			return result;
		}
	}

	[[nodiscard]] EPaneCompositeCancelStatus Cancel(
		const PaneCompositePreparationToken token) noexcept
	{
		if (m_closed) return EPaneCompositeCancelStatus::Closed;
		if (!m_prepared || m_prepared->token != token) {
			return EPaneCompositeCancelStatus::StalePreparation;
		}
		m_prepared.reset();
		return EPaneCompositeCancelStatus::Cancelled;
	}

	[[nodiscard]] PaneCompositeSupportedLocationsResult SupportedLocations(
		const std::string_view containerId) const noexcept
	{
		PaneCompositeSupportedLocationsResult result;
		if (m_closed || m_faulted || containerId.empty() || !BindingsValid()) return result;
		try {
			result.locations.reserve(kPaneCompositeHostCount);
			for (const auto location : kLocations) {
				const auto* binding = BindingFor(location);
				if (binding != nullptr && binding->supportsContainer(containerId)) {
					result.locations.push_back(location);
				}
			}
			result.complete = true;
		} catch (...) {
			result.locations.clear();
			result.complete = false;
		}
		return result;
	}

	[[nodiscard]] EPaneCompositeCloseStatus Close() noexcept
	{
		if (m_closed) return EPaneCompositeCloseStatus::AlreadyClosed;
		m_closed = true;
		m_prepared.reset();
		bool failed = false;
		for (auto iterator = kLocations.rbegin(); iterator != kLocations.rend(); ++iterator) {
			auto* binding = BindingFor(*iterator);
			try {
				if (binding == nullptr || !binding->closeProjection()) failed = true;
			} catch (...) {
				failed = true;
			}
		}
		return failed ? EPaneCompositeCloseStatus::HostCloseFailed
			: EPaneCompositeCloseStatus::Closed;
	}

	std::array<PaneCompositeHostBinding, kPaneCompositeHostCount> m_bindings;
	std::optional<PreparedState> m_prepared;
	std::optional<PaneCompositeProjection> m_lastCommitted;
	std::uint64_t m_nextToken{};
	bool m_faulted{};
	bool m_closed{};
};

PaneCompositeProjectionService::PaneCompositeProjectionService(
	std::array<PaneCompositeHostBinding, kPaneCompositeHostCount> bindings)
	: m_impl(std::make_unique<Impl>(std::move(bindings)))
{
}

PaneCompositeProjectionService::~PaneCompositeProjectionService()
{
	if (m_impl) (void)m_impl->Close();
}

PaneCompositePrepareResult PaneCompositeProjectionService::Prepare(
	const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept
{
	return m_impl ? m_impl->Prepare(snapshot) : PaneCompositePrepareResult{};
}

PaneCompositeCommitResult PaneCompositeProjectionService::Commit(
	const PaneCompositePreparationToken token) noexcept
{
	return m_impl ? m_impl->Commit(token) : PaneCompositeCommitResult{};
}

EPaneCompositeCancelStatus PaneCompositeProjectionService::Cancel(
	const PaneCompositePreparationToken token) noexcept
{
	return m_impl ? m_impl->Cancel(token) : EPaneCompositeCancelStatus::Closed;
}

PaneCompositeSupportedLocationsResult PaneCompositeProjectionService::SupportedLocations(
	const std::string_view containerId) const noexcept
{
	return m_impl ? m_impl->SupportedLocations(containerId)
		: PaneCompositeSupportedLocationsResult{};
}

const PaneCompositeProjection* PaneCompositeProjectionService::LastCommittedProjection() const noexcept
{
	return m_impl && m_impl->m_lastCommitted ? &*m_impl->m_lastCommitted : nullptr;
}

EPaneCompositeCloseStatus PaneCompositeProjectionService::Close() noexcept
{
	return m_impl ? m_impl->Close() : EPaneCompositeCloseStatus::AlreadyClosed;
}

} // namespace workbench::win32
