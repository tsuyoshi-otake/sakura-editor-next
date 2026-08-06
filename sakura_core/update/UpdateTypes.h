/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "update/UpdateVersion.h"

//! In-session updating, modelled on VS Code's update service.
//!
//! The vocabulary here is upstream's, verbatim: the state names are the exact
//! strings `src/vs/platform/update/common/update.ts` publishes, because they are
//! the values of the `updateState` context key that every `when` clause in
//! `src/vs/workbench/contrib/update/browser/update.ts` compares against. A
//! renamed state would make those clauses — which are also copied verbatim —
//! silently never match.
namespace update {

//! Upstream `StateType`. Declaration order follows upstream's own.
enum class EUpdateStateType : std::uint8_t {
	Uninitialized,
	Disabled,
	Idle,
	CheckingForUpdates,
	AvailableForDownload,
	Downloading,
	Downloaded,
	Updating,
	Ready,
	Overwriting,
	Cancelling,
	Restarting,
};

//! The `updateState` context-key value. These carry upstream's spaces
//! (`"checking for updates"`); do not "normalize" them into identifiers.
[[nodiscard]] std::string_view UpdateStateTypeId(EUpdateStateType state) noexcept;
[[nodiscard]] std::optional<EUpdateStateType> ParseUpdateStateTypeId(std::string_view id) noexcept;

//! Upstream `UpdateType`, minus `Snap`, which is Linux-only and has no meaning
//! on this platform. `Setup` is a build running from a recorded installer
//! location and can update itself; `Archive` is any other build — a developer
//! tree, a ZIP extraction — and stops at `available for download`.
enum class EUpdateType : std::uint8_t {
	Setup,
	Archive,
};

//! `update.mode` from `update.config.contribution.ts`.
enum class EUpdateMode : std::uint8_t {
	None,
	Manual,
	Start,
	Default,
};

[[nodiscard]] std::string_view UpdateModeId(EUpdateMode mode) noexcept;
[[nodiscard]] std::optional<EUpdateMode> ParseUpdateModeId(std::string_view id) noexcept;

//! Upstream `IUpdate`, narrowed to what a GitHub Release actually carries.
struct Update final {
	//! `3.1.0.7221`, the four-field product version of the candidate build.
	UpdateVersion version;
	//! `v3.1.0-build.7221`.
	std::wstring tagName;
	//! `sakura_install3-1-0-7221-x64.exe`.
	std::wstring assetName;
	std::wstring downloadUrl;
	std::uint64_t sizeBytes = 0;
	//! Lowercase hex, from the asset's `digest` field. Absent when the release
	//! predates GitHub's digest field; verification then rests on `sizeBytes`
	//! alone and the state machine says so in its diagnostic.
	std::optional<std::wstring> sha256;
	std::wstring releaseUrl;
	//! The release body, used by `update.showUpdateInfo`.
	std::wstring releaseNotes;

	[[nodiscard]] bool operator==(const Update&) const = default;
};

struct UpdateState final {
	EUpdateStateType type = EUpdateStateType::Uninitialized;
	//! Meaningful from `AvailableForDownload` onwards.
	EUpdateType updateType = EUpdateType::Setup;
	std::optional<Update> update;
	//! Why the state machine landed here. Empty on the ordinary paths; a
	//! user-facing sentence on every fail-closed path.
	std::wstring reason;

	[[nodiscard]] bool operator==(const UpdateState&) const = default;
};

//! Upstream `ACTIONABLE_STATES` in `updateTitleBarEntry.ts`. The title-bar
//! Update button exists only in these three states.
[[nodiscard]] bool IsActionableUpdateState(EUpdateStateType state) noexcept;

//! What `workbench.actions.updateIndicator` dispatches to, per upstream's
//! switch over `ACTIONABLE_STATES`. Empty for a state with no action.
[[nodiscard]] std::string_view UpdateIndicatorCommandId(EUpdateStateType state) noexcept;

} // namespace update
