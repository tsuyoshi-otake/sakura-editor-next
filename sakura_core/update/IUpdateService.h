/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/UpdateInstallerCommandLine.h"
#include "update/UpdateTypes.h"
#include "update/UpdateVersion.h"

//! The seams the update state machine is built on. Every side effect an update
//! has — network, disk, registry, process creation — enters through one of the
//! interfaces below, so the state machine itself can be exercised without any
//! of them.
namespace update {

//! What survives a restart. The staging directory holds one of these beside the
//! installer it describes, which is what lets a new process work out, from disk
//! alone, that an update is already downloaded, already staged, or was tried and
//! failed.
struct UpdateManifest final {
	//! The build the staged installer would move this installation to.
	UpdateVersion version;
	std::wstring tagName;
	//! Full path to the staged `sakura_install*-x64.exe`.
	std::wstring installerPath;
	//! The directory Setup must reinstall into, captured at staging time so the
	//! exiting process does not have to re-read the registry while shutting down.
	std::wstring installDirectory;
	std::uint64_t sizeBytes = 0;
	//! Lowercase hex; empty when the release carried no digest.
	std::wstring sha256;
	//! Kept so a session that starts with an already-staged update can still
	//! offer `update.showUpdateInfo` without re-fetching the feed.
	std::wstring releaseUrl;
	//! Set by `update.restart` and cleared if the quit is cancelled. The last
	//! process to exit runs the installer only when this is set.
	bool applyOnExit = false;
	//! Populated when a previous attempt failed, so the next session can explain
	//! itself instead of silently offering the same update again.
	std::wstring lastFailure;

	[[nodiscard]] bool operator==(const UpdateManifest&) const = default;
};

//! Where downloads live and what is known about them. The Win32 implementation
//! roots everything at `%LOCALAPPDATA%\sakura-editor-next\update\<revision>\`.
class IUpdateStagingStore {
public:
	virtual ~IUpdateStagingStore() = default;

	[[nodiscard]] virtual std::optional<UpdateManifest> ReadManifest() = 0;
	[[nodiscard]] virtual bool WriteManifest(const UpdateManifest& manifest) = 0;
	virtual void ClearManifest() noexcept = 0;

	//! Writes the downloaded bytes and returns the resulting full path. Returns
	//! nothing on any I/O failure; a partially written installer must never be
	//! reported as stored.
	[[nodiscard]] virtual std::optional<std::wstring> StoreInstaller(
		const UpdateVersion& version,
		std::wstring_view assetName,
		const std::vector<std::uint8_t>& bytes) = 0;

	//! Whether the recorded installer is still on disk at exactly the recorded
	//! size. A manifest whose payload was cleaned up must not survive as `Ready`.
	[[nodiscard]] virtual bool InstallerMatches(std::wstring_view installerPath, std::uint64_t sizeBytes) = 0;

	//! Where Setup should write its own log for this build.
	[[nodiscard]] virtual std::wstring InstallLogPath(const UpdateVersion& version) = 0;

	//! Deletes staged builds other than `keep`, so an abandoned download cannot
	//! accumulate installers in the user's profile.
	virtual void RemoveOtherStagedBuilds(const UpdateVersion& keep) noexcept = 0;
};

//! Whether this installation can update itself, and where to.
struct UpdateInstallTarget final {
	EUpdateType type = EUpdateType::Archive;
	//! Where Setup has to install: the running copy's own directory when it is
	//! itself an installation, otherwise the `InstallLocation` recorded in the
	//! uninstall key. Empty for `Archive`.
	std::wstring installDirectory;

	[[nodiscard]] bool operator==(const UpdateInstallTarget&) const = default;
};

//! Decides whether an update can be installed and where. `Setup` when the
//! running copy is an Inno installation, and also when it is not but a real
//! installation is recorded on this computer. `Archive` only when no install
//! directory can be determined at all.
class IUpdateInstallLocation {
public:
	virtual ~IUpdateInstallLocation() = default;
	[[nodiscard]] virtual UpdateInstallTarget Resolve() = 0;
};

//! The only component that starts processes.
class IUpdateLauncher {
public:
	virtual ~IUpdateLauncher() = default;

