/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::layout {

//! The logical workbench area containing a view container. Values intentionally match VS Code.
enum class EViewContainerLocation : std::uint8_t {
	Sidebar,
	Panel,
	AuxiliaryBar,
};

//! Owner generations make extension reload/dispose deterministic without coupling the registry to transport.
struct WorkbenchContributionOwner {
	std::string ownerId;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
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

struct WorkbenchContributionOperation {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

//! One atomic owner-scoped contribution registration. A batch either entirely commits or leaves the registry unchanged.
struct RegisterWorkbenchContributionsRequest {
	WorkbenchContributionOperation operation;
	WorkbenchContributionOwner owner;
	std::vector<WorkbenchPartDescriptor> parts;
	std::vector<WorkbenchViewContainerDescriptor> viewContainers;
	std::vector<WorkbenchViewDescriptor> views;
};

struct DisposeWorkbenchContributionsRequest {
	WorkbenchContributionOperation operation;
	WorkbenchContributionOwner owner;
};

enum class EWorkbenchContributionOperationStatus : std::uint8_t {
	Succeeded,
	Replayed,
	NotApplicable,
	Rejected,
	Conflict,
	StaleRevision,
	RevisionExhausted,
};

enum class EWorkbenchContributionOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	InvalidOwner,
	InvalidDescriptor,
	EmptyBatch,
	BatchLimitExceeded,
	RegistryLimitExceeded,
	DuplicatePartId,
	DuplicateViewContainerId,
	DuplicateViewId,
	UnknownViewContainer,
	OwnerGenerationConflict,
	OperationIdConflict,
	BuiltinProtected,
	SubscriptionLimitExceeded,
};

struct WorkbenchContributionOperationResult {
	EWorkbenchContributionOperationStatus status{ EWorkbenchContributionOperationStatus::Rejected };
	EWorkbenchContributionOperationReason reason{ EWorkbenchContributionOperationReason::None };
	std::uint64_t revision{};
};

struct RegisteredWorkbenchPart {
	WorkbenchPartDescriptor descriptor;
	WorkbenchContributionOwner owner;
	bool isBuiltin{};
};

struct RegisteredWorkbenchViewContainer {
	WorkbenchViewContainerDescriptor descriptor;
	WorkbenchContributionOwner owner;
	bool isBuiltin{};
};

struct RegisteredWorkbenchView {
	WorkbenchViewDescriptor descriptor;
	WorkbenchContributionOwner owner;
	bool isBuiltin{};
};

//! A deterministic, ID-sorted snapshot. It contains no HWNDs or view implementation objects.
struct WorkbenchContributionSnapshot {
	std::uint64_t revision{};
	std::vector<RegisteredWorkbenchPart> parts;
	std::vector<RegisteredWorkbenchViewContainer> viewContainers;
	std::vector<RegisteredWorkbenchView> views;
};

enum class EWorkbenchContributionChangeKind : std::uint8_t {
	Registered,
	OwnerDisposed,
};

struct WorkbenchContributionChange {
	std::uint64_t revision{};
	EWorkbenchContributionChangeKind kind{ EWorkbenchContributionChangeKind::Registered };
	WorkbenchContributionOwner owner;
};

using WorkbenchContributionSubscriptionId = std::uint64_t;
using WorkbenchContributionListener = std::function<void(const WorkbenchContributionChange&)>;

//! Thread-safe, HWND-free registry for Parts, ViewContainers and Views.
class WorkbenchContributionRegistry final {
public:
	WorkbenchContributionRegistry();
	~WorkbenchContributionRegistry();

	WorkbenchContributionRegistry(const WorkbenchContributionRegistry&) = delete;
	WorkbenchContributionRegistry& operator=(const WorkbenchContributionRegistry&) = delete;

	[[nodiscard]] WorkbenchContributionOperationResult Register(const RegisterWorkbenchContributionsRequest& request);
	[[nodiscard]] WorkbenchContributionOperationResult DisposeOwner(const DisposeWorkbenchContributionsRequest& request);
	[[nodiscard]] WorkbenchContributionSnapshot Snapshot() const;

	//! The subscriber is removed automatically when its exact owner generation is disposed.
	[[nodiscard]] std::optional<WorkbenchContributionSubscriptionId> Subscribe(
		const WorkbenchContributionOwner& subscriber, WorkbenchContributionListener listener);
	void Unsubscribe(WorkbenchContributionSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;
	[[nodiscard]] static bool IsValidOperationId(std::string_view value) noexcept;

private:
	struct Impl;
	Impl* m_impl;
};

} // namespace workbench::layout
