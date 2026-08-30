/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/viewcontainer/IViewContainerPage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
