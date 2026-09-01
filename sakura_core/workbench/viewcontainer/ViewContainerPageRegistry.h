/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/viewcontainer/IViewContainerPage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::viewcontainer {

using ViewContainerPageFactory = std::function<std::unique_ptr<IViewContainerPage>()>;

struct ViewContainerPageDescriptor final {
	std::string containerId;
	layout::SupportedViewContainerLocations supportedLocations;
	ViewContainerPageFactory factory;
};

//! Product-owned implementation of one declarative host View provider. The
//! provider id is package-visible; the factory remains process-local authority.
struct HostViewProviderDescriptor final {
	std::string id;
	ViewContainerPageFactory factory;
};

enum class EHostViewPageProjectionStatus : std::uint8_t {
	Projected,
	NotApplicable,
	InvalidContribution,
	InvalidProvider,
	UnknownProvider,
	DuplicateContainerId,
	Failed,
};

struct HostViewPageProjectionResult final {
	EHostViewPageProjectionStatus status{ EHostViewPageProjectionStatus::Failed };
	std::vector<ViewContainerPageDescriptor> descriptors;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EHostViewPageProjectionStatus::Projected
			|| status == EHostViewPageProjectionStatus::NotApplicable;
	}
};

//! Resolves declarative View provider ids to native page factories without
//! granting the package HWND or process authority. One native page represents
//! one ViewContainer; multiple host Views in the same container fail closed.
[[nodiscard]] HostViewPageProjectionResult ProjectHostViewPages(
	const layout::WorkbenchContributionSnapshot& snapshot,
	std::span<const HostViewProviderDescriptor> providers) noexcept;

enum class EViewContainerPageRegistrationStatus : std::uint8_t {
	Registered,
	NotApplicable,
	InvalidDescriptor,
	DuplicateContainerId,
	Failed,
};

struct ViewContainerPageRegistrationResult final {
	EViewContainerPageRegistrationStatus status{ EViewContainerPageRegistrationStatus::Failed };
	std::size_t registeredCount{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EViewContainerPageRegistrationStatus::Registered
			|| status == EViewContainerPageRegistrationStatus::NotApplicable;
	}
};

//! Native page factories are kept beside the native page contract, not in the
//! HWND-independent WorkbenchContributionRegistry.
class ViewContainerPageRegistry final {
public:
	//! Validates and copies the complete candidate registry before publishing it.
	[[nodiscard]] ViewContainerPageRegistrationResult RegisterBatch(
		std::vector<ViewContainerPageDescriptor> descriptors) noexcept;
	[[nodiscard]] const ViewContainerPageDescriptor* Find(
		std::string_view containerId) const noexcept;
	[[nodiscard]] std::size_t Size() const noexcept { return m_descriptors.size(); }

private:
	using DescriptorMap = std::map<std::string, ViewContainerPageDescriptor, std::less<>>;
	DescriptorMap m_descriptors;
};

} // namespace workbench::viewcontainer
