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

//! The three built-in physical surfaces supported by the current native shell.
struct BuiltinPartProjection {
	BuiltinPartProjectionState left;
	BuiltinPartProjectionState bottom;
	BuiltinPartProjectionState right;
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
	LegacyExtensionViews,
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

/*!
	@brief Projects only the built-in physical parts from one model snapshot.

	The current native mapping is Sidebar -> Left, Panel -> Bottom, and
	Auxiliarybar -> Right. Unknown parts are intentionally ignored so later
	contributions do not become an accidental native-shell contract.
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

} // namespace workbench::win32