	//! Starts the staged installer detached. Called by the last process to exit.
	[[nodiscard]] virtual bool LaunchInstaller(const InstallerInvocation& invocation) = 0;

	//! Opens the release page in the user's browser. This is the whole of the
	//! `Archive` download path: with no installation anywhere to write to, the
	//! user is sent to the release rather than given a fake in-app install.
	[[nodiscard]] virtual bool OpenReleasePage(std::wstring_view url) = 0;
};

//! SHA-256 over the downloaded bytes, lowercase hex. Injected so the verifier
//! can be exercised with a known-wrong digest without touching BCrypt.
class IUpdateDigest {
public:
	virtual ~IUpdateDigest() = default;
	[[nodiscard]] virtual std::optional<std::wstring> ComputeSha256Hex(const std::vector<std::uint8_t>& bytes) = 0;
};

//! Runs one unit of update work off the caller's thread. The production
//! implementation is a single worker thread; tests supply an inline executor, so
//! every transition below is observable synchronously.
class IUpdateExecutor {
public:
	virtual ~IUpdateExecutor() = default;
	virtual void Post(std::function<void()> work) = 0;
};

//! Delayed work for the update service's own automatic-check timer. This is a
//! distinct seam from `IUpdateExecutor::Post`: that one runs a unit of work
//! right away, on the worker thread; this one is "not yet, later". It exists so
//! `update.mode`'s `start` (check once, shortly after startup) and `default`
//! (keep polling) are an observed side effect instead of two decision functions
//! (`AllowsAutomaticCheck`, `AllowsPeriodicCheck`) that nothing ever calls. A
//! test fakes this one seam to make that distinction observable without a real
//! clock; see `sakura_core/update/CLAUDE.md`.
class IUpdateScheduler {
public:
	virtual ~IUpdateScheduler() = default;

	//! Runs `work` once, after `delay`, unless a later call to `PostDelayed`
	//! replaces it first. There is only ever one pending item: the state machine
	//! never wants two automatic checks racing each other.
	virtual void PostDelayed(std::chrono::milliseconds delay, std::function<void()> work) = 0;
};

using UpdateServiceSubscriptionId = std::uint64_t;
using UpdateServiceListener = std::function<void(const UpdateState&)>;

//! Upstream `IUpdateService`, minus the members that only exist for providers
//! this platform does not have. Divergences are recorded in
//! `sakura_core/update/CLAUDE.md`.
class IUpdateService {
public:
	virtual ~IUpdateService() = default;

	[[nodiscard]] virtual UpdateState State() const = 0;

	//! `explicitRequest` distinguishes the user asking from the poll asking, and
	//! it is what lets `update.mode` = `manual` suppress the latter only.
	virtual void CheckForUpdates(bool explicitRequest) = 0;
	virtual void DownloadUpdate() = 0;
	virtual void ApplyUpdate() = 0;

	//! Arms the staged update and moves to `restarting`. The quit itself belongs
	//! to the workbench; if the user cancels it, `AbortQuitAndInstall` puts the
	//! service back where it was.
	virtual void QuitAndInstall() = 0;
	virtual void AbortQuitAndInstall() = 0;

	//! Only meaningful while `downloading`.
	virtual void CancelUpdate() = 0;

	//! Returns nothing when the subscription bound is reached, so a leaking
	//! caller fails loudly instead of growing the listener list without limit.
	[[nodiscard]] virtual std::optional<UpdateServiceSubscriptionId> Subscribe(UpdateServiceListener listener) = 0;
	virtual void Unsubscribe(UpdateServiceSubscriptionId subscriptionId) noexcept = 0;
};

} // namespace update
