/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/viewcontainer/ViewContainerPageRegistry.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace workbench::viewcontainer {

HostViewPageProjectionResult ProjectHostViewPages(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const std::span<const HostViewProviderDescriptor> providers) noexcept
{
	if (!layout::WorkbenchContributionRegistry::IsValidContributionSnapshot(snapshot)) {
		return { EHostViewPageProjectionStatus::InvalidContribution, {} };
	}
	try {
		std::unordered_set<std::string_view> providerIds;
		providerIds.reserve(providers.size());
		for (const auto& provider : providers) {
			if (!layout::WorkbenchContributionRegistry::IsValidStableId(provider.id)
				|| !provider.factory || !providerIds.emplace(provider.id).second) {
				return { EHostViewPageProjectionStatus::InvalidProvider, {} };
			}
		}

		HostViewPageProjectionResult result{ EHostViewPageProjectionStatus::NotApplicable, {} };
		std::unordered_set<std::string_view> projectedContainers;
		for (const auto& registeredView : snapshot.views) {
			const auto& view = registeredView.descriptor;
			if (view.provider.empty()) continue;
			const auto provider = std::ranges::find(providers, view.provider,
				&HostViewProviderDescriptor::id);
			if (provider == providers.end()) {
				return { EHostViewPageProjectionStatus::UnknownProvider, {} };
			}
			const auto container = std::ranges::find(snapshot.viewContainers,
				view.containerId, [](const auto& entry) -> const std::string& {
					return entry.descriptor.id;
				});
			if (container == snapshot.viewContainers.end()) {
				return { EHostViewPageProjectionStatus::InvalidContribution, {} };
			}
			if (!projectedContainers.emplace(container->descriptor.id).second) {
				return { EHostViewPageProjectionStatus::DuplicateContainerId, {} };
			}
			result.descriptors.push_back({
				.containerId = container->descriptor.id,
				.supportedLocations = container->descriptor.supportedLocations,
				.factory = provider->factory,
			});
			result.status = EHostViewPageProjectionStatus::Projected;
		}
		return result;
	} catch (...) {
		return { EHostViewPageProjectionStatus::Failed, {} };
	}
}

ViewContainerPageRegistrationResult ViewContainerPageRegistry::RegisterBatch(
	std::vector<ViewContainerPageDescriptor> descriptors) noexcept
{
	if (descriptors.empty()) {
		return { EViewContainerPageRegistrationStatus::NotApplicable, 0 };
	}
	try {
		std::unordered_set<std::string_view> batchIds;
		batchIds.reserve(descriptors.size());
		for (const auto& descriptor : descriptors) {
			if (!layout::WorkbenchContributionRegistry::IsValidStableId(descriptor.containerId)
				|| !descriptor.supportedLocations.IsValid() || !descriptor.factory) {
				return { EViewContainerPageRegistrationStatus::InvalidDescriptor, 0 };
			}
			if (m_descriptors.contains(descriptor.containerId)
				|| !batchIds.emplace(descriptor.containerId).second) {
				return { EViewContainerPageRegistrationStatus::DuplicateContainerId, 0 };
			}
		}

		// Publish by swap so allocation/copy failure can never expose a partial batch.
		auto candidate = m_descriptors;
		for (auto& descriptor : descriptors) {
			auto key = descriptor.containerId;
			candidate.emplace(std::move(key), std::move(descriptor));
		}
		m_descriptors.swap(candidate);
		return { EViewContainerPageRegistrationStatus::Registered, descriptors.size() };
	} catch (...) {
		return { EViewContainerPageRegistrationStatus::Failed, 0 };
	}
}

const ViewContainerPageDescriptor* ViewContainerPageRegistry::Find(
	const std::string_view containerId) const noexcept
{
	try {
		const auto found = m_descriptors.find(containerId);
		return found != m_descriptors.end() ? &found->second : nullptr;
	} catch (...) {
		return nullptr;
	}
}

} // namespace workbench::viewcontainer
