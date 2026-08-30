/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchContributionRegistry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::viewcontainer {

//! Opaque native values are interpreted only by the page implementation.
using ViewContainerNativeHandle = std::uintptr_t;

struct ViewContainerPageHost final {
	std::string id;
	layout::EViewContainerLocation location{ layout::EViewContainerLocation::Sidebar };
	ViewContainerNativeHandle nativeParent{};

	[[nodiscard]] bool operator==(const ViewContainerPageHost&) const noexcept = default;
};

struct ViewContainerFocusToken final {
	ViewContainerNativeHandle value{};

	[[nodiscard]] bool operator==(const ViewContainerFocusToken&) const noexcept = default;
};

enum class EViewContainerFocusCaptureStatus : std::uint8_t {
	Captured,
	NoFocus,
	Failed,
};

struct ViewContainerFocusCaptureResult final {
	EViewContainerFocusCaptureStatus status{ EViewContainerFocusCaptureStatus::Failed };
	std::optional<ViewContainerFocusToken> token;
};

enum class EViewContainerPageDetachStatus : std::uint8_t {
	Detached,
	Failed,
};

enum class EViewContainerPageReparentStatus : std::uint8_t {
	Reparented,
	Failed,
};

enum class EViewContainerPageAttachStatus : std::uint8_t {
	Attached,
	Failed,
};

enum class EViewContainerFocusRestoreStatus : std::uint8_t {
	Restored,
	Failed,
};

enum class EViewContainerPageCloseStatus : std::uint8_t {
	Closed,
	Failed,
};

//! One logical ViewContainer page. The page owns every native window it creates.
//!
//! A host is borrowed and never owns the page. Failed Detach, Reparent, or Attach
//! calls must leave that step's input state unchanged. Close is invoked at most
//! once by the pool; after a failed Close, the page destructor owns final cleanup.
class IViewContainerPage {
public:
	virtual ~IViewContainerPage() = default;

	[[nodiscard]] virtual std::string_view ContainerId() const noexcept = 0;
	[[nodiscard]] virtual ViewContainerFocusCaptureResult CaptureFocusToken() noexcept = 0;
	[[nodiscard]] virtual EViewContainerPageDetachStatus Detach(
		const ViewContainerPageHost& host) noexcept = 0;
	//! Reparents every native window owned by this page. Zero means no physical host.
	[[nodiscard]] virtual EViewContainerPageReparentStatus Reparent(
		ViewContainerNativeHandle nativeParent) noexcept = 0;
	[[nodiscard]] virtual EViewContainerPageAttachStatus Attach(
		const ViewContainerPageHost& host) noexcept = 0;
	[[nodiscard]] virtual EViewContainerFocusRestoreStatus RestoreFocusToken(
		ViewContainerFocusToken token) noexcept = 0;
	[[nodiscard]] virtual EViewContainerPageCloseStatus Close() noexcept = 0;
};

} // namespace workbench::viewcontainer
