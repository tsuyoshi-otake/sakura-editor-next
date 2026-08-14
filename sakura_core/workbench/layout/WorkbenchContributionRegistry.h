/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::layout {

enum class EViewContainerLocation : std::uint8_t {
	Sidebar,
	Panel,
	AuxiliaryBar,
};

struct WorkbenchPartDescriptor {
	std::string id;
	std::string title;
	bool supportsVisibility{ true };
};

struct WorkbenchViewContainerDescriptor {
	std::string id;
	std::string title;
	EViewContainerLocation location{ EViewContainerLocation::Sidebar };
	std::int32_t order{};
	bool hideIfEmpty{};
	bool canMove{ true };
};

struct WorkbenchViewDescriptor {
	std::string id;
	std::string containerId;
	std::string title;
	std::int32_t order{};
	bool canToggleVisibility{ true };
	bool canMove{ true };
};

struct RegisteredWorkbenchPart { WorkbenchPartDescriptor descriptor; };
struct RegisteredWorkbenchViewContainer { WorkbenchViewContainerDescriptor descriptor; };
struct RegisteredWorkbenchView { WorkbenchViewDescriptor descriptor; };

//! Deterministic, ID-sorted declarations for the built-in workbench.
struct WorkbenchContributionSnapshot {
	std::uint64_t revision{ 1 };
	std::vector<RegisteredWorkbenchPart> parts;
	std::vector<RegisteredWorkbenchViewContainer> viewContainers;
	std::vector<RegisteredWorkbenchView> views;
};

//! Immutable registry for Sakura Editor NEXT's built-in Parts, ViewContainers and Views.
class WorkbenchContributionRegistry final {
public:
	WorkbenchContributionRegistry();

	[[nodiscard]] WorkbenchContributionSnapshot Snapshot() const { return m_snapshot; }
	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;

private:
	WorkbenchContributionSnapshot m_snapshot;
};

} // namespace workbench::layout
