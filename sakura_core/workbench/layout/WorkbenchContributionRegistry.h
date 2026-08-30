/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::layout {

enum class EViewContainerLocation : std::uint8_t {
	Sidebar,
	Panel,
	AuxiliaryBar,
};

//! Bounded destination contract for one ViewContainer contribution.
class SupportedViewContainerLocations final {
public:
	constexpr SupportedViewContainerLocations() noexcept = default;
	constexpr SupportedViewContainerLocations(
		std::initializer_list<EViewContainerLocation> locations) noexcept
	{
		for (const auto location : locations) {
			const auto bit = BitFor(location);
			if (bit == 0) m_bits |= kInvalidBit;
			else m_bits |= bit;
		}
	}

	[[nodiscard]] constexpr bool Contains(EViewContainerLocation location) const noexcept
	{
		const auto bit = BitFor(location);
		return bit != 0 && (m_bits & kInvalidBit) == 0 && (m_bits & bit) != 0;
	}
	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return (m_bits & kInvalidBit) == 0 && (m_bits & kLocationBits) != 0;
	}

private:
	static constexpr std::uint8_t kSidebarBit = 1U << 0;
	static constexpr std::uint8_t kPanelBit = 1U << 1;
	static constexpr std::uint8_t kAuxiliaryBarBit = 1U << 2;
	static constexpr std::uint8_t kLocationBits = kSidebarBit | kPanelBit | kAuxiliaryBarBit;
	static constexpr std::uint8_t kInvalidBit = 1U << 7;

	[[nodiscard]] static constexpr std::uint8_t BitFor(EViewContainerLocation location) noexcept
	{
		switch (location) {
		case EViewContainerLocation::Sidebar: return kSidebarBit;
		case EViewContainerLocation::Panel: return kPanelBit;
		case EViewContainerLocation::AuxiliaryBar: return kAuxiliaryBarBit;
		}
		return 0;
	}

	std::uint8_t m_bits{};
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
	SupportedViewContainerLocations supportedLocations;
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
	[[nodiscard]] static bool IsValidViewContainerDescriptor(
		const WorkbenchViewContainerDescriptor& descriptor) noexcept;
	//! Validates the complete batch before any consumer can register a partial index.
	[[nodiscard]] static bool IsValidContributionSnapshot(
		const WorkbenchContributionSnapshot& snapshot) noexcept;

private:
	WorkbenchContributionSnapshot m_snapshot;
};

} // namespace workbench::layout
