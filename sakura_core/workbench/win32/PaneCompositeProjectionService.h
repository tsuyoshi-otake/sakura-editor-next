/*! @file
	@brief Atomic native projection for the three Workbench Pane Composite Parts
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::win32 {

inline constexpr std::size_t kPaneCompositeHostCount = 3;

struct PaneCompositeHostState final {
	layout::EWorkbenchViewContainerLocation location{
		layout::EWorkbenchViewContainerLocation::SideBar };
	std::string partId;
	bool visible{};
	std::optional<std::uint32_t> committedExtentDip;
	std::optional<std::string> activeContainerId;
	//! The page physically owned by this host. It is separate from logical activation.
	std::optional<std::string> attachedContainerId;

	[[nodiscard]] bool operator==(const PaneCompositeHostState&) const noexcept = default;
};

struct PaneCompositeFocusProjection final {
	std::string partId;
	std::optional<layout::EWorkbenchViewContainerLocation> location;
	std::optional<std::string> containerId;
	std::optional<std::string> viewId;

	[[nodiscard]] bool operator==(const PaneCompositeFocusProjection&) const noexcept = default;
};

struct PaneCompositeProjection final {
	std::uint64_t generation{};
	std::uint64_t revision{};
	std::array<PaneCompositeHostState, kPaneCompositeHostCount> hosts;
	//! Absent unless the committed snapshot contains one explicit coherent focus target.
	std::optional<PaneCompositeFocusProjection> focus;
};

enum class EPaneCompositeHostApplyStatus : std::uint8_t {
	Applied,
	AlreadyApplied,
	Failed,
};

struct PaneCompositeHostBinding final {
	layout::EWorkbenchViewContainerLocation location{
		layout::EWorkbenchViewContainerLocation::SideBar };
	//! Fast capability query used by movement menus and pre-transaction validation.
	std::function<bool(std::string_view)> supportsContainer;
	//! Complete non-mutating validation for the staged native host state.
	std::function<bool(const PaneCompositeHostState&)> canApply;
	//! Returns the current stable native owner. Empty means the host cannot be observed.
	std::function<std::optional<PaneCompositeHostState>()> readState;
	//! Applies one staged state. A failure may be partial; the service always compensates.
	std::function<EPaneCompositeHostApplyStatus(const PaneCompositeHostState&)> applyState;
	//! Ends this adapter's projection lifetime. It must not destroy the borrowed physical host.
	std::function<bool()> closeProjection;
};

enum class EPaneCompositePrepareStatus : std::uint8_t {
	Prepared,
	InvalidSnapshot,
	MissingRequiredPart,
	DuplicateRequiredPart,
	UnsupportedPartPosition,
	InvalidExtent,
	DuplicateActiveContainer,
	InvalidActiveContainer,
	UnsupportedContainerLocation,
	InvalidFocus,
	HostUnavailable,
	HostPrepareFailed,
	Busy,
	Faulted,
	Closed,
	InternalFailure,
};

struct PaneCompositePreparationToken final {
	std::uint64_t value{};
	[[nodiscard]] bool operator==(const PaneCompositePreparationToken&) const noexcept = default;
};

struct PaneCompositePrepareResult final {
	EPaneCompositePrepareStatus status{ EPaneCompositePrepareStatus::InternalFailure };
	std::optional<PaneCompositePreparationToken> token;
	std::optional<layout::EWorkbenchViewContainerLocation> failedLocation;
	std::array<std::optional<PaneCompositeHostState>, kPaneCompositeHostCount> finalStates;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EPaneCompositePrepareStatus::Prepared && token.has_value();
	}
};

enum class EPaneCompositeCommitStatus : std::uint8_t {
	Committed,
	HostCommitFailedCompensated,
	HostVerificationFailedCompensated,
	CompensationFailed,
	StalePreparation,
	Faulted,
	Closed,
	InternalFailure,
};

struct PaneCompositeCommitResult final {
	EPaneCompositeCommitStatus status{ EPaneCompositeCommitStatus::InternalFailure };
	std::optional<layout::EWorkbenchViewContainerLocation> failedLocation;
	std::optional<layout::EWorkbenchViewContainerLocation> compensationFailedLocation;
	std::array<std::optional<PaneCompositeHostState>, kPaneCompositeHostCount> finalStates;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EPaneCompositeCommitStatus::Committed;
	}
};

enum class EPaneCompositeCancelStatus : std::uint8_t {
	Cancelled,
	StalePreparation,
	Closed,
};

enum class EPaneCompositeCloseStatus : std::uint8_t {
	Closed,
	AlreadyClosed,
	HostCloseFailed,
};

struct PaneCompositeSupportedLocationsResult final {
	std::vector<layout::EWorkbenchViewContainerLocation> locations;
	bool complete{};

	[[nodiscard]] bool Contains(layout::EWorkbenchViewContainerLocation location) const noexcept;
};

//! One staged owner for Primary Side Bar, Panel, and Auxiliary Bar projection.
//! The bindings borrow physical hosts; this service owns only projection state.
class PaneCompositeProjectionService final {
public:
	explicit PaneCompositeProjectionService(
		std::array<PaneCompositeHostBinding, kPaneCompositeHostCount> bindings);
	~PaneCompositeProjectionService();
	PaneCompositeProjectionService(const PaneCompositeProjectionService&) = delete;
	PaneCompositeProjectionService& operator=(const PaneCompositeProjectionService&) = delete;

	[[nodiscard]] PaneCompositePrepareResult Prepare(
		const layout::WorkbenchLayoutStateSnapshot& snapshot) noexcept;
	[[nodiscard]] PaneCompositeCommitResult Commit(
		PaneCompositePreparationToken token) noexcept;
	[[nodiscard]] EPaneCompositeCancelStatus Cancel(
		PaneCompositePreparationToken token) noexcept;
	[[nodiscard]] PaneCompositeSupportedLocationsResult SupportedLocations(
		std::string_view containerId) const noexcept;
	[[nodiscard]] const PaneCompositeProjection* LastCommittedProjection() const noexcept;
	[[nodiscard]] EPaneCompositeCloseStatus Close() noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::win32
