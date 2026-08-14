/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "config/CConfigurationNetworkPolicy.h"
#include "update/IUpdateService.h"
#include "update/UpdateService.h"
#include "update/UpdateTypes.h"
#include "update/UpdateVersion.h"

namespace config {
class IConfigurationService;
}

namespace update {

//! Every way composing the update stack can fail before a byte leaves the
//! machine. They are distinct because they mean different things to the user: a
//! profile this process cannot identify is a programming error, an unreadable
//! configuration is a broken installation, and an unresolvable staging root is a
//! machine with no writable per-user application data.
enum class EUpdateCompositionOutcome : std::uint8_t {
	Ready,
	InvalidProfileId,
	ConfigurationUnavailable,
	ConfigurationInvalid,
	StagingUnavailable,
	InternalFailure,
};

//! `update.mode`, `update.enableWindowsBackgroundUpdates`, and `update.titleBar`
//! read as one coherent snapshot. Reading them one `GetValue` at a time is not a
//! coherent policy read; see `config/CLAUDE.md`.
struct UpdateConfigurationSnapshot final {
	EUpdateMode mode = EUpdateMode::Default;
	bool enableWindowsBackgroundUpdates = true;
	//! `update.titleBar`. When false the indicator never appears, even in an
	//! actionable state, exactly as upstream's `updateTitleBarEntry.ts` behaves.
	bool titleBar = true;

	[[nodiscard]] bool operator==(const UpdateConfigurationSnapshot&) const = default;
};

/*!
	@brief One window's production update stack, assembled and owned together.

	The state machine borrows seven collaborators, and every one of them has to
	outlive it. Putting them in one object with a defined destruction order is
	what makes that a compile-time fact rather than a member-declaration-order
	convention spread across `CEditWnd`.

	The configuration snapshot is taken once, here, and never re-read. Changing
	`update.mode` therefore takes effect in the next window, which matches the
	way other request clients freeze their own network policy.
*/
class UpdateComposition final {
public:
	UpdateComposition(
		config::ConfigurationNetworkPolicySnapshot networkPolicy,
		UpdateConfigurationSnapshot configuration,
		std::filesystem::path stagingRoot,
		std::filesystem::path executablePath,
		UpdateServiceOptions options);
	~UpdateComposition();

	UpdateComposition(const UpdateComposition&) = delete;
	UpdateComposition& operator=(const UpdateComposition&) = delete;

	[[nodiscard]] IUpdateService& Service() noexcept;
	[[nodiscard]] const UpdateConfigurationSnapshot& Configuration() const noexcept;
	//! Exposed for `update.showUpdateInfo`, whose whole effect is opening the
	//! release page. It goes through the launcher rather than a second
	//! `ShellExecute` call site so process creation stays in the one component
	//! that owns it.
	[[nodiscard]] IUpdateLauncher& Launcher() noexcept;

	//! Stops the worker thread and joins it. Called before the window tears down
	//! the surfaces an update notification would otherwise reach; the destructor
	//! calls it too, so a missed call is a latency bug rather than a crash.
	void Shutdown() noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

struct UpdateCompositionResult final {
	EUpdateCompositionOutcome outcome = EUpdateCompositionOutcome::InternalFailure;
	std::unique_ptr<UpdateComposition> composition;
	//! A fixed, non-confidential sentence. It never carries a profile id or a path.
	std::string diagnostic;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return outcome == EUpdateCompositionOutcome::Ready && composition != nullptr;
	}
};

//! Builds the whole stack for the selected user-data profile. Never throws and
//! never contacts the network; the first request happens on the first check.
[[nodiscard]] UpdateCompositionResult CreateUpdateComposition(
	config::IConfigurationService& configurationService,
	std::wstring userDataProfileId) noexcept;

//! Reads the three `update.*` settings as one snapshot. Exposed separately
//! because the composition is only built for a window that has a runtime, while
//! the settings themselves are meaningful to any caller with a profile.
[[nodiscard]] std::optional<UpdateConfigurationSnapshot> ReadUpdateConfiguration(
	config::IConfigurationService& configurationService,
	const std::wstring& userDataProfileId);

//! What `version.h` says this binary is: `VER_A`.`VER_B`.`VER_C`.`BUILD_VERSION`.
[[nodiscard]] UpdateVersion CurrentBuildVersion() noexcept;

//! `GIT_REMOTE_ORIGIN_URL` as recorded by the generated `githash.h`. A fork
//! checks its own releases; a build with no recorded origin checks nothing.
[[nodiscard]] std::wstring CurrentBuildRemoteUrl();

} // namespace update
