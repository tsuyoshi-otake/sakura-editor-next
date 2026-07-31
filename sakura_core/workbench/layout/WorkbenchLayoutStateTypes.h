/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//! HWND-independent, stable-ID layout state for the workbench.
namespace workbench::layout {

inline constexpr std::uint32_t kWorkbenchLayoutStateSchemaVersion = 2;
//! Keep the state boundary no wider than the contribution registry's external-ID/replay limits.
inline constexpr std::size_t kMaxWorkbenchLayoutIdLength = 160;
inline constexpr std::size_t kMaxWorkbenchLayoutOperationIdLength = 160;
inline constexpr std::size_t kMaxWorkbenchLayoutCompletedOperations = 256;
//! Non-zero persisted divider extents are bounded so every accepted state is memento-encodable.
inline constexpr std::uint32_t kMaximumWorkbenchLayoutCommittedExtentDip = 65'535;

[[nodiscard]] constexpr bool IsValidWorkbenchLayoutId(std::string_view value,
	std::size_t maximumLength = kMaxWorkbenchLayoutIdLength) noexcept
{
	return !value.empty() && value.size() <= maximumLength && value.find('\0') == std::string_view::npos;
}

//! Physical parts are distinct from view and container identities.
enum class EWorkbenchPartPosition : std::uint8_t {
	Top,
	Left,
	Center,
	Right,
	Bottom,
};

//! A view container can be placed independently of its physical part HWND.
enum class EWorkbenchViewContainerLocation : std::uint8_t {
	SideBar,
	Panel,
	AuxiliaryBar,
};

//! Persisted logical panel alignment. Maximized and drag geometry remain window-local/transient.
enum class EWorkbenchPanelAlignment : std::uint8_t {
	Left,
	Center,
	Right,
	Justify,
};

struct WorkbenchPartState {
	std::string partId;
	bool visible = true;
	EWorkbenchPartPosition position = EWorkbenchPartPosition::Center;
	//! The last committed divider extent in DIP. Drag/hover geometry is adapter-local and absent here.
	std::optional<std::uint32_t> committedExtentDip;
	[[nodiscard]] constexpr bool operator==(const WorkbenchPartState&) const noexcept = default;
};

struct WorkbenchViewContainerState {
	std::string containerId;
	EWorkbenchViewContainerLocation location = EWorkbenchViewContainerLocation::SideBar;
	std::int32_t order = 0;
	bool visible = true;
	std::optional<std::string> activeViewId;
	[[nodiscard]] bool operator==(const WorkbenchViewContainerState&) const noexcept = default;
};

struct WorkbenchViewState {
	std::string viewId;
	std::string containerId;
	std::int32_t order = 0;
	bool visible = true;
	[[nodiscard]] bool operator==(const WorkbenchViewState&) const noexcept = default;
};

//! Focus keeps part, container, and view as independent concepts. Only one leaf needs to be present.
struct WorkbenchFocusState {
	std::optional<std::string> partId;
	std::optional<std::string> containerId;
	std::optional<std::string> viewId;
	[[nodiscard]] bool operator==(const WorkbenchFocusState&) const noexcept = default;
};

//! Exactly one ViewContainer can own each physical container location at a time.
//! Visibility, activation, and keyboard focus remain independent state axes.
struct WorkbenchActiveContainerState {
	std::optional<std::string> sideBar;
	std::optional<std::string> panel;
	std::optional<std::string> auxiliaryBar;
	[[nodiscard]] bool operator==(const WorkbenchActiveContainerState&) const noexcept = default;
};

struct WorkbenchLayoutStateSnapshot {
	std::uint32_t schemaVersion = kWorkbenchLayoutStateSchemaVersion;
	std::uint64_t generation = 0;
	std::uint64_t revision = 0;
	std::vector<WorkbenchPartState> parts;
	std::vector<WorkbenchViewContainerState> containers;
	std::vector<WorkbenchViewState> views;
	WorkbenchActiveContainerState activeContainers;
	EWorkbenchPanelAlignment panelAlignment = EWorkbenchPanelAlignment::Center;
	WorkbenchFocusState focus;
};

enum class EWorkbenchLayoutOperationStatus : std::uint8_t {
	Succeeded,
	NotApplicable,
	Conflict,
	Invalid,
	UnknownId,
	Unsupported,
	Failed,
};

enum class EWorkbenchLayoutOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	OperationIdConflict,
	RevisionConflict,
	RevisionExhausted,
	InvalidRequest,
	UnknownPart,
	UnknownContainer,
	UnknownView,
	CapabilityNotSupported,
	AlreadyInRequestedState,
	NoRegisteredFallback,
	InconsistentHierarchy,
	TargetNotVisible,
	TargetNotActive,
	InternalFailure,
};

//! Initial memento hydration has a separate, non-revisioned terminal result.
//! A successful hydration is deliberately not a user operation and therefore
//! does not emit a change batch or consume an operation replay entry.
enum class EWorkbenchLayoutHydrationStatus : std::uint8_t {
	Succeeded,
	InvalidSnapshot,
	AlreadyHydrated,
	Failed,
};

struct WorkbenchLayoutHydrationResult {
	EWorkbenchLayoutHydrationStatus status = EWorkbenchLayoutHydrationStatus::Failed;
	//! Always the currently committed stable state.  It is unchanged on failure.
	WorkbenchLayoutStateSnapshot snapshot;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkbenchLayoutHydrationStatus::Succeeded;
	}
};

struct WorkbenchLayoutOperationMetadata {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

enum class EWorkbenchLayoutChangeKind : std::uint8_t {
	Initialized,
	Reconciled,
	PartVisibilityChanged,
	PartExtentChanged,
	ContainerRevealed,
	ContainerActivated,
	ContainerMoved,
	ViewRevealed,
	ViewMoved,
	FocusChanged,
};

struct WorkbenchLayoutChange {
	EWorkbenchLayoutChangeKind kind = EWorkbenchLayoutChangeKind::Initialized;
	std::optional<std::string> partId;
	std::optional<std::string> containerId;
	std::optional<std::string> viewId;
};

struct WorkbenchLayoutChangeBatch {
	std::uint64_t generation = 0;
	std::uint64_t baseRevision = 0;
	std::uint64_t revision = 0;
	std::vector<WorkbenchLayoutChange> changes;
};

struct WorkbenchLayoutOperationResult {
	EWorkbenchLayoutOperationStatus status = EWorkbenchLayoutOperationStatus::Failed;
	EWorkbenchLayoutOperationReason reason = EWorkbenchLayoutOperationReason::None;
	std::uint64_t revision = 0;
	bool replayed = false;
	//! Present only for a successful revision-changing operation.
	std::optional<WorkbenchLayoutChangeBatch> changeBatch;
	//! The final stable state for every result, including failed requests.
	WorkbenchLayoutStateSnapshot snapshot;
};

using WorkbenchLayoutChangeCallback = std::function<void(const WorkbenchLayoutChangeBatch&)>;

class IWorkbenchLayoutSubscription {
public:
	virtual ~IWorkbenchLayoutSubscription() = default;
	virtual void Unsubscribe() noexcept = 0;
	[[nodiscard]] virtual bool IsSubscribed() const noexcept = 0;
};

} // namespace workbench::layout
