/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "update/IUpdateService.h"
#include "update/UpdateTypes.h"
#include "update/UpdateVersion.h"

namespace platform::request {
class IRequestService;
}

namespace update {

//! Whether `update.mode` permits a check the user did not ask for.
[[nodiscard]] bool AllowsAutomaticCheck(EUpdateMode mode) noexcept;
//! Whether `update.mode` permits repeating that check while the editor runs.
//! `start` checks once and then stops; only `default` keeps polling.
[[nodiscard]] bool AllowsPeriodicCheck(EUpdateMode mode) noexcept;

struct UpdateServiceOptions final {
	//! This build, from `version.h` / `githash.h`, injected by the composition
	//! root rather than read here — the state machine must be testable at any
	//! version, including versions this source tree never had.
	UpdateVersion currentVersion;
	//! `GIT_REMOTE_ORIGIN_URL`. A fork checks its own releases.
	std::wstring remoteUrl;
	std::wstring architecture = L"x64";
	EUpdateMode mode = EUpdateMode::Default;
	//! `update.enableWindowsBackgroundUpdates`. When set, a found update walks
	//! itself to `ready` without further clicks, and the title-bar button the
	//! user finally sees says "Restart to Update".
	bool enableWindowsBackgroundUpdates = true;
	std::size_t maximumSubscriptions = 256;
	//! Refuses an absurd asset before a byte of it is written to the profile.
	std::size_t maximumInstallerBytes = 128u * 1024u * 1024u;
	//! GitHub rejects API requests without one.
	std::wstring userAgent = L"sakura-editor-next";
	//! `update.mode` = `start`/`default`'s one-time startup check, armed once
	//! `Initialize()` lands at `idle`. Matches upstream's own delayed startup
	//! check (`AbstractUpdateService`), so a session that exits quickly never
	//! fires it.
	std::chrono::milliseconds initialCheckDelay = std::chrono::seconds(30);
	//! `update.mode` = `default`'s recurring poll, rearmed every time a check,
	//! download, or apply cycle settles back at `idle`. `manual` and `start`
	//! never reach this: `start` only ever arms the initial check above.
	std::chrono::milliseconds periodicCheckInterval = std::chrono::hours(1);
};

//! All borrowed; the owner outlives the service. A null member is a programming
//! error and is checked at construction.
struct UpdateServiceDependencies final {
	platform::request::IRequestService* requestService = nullptr;
	IUpdateStagingStore* stagingStore = nullptr;
	IUpdateInstallLocation* installLocation = nullptr;
	IUpdateLauncher* launcher = nullptr;
	IUpdateDigest* digest = nullptr;
	IUpdateExecutor* executor = nullptr;
	//! The automatic-check timer. See `IUpdateScheduler` for why this is a
	//! separate seam from `executor`.
	IUpdateScheduler* scheduler = nullptr;

	[[nodiscard]] bool IsComplete() const noexcept;
};

//! Upstream's transitions over this fork's GitHub Releases feed.
//!
//! Every failure path lands back on `idle` carrying a sentence that says what
//! went wrong. There is deliberately no "probably fine" branch: a size or digest
//! that does not match means no update is offered at all, because the alternative
//! is running an unverified installer against the user's installation.
class UpdateService final : public IUpdateService {
public:
	UpdateService(UpdateServiceDependencies dependencies, UpdateServiceOptions options);
	~UpdateService() override;

	UpdateService(const UpdateService&) = delete;
	UpdateService& operator=(const UpdateService&) = delete;

	//! Derives the opening state from the staging directory: a verified staged
	//! installer for a newer build resumes at `ready`, a recorded failure surfaces
	//! as `idle` plus its diagnostic, and anything stale is deleted.
	void Initialize();

	[[nodiscard]] UpdateState State() const override;

	void CheckForUpdates(bool explicitRequest) override;
	void DownloadUpdate() override;
	void ApplyUpdate() override;
	void QuitAndInstall() override;
	void AbortQuitAndInstall() override;
	void CancelUpdate() override;

	[[nodiscard]] std::optional<UpdateServiceSubscriptionId> Subscribe(UpdateServiceListener listener) override;
	void Unsubscribe(UpdateServiceSubscriptionId subscriptionId) noexcept override;

private:
	struct Impl;
	//! Shared, not unique: posted work holds a weak reference to it. That is what
	//! lets the service be destroyed while a download is still in flight — the
	//! worker finds an expired reference and stops, instead of writing into a
	//! freed object.
	std::shared_ptr<Impl> m_impl;
};

enum class EPendingUpdateOutcome : std::uint8_t {
	//! No manifest, or the manifest is not armed. The overwhelmingly common case.
	NotArmed,
	Launched,
	//! Armed, but the recorded installer or install directory no longer holds up.
	//! The manifest is cleared so the next session does not retry it forever.
	InvalidManifest,
	LaunchFailed,
};

//! Runs the armed installer, if there is one. Called by the last process to
//! exit, which is why it takes its collaborators directly instead of needing a
//! live `UpdateService` during shutdown.
EPendingUpdateOutcome RunPendingUpdate(IUpdateStagingStore& stagingStore, IUpdateLauncher& launcher);

} // namespace update
