/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <memory>
#include <initializer_list>
#include <span>
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
	[[nodiscard]] constexpr bool operator==(const SupportedViewContainerLocations&) const = default;

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
	[[nodiscard]] bool operator==(const WorkbenchPartDescriptor&) const = default;
};

struct WorkbenchViewContainerDescriptor {
	std::string id;
	std::string title;
	EViewContainerLocation location{ EViewContainerLocation::Sidebar };
	std::int32_t order{};
	std::string icon;
	bool hideIfEmpty{};
	SupportedViewContainerLocations supportedLocations;
	[[nodiscard]] bool operator==(const WorkbenchViewContainerDescriptor&) const = default;
};

struct WorkbenchViewDescriptor {
	std::string id;
	std::string containerId;
	std::string title;
	std::int32_t order{};
	bool canToggleVisibility{ true };
	bool canMove{ true };
	//! Empty for product-owned built-ins. Extension-contributed native Views use
	//! a stable host provider id that the page projection resolves.
	std::string provider;
	[[nodiscard]] bool operator==(const WorkbenchViewDescriptor&) const = default;
};

struct RegisteredWorkbenchPart { WorkbenchPartDescriptor descriptor; };
//! A native-issued, process-local identity. Empty/zero denotes a built-in.
//! Generations never enter a durable layout memento.
struct WorkbenchContributionOwner {
	std::string ownerId;
	std::uint64_t generation{};
	[[nodiscard]] bool operator==(const WorkbenchContributionOwner&) const = default;
};

struct RegisteredWorkbenchViewContainer {
	WorkbenchViewContainerDescriptor descriptor;
	WorkbenchContributionOwner owner;
};
struct RegisteredWorkbenchView {
	WorkbenchViewDescriptor descriptor;
	WorkbenchContributionOwner owner;
};

//! Deterministic, ID-sorted declarations for the built-in workbench.
struct WorkbenchContributionSnapshot {
	std::uint64_t revision{ 1 };
	std::vector<RegisteredWorkbenchPart> parts;
	std::vector<RegisteredWorkbenchViewContainer> viewContainers;
	std::vector<RegisteredWorkbenchView> views;
	std::vector<WorkbenchContributionOwner> owners;
};

enum class EWorkbenchContributionChangeStatus : std::uint8_t {
	Prepared, Committed, Invalid, Conflict, Exhausted, Failed, Unsupported,
};

class WorkbenchContributionRegistry;

//! An unpublished catalog candidate. Destruction aborts preparation. Consumers
//! may validate native page/layout candidates against its immutable snapshot.
class PreparedWorkbenchContributions final {
public:
	[[nodiscard]] const WorkbenchContributionSnapshot& Snapshot() const noexcept { return m_snapshot; }
private:
	friend class WorkbenchContributionRegistry;
	WorkbenchContributionSnapshot m_snapshot;
	std::uint64_t m_baseRevision{};
	std::uint64_t m_lastGeneration{};
	const WorkbenchContributionRegistry* m_registry{};
};

struct PrepareWorkbenchContributionsResult {
	EWorkbenchContributionChangeStatus status{ EWorkbenchContributionChangeStatus::Failed };
	std::unique_ptr<PreparedWorkbenchContributions> change;
};

//! Catalog access and publication are serialized by the native composition
//! owner. Preparation does not mutate the live catalog or invoke callbacks.
class WorkbenchContributionRegistry final {
public:
	WorkbenchContributionRegistry();

	[[nodiscard]] WorkbenchContributionSnapshot Snapshot() const { return m_snapshot; }
	//! Non-reserving suggestion for serialized composition. Zero means exhausted;
	//! preparing and committing still enforce the catalog revision fence.
	[[nodiscard]] std::uint64_t NextOwnerGeneration() const noexcept;
	//! Atomically appends one validated startup batch. Runtime registration after
	//! native page creation is deliberately unsupported by this checkpoint.
	[[nodiscard]] bool RegisterExtensionContributions(
		std::span<const WorkbenchViewContainerDescriptor> containers,
		std::span<const WorkbenchViewDescriptor> views);
	//! expectedGeneration zero means the owner must be absent. A replacement
	//! generation must exceed every generation previously committed here, even
	//! after disposal. Parallel preparations may conflict and are never retried.
	[[nodiscard]] PrepareWorkbenchContributionsResult PrepareOwnerReplacement(
		WorkbenchContributionOwner replacement, std::uint64_t expectedGeneration,
		std::span<const WorkbenchViewContainerDescriptor> containers,
		std::span<const WorkbenchViewDescriptor> views) const noexcept;
	[[nodiscard]] PrepareWorkbenchContributionsResult PrepareOwnerDisposal(
		const WorkbenchContributionOwner& owner) const noexcept;
	//! Revocation has no allocation or callbacks. Registration reserves revision
	//! capacity for the removal of every live owner, including allocation failure.
	[[nodiscard]] EWorkbenchContributionChangeStatus DisposeOwner(
		const WorkbenchContributionOwner& owner) noexcept;
	//! Preflights the allocation-free commit so native page and catalog
	//! candidates can verify both revision fences before either is published.
	[[nodiscard]] bool CanCommit(const PreparedWorkbenchContributions& change) const noexcept;
	[[nodiscard]] EWorkbenchContributionChangeStatus Commit(
		std::unique_ptr<PreparedWorkbenchContributions> change) noexcept;
	[[nodiscard]] bool IsOwnerCurrent(const WorkbenchContributionOwner& owner) const noexcept;
	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;
	[[nodiscard]] static bool IsValidViewContainerDescriptor(
		const WorkbenchViewContainerDescriptor& descriptor) noexcept;
	//! Validates the complete batch before any consumer can register a partial index.
	[[nodiscard]] static bool IsValidContributionSnapshot(
		const WorkbenchContributionSnapshot& snapshot) noexcept;

private:
	WorkbenchContributionSnapshot m_snapshot;
	bool m_extensionBatchRegistered = false;
	std::uint64_t m_lastOwnerGeneration{};
};

} // namespace workbench::layout
