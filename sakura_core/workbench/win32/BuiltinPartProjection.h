/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <cstdint>
#include <optional>

namespace workbench::win32 {

//! HWND-free state for one of the currently materialized native workbench parts.
struct BuiltinPartProjectionState {
	bool visible = true;
	std::optional<std::uint32_t> committedExtentDip;
	[[nodiscard]] constexpr bool operator==(const BuiltinPartProjectionState&) const noexcept = default;
};

//! HWND-free state for the Banner Part (`workbench.parts.banner`).
//!
//! Unlike Left/Bottom/Right, VS Code sizes the banner from its own content
//! rather than a user-draggable sash, so it has no committed extent to carry.
//! Reusing `BuiltinPartProjectionState` here would silently promise a
//! sash-driven extent this Part does not have; a narrower type keeps that
//! honest.
struct BuiltinBannerProjectionState {
	bool visible = true;
	[[nodiscard]] constexpr bool operator==(const BuiltinBannerProjectionState&) const noexcept = default;
};

//! The built-in physical surfaces supported by the current native shell.
struct BuiltinPartProjection {
	BuiltinPartProjectionState left;
	BuiltinPartProjectionState bottom;
	BuiltinPartProjectionState right;
	//! Left/Bottom/Right are required because the native shell's editor-rectangle
	//! math cannot be computed without them. The Banner is a separate, optional
	//! member of this projection: `std::nullopt` means the current contribution
	//! registry snapshot has no `workbench.parts.banner` registration at all
	//! (a malformed/degraded model the native host must treat as "no banner
	//! capability"), while an engaged optional with `visible == false` means the
	//! banner is registered and present but currently hidden. Collapsing those
	//! two states into one would let a host reserve zero height for a banner it
	//! actually has, or silently treat a malformed model as merely quiet.
	std::optional<BuiltinBannerProjectionState> banner;
	[[nodiscard]] constexpr bool operator==(const BuiltinPartProjection&) const noexcept = default;
};

//! Every projection attempt reaches exactly one terminal status.
enum class EBuiltinPartProjectionStatus : std::uint8_t {
	Succeeded,
	UnsupportedSchema,
	MissingRequiredPart,
	DuplicateRequiredPart,
	UnsupportedPosition,
	InvalidExtent,
	//! The optional Banner Part was present more than once. Its absence is a
	//! valid projection (see `BuiltinPartProjection::banner`), but a duplicate
	//! registration is malformed data the same way a duplicate Sidebar is.
	DuplicateOptionalPart,
};

//! A failed result never exposes a partial native projection.
struct BuiltinPartProjectionResult {
	EBuiltinPartProjectionStatus status = EBuiltinPartProjectionStatus::UnsupportedSchema;
	std::optional<BuiltinPartProjection> projection;

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return status == EBuiltinPartProjectionStatus::Succeeded && projection.has_value();
	}
};

//! Stable native shell surfaces that currently have a concrete ViewContainer/View host.
enum class BuiltinActiveSurface : std::uint8_t {
	//! Focus-only native editor surface; never an active ViewContainer/View slot.
	Editor,
	Explorer,
	Outline,
	SourceControl,
	Terminal,
	Problems,
	Output,
};

//! HWND-free active logical surfaces for the native shell.
struct BuiltinActiveSurfaceProjection {
	std::optional<BuiltinActiveSurface> sidebar;
	std::optional<BuiltinActiveSurface> panel;
	std::optional<BuiltinActiveSurface> auxiliaryBar;
	//! Keyboard focus is independent from activation and is absent unless explicitly projected.
	std::optional<BuiltinActiveSurface> focus;
	[[nodiscard]] constexpr bool operator==(const BuiltinActiveSurfaceProjection&) const noexcept = default;
};

//! Every native ViewContainer/View projection attempt reaches exactly one terminal status.
enum class EBuiltinActiveSurfaceProjectionStatus : std::uint8_t {
	Succeeded,
	UnsupportedSchema,
	DuplicateContainer,
	DuplicateView,
	InvalidActiveContainer,
	InvalidActiveView,
	UnsupportedSurface,
	InconsistentHierarchy,
	InvalidFocus,
};

//! A failed result never exposes a partial logical native-surface projection.
struct BuiltinActiveSurfaceProjectionResult {
	EBuiltinActiveSurfaceProjectionStatus status = EBuiltinActiveSurfaceProjectionStatus::UnsupportedSchema;
	std::optional<BuiltinActiveSurfaceProjection> projection;

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return status == EBuiltinActiveSurfaceProjectionStatus::Succeeded && projection.has_value();
	}
};

//! The complete native shell projection for one committed Workbench snapshot.
//!
//! VS Code keeps physical Parts and the active ViewContainer/View projection as
//! separate responsibilities, but the native shell must apply them from the same
//! committed snapshot.  This value is the boundary between those two model
//! concerns and HWND-bearing controls.
struct BuiltinWorkbenchProjection {
	BuiltinPartProjection parts;
	BuiltinActiveSurfaceProjection surfaces;
	[[nodiscard]] constexpr bool operator==(const BuiltinWorkbenchProjection&) const noexcept = default;
};

//! A failed composite projection never exposes a half-applicable native state.
enum class EBuiltinWorkbenchProjectionStatus : std::uint8_t {
	Succeeded,
	PartProjectionFailed,
	ActiveSurfaceProjectionFailed,
};

struct BuiltinWorkbenchProjectionResult {
	EBuiltinWorkbenchProjectionStatus status = EBuiltinWorkbenchProjectionStatus::PartProjectionFailed;
	std::optional<BuiltinWorkbenchProjection> projection;

	[[nodiscard]] constexpr bool Succeeded() const noexcept
	{
		return status == EBuiltinWorkbenchProjectionStatus::Succeeded && projection.has_value();
	}
};

/*!
	@brief Projects only the built-in physical parts from one model snapshot.

	The current native mapping is Sidebar -> Left, Panel -> Bottom, and
	Auxiliarybar -> Right; these three are required, exactly as before. The
	Banner Part (`workbench.parts.banner`) is projected independently and is
	optional: an unregistered banner succeeds with `projection->banner ==
	std::nullopt`, while a malformed banner (duplicate registration, or a
	position other than Top) still fails the whole projection. Unknown
	unrelated parts are intentionally ignored so later contributions do not
	become an accidental native-shell contract.
*/
[[nodiscard]] BuiltinPartProjectionResult ProjectBuiltinParts(
	const layout::WorkbenchLayoutStateSnapshot& snapshot);

/*!
	@brief Projects active built-in ViewContainer/View pairs to supported native surfaces.

	This does not project physical part extent or visibility; use ProjectBuiltinParts for
	that independent boundary. Unsupported active pairs, malformed active hierarchies,
	or an explicit focus that cannot resolve to a supported active leaf fail atomically.
*/
[[nodiscard]] BuiltinActiveSurfaceProjectionResult ProjectBuiltinActiveSurfaces(
	const layout::WorkbenchLayoutStateSnapshot& snapshot);

/*!
	@brief Projects a complete native shell view from one model snapshot.

	The individual functions above remain useful for focused validation, while this
	composite adapter is the only boundary used by the window projection path.  It
	prevents a valid Part projection from being applied when the active surface
	projection for the same snapshot is malformed.
*/
[[nodiscard]] BuiltinWorkbenchProjectionResult ProjectBuiltinWorkbench(
	const layout::WorkbenchLayoutStateSnapshot& snapshot);

} // namespace workbench::win32
