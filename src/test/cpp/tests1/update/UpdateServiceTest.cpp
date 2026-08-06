/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sakura/request/RequestService.h>

#include "update/IUpdateService.h"
#include "update/UpdateService.h"
#include "update/UpdateTypes.h"
#include "update/UpdateVersion.h"

namespace {

using platform::request::ERequestOutcome;
using platform::request::ETransportFailure;
using platform::request::HttpResponse;
using platform::request::IRequestCancellation;
using platform::request::IRequestService;
using platform::request::Request;
using platform::request::RequestResult;

using update::EPendingUpdateOutcome;
using update::EUpdateMode;
using update::EUpdateStateType;
using update::EUpdateType;
using update::IUpdateDigest;
using update::IUpdateExecutor;
using update::IUpdateInstallLocation;
using update::IUpdateLauncher;
using update::IUpdateScheduler;
using update::IUpdateStagingStore;
using update::InstallerInvocation;
using update::RunPendingUpdate;
using update::UpdateInstallTarget;
using update::UpdateManifest;
using update::UpdateService;
using update::UpdateServiceDependencies;
using update::UpdateServiceOptions;
using update::UpdateState;
using update::UpdateVersion;

constexpr UpdateVersion kCurrentVersion{ 3, 1, 0, 7221 };
constexpr UpdateVersion kNewerVersion{ 3, 1, 0, 7300 };
constexpr std::uint64_t kPayloadSize = 32;
constexpr std::wstring_view kPayloadDigest = L"8d45e72d30a2294a4d8ae5b9c82c1df424af2fb1c2deeb36264611d6ac5f5109";
constexpr std::wstring_view kInstallDirectory = LR"(C:\Program Files\sakura)";
constexpr std::wstring_view kDownloadUrl =
	L"https://github.com/tsuyoshi-otake/sakura-editor-next/releases/download/"
	L"v3.1.0-build.7300/sakura_install3-1-0-7300-x64.exe";

//! One stable release carrying this architecture's installer, sized and
//! digested to agree with `Payload()` below.
constexpr std::string_view kLatestRelease = R"({
	"tag_name": "v3.1.0-build.7300",
	"draft": false,
	"prerelease": false,
	"html_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300",
	"body": "The seventy-third build.",
	"assets": [
		{
			"name": "sakura_install3-1-0-7300-x64.exe",
			"state": "uploaded",
			"size": 32,
			"digest": "sha256:8d45e72d30a2294a4d8ae5b9c82c1df424af2fb1c2deeb36264611d6ac5f5109",
			"browser_download_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/download/v3.1.0-build.7300/sakura_install3-1-0-7300-x64.exe"
		}
	]
})";

//! The same release with the `digest` field absent. GitHub populates it for
//! every asset today, so this is the shape a future feed change — or a
//! malformed/non-SHA-256 digest that `ParseAssetDigest` refuses — would take.
constexpr std::string_view kLatestReleaseWithoutDigest = R"({
	"tag_name": "v3.1.0-build.7300",
	"draft": false,
	"prerelease": false,
	"html_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300",
	"body": "The seventy-third build.",
	"assets": [
		{
			"name": "sakura_install3-1-0-7300-x64.exe",
			"state": "uploaded",
			"size": 32,
			"browser_download_url": "https://github.com/tsuyoshi-otake/sakura-editor-next/releases/download/v3.1.0-build.7300/sakura_install3-1-0-7300-x64.exe"
		}
	]
})";

std::vector<std::uint8_t> Bytes(std::string_view text)
{
	return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> Payload()
{
	return std::vector<std::uint8_t>(static_cast<std::size_t>(kPayloadSize), std::uint8_t{ 0x53 });
}

RequestResult Ok(std::vector<std::uint8_t> body)
{
	HttpResponse response;
	response.statusCode = 200;
	response.body = std::move(body);
	RequestResult result;
	result.outcome = ERequestOutcome::Success;
	result.response = std::move(response);
	return result;
}

//! Answers the feed URL and the asset URL separately, so a test can break one
//! without disturbing the other, and records what it was asked for.
class FakeRequestService final : public IRequestService {
public:
	RequestResult feedResult = Ok(Bytes(kLatestRelease));
	RequestResult downloadResult = Ok(Payload());
	//! Runs before answering, which is the only way to reach the service while a
	//! request is still in flight under an inline executor.
	std::function<void()> beforeAnswering;

	std::vector<std::wstring> urls;
	bool sawCancellationToken = false;

	RequestResult Execute(const Request& request, const IRequestCancellation* cancellation) override
	{
		urls.push_back(request.url);
		if (cancellation != nullptr) sawCancellationToken = true;
		if (beforeAnswering) beforeAnswering();
		return request.url.find(L"api.github.com") != std::wstring::npos ? feedResult : downloadResult;
	}
};

class FakeStagingStore final : public IUpdateStagingStore {
public:
	std::optional<UpdateManifest> manifest;
	bool installerMatches = true;
	bool storeSucceeds = true;
	bool writeSucceeds = true;

	int storeInstallerCalls = 0;
	int clearManifestCalls = 0;
	std::optional<UpdateVersion> keptBuild;

	std::optional<UpdateManifest> ReadManifest() override { return manifest; }

	bool WriteManifest(const UpdateManifest& written) override
	{
		if (!writeSucceeds) return false;
		manifest = written;
		return true;
	}

	void ClearManifest() noexcept override
	{
		++clearManifestCalls;
		manifest.reset();
	}

	std::optional<std::wstring> StoreInstaller(
		const UpdateVersion& version,
		std::wstring_view assetName,
		const std::vector<std::uint8_t>& bytes) override
	{
		++storeInstallerCalls;
		storedBytes = bytes;
		if (!storeSucceeds) return std::nullopt;
		return LR"(C:\stage\)" + std::to_wstring(version.revision) + L"\\" + std::wstring(assetName);
	}

	bool InstallerMatches(std::wstring_view, std::uint64_t) override { return installerMatches; }

	std::wstring InstallLogPath(const UpdateVersion& version) override
	{
		return LR"(C:\stage\)" + std::to_wstring(version.revision) + LR"(\install.log)";
	}

	void RemoveOtherStagedBuilds(const UpdateVersion& keep) noexcept override { keptBuild = keep; }

	std::vector<std::uint8_t> storedBytes;
};

class FakeInstallLocation final : public IUpdateInstallLocation {
public:
	UpdateInstallTarget target{ EUpdateType::Setup, std::wstring(kInstallDirectory) };
	UpdateInstallTarget Resolve() override { return target; }
};

class FakeLauncher final : public IUpdateLauncher {
public:
	bool launchSucceeds = true;
	std::optional<InstallerInvocation> launched;
	std::wstring openedUrl;

	bool LaunchInstaller(const InstallerInvocation& invocation) override
	{
		launched = invocation;
		return launchSucceeds;
	}

	bool OpenReleasePage(std::wstring_view url) override
	{
		openedUrl = url;
		return true;
	}
};

class FakeDigest final : public IUpdateDigest {
public:
	std::optional<std::wstring> value = std::wstring(kPayloadDigest);
	std::optional<std::wstring> ComputeSha256Hex(const std::vector<std::uint8_t>&) override { return value; }
};

//! Runs the work where it was posted, so every transition is observable by the
//! line after the call that caused it.
class InlineExecutor final : public IUpdateExecutor {
public:
	void Post(std::function<void()> work) override
	{
		if (work) work();
	}
};

//! Records the one delayed item `UpdateService` ever arms, instead of waiting
//! on a real clock. `Fire()` takes ownership of the pending item before
//! invoking it, so a callback that reschedules itself (the periodic-poll case)
//! is observed as a fresh, distinct `pending` rather than this call recursing
//! into its own replacement.
class FakeScheduler final : public IUpdateScheduler {
public:
	struct Pending final {
		std::chrono::milliseconds delay;
		std::function<void()> work;
	};

	std::optional<Pending> pending;
	int postCount = 0;

	void PostDelayed(std::chrono::milliseconds delay, std::function<void()> work) override
	{
		++postCount;
		pending = Pending{ delay, std::move(work) };
	}

	void Fire()
	{
		if (!pending.has_value()) return;
		auto due = std::move(*pending);
		pending.reset();
		if (due.work) due.work();
	}
};

//! Everything one service needs, assembled so a test can reach in and change a
//! single collaborator before constructing the service under test.
struct Harness final {
	FakeRequestService requests;
	FakeStagingStore store;
	FakeInstallLocation installLocation;
	FakeLauncher launcher;
	FakeDigest digest;
	InlineExecutor executor;
	FakeScheduler scheduler;
	std::vector<EUpdateStateType> observed;

	UpdateServiceOptions Options() const
	{
		UpdateServiceOptions options;
		options.currentVersion = kCurrentVersion;
		options.remoteUrl = L"https://github.com/tsuyoshi-otake/sakura-editor-next.git";
		options.architecture = L"x64";
		return options;
	}

	UpdateServiceDependencies Dependencies()
	{
		UpdateServiceDependencies dependencies;
		dependencies.requestService = &requests;
		dependencies.stagingStore = &store;
		dependencies.installLocation = &installLocation;
		dependencies.launcher = &launcher;
		dependencies.digest = &digest;
		dependencies.executor = &executor;
		dependencies.scheduler = &scheduler;
		return dependencies;
	}

	void Observe(UpdateService& service)
	{
		const auto id = service.Subscribe([this](const UpdateState& state) { observed.push_back(state.type); });
		EXPECT_TRUE(id.has_value());
	}
};

//! The manifest a previous session would have left behind for a staged update.
UpdateManifest StagedManifest()
{
	UpdateManifest manifest;
	manifest.version = kNewerVersion;
	manifest.tagName = L"v3.1.0-build.7300";
	manifest.installerPath = LR"(C:\stage\7300\sakura_install3-1-0-7300-x64.exe)";
	manifest.installDirectory = std::wstring(kInstallDirectory);
	manifest.sizeBytes = kPayloadSize;
	manifest.sha256 = std::wstring(kPayloadDigest);
	manifest.releaseUrl = L"https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300";
	return manifest;
}

} // namespace

TEST(UpdateService, WalksFromIdleAllTheWayToReadyWhenBackgroundUpdatesAreEnabled)
{
	Harness harness;
	UpdateService service(harness.Dependencies(), harness.Options());
	harness.Observe(service);

	service.Initialize();
	ASSERT_EQ(EUpdateStateType::Idle, service.State().type);

	service.CheckForUpdates(true);

	const std::vector<EUpdateStateType> expected{
		EUpdateStateType::Idle,
		EUpdateStateType::CheckingForUpdates,
		EUpdateStateType::AvailableForDownload,
		EUpdateStateType::Downloading,
		EUpdateStateType::Downloaded,
		EUpdateStateType::Updating,
		EUpdateStateType::Ready,
	};
	EXPECT_EQ(expected, harness.observed);

	const UpdateState state = service.State();
	EXPECT_EQ(EUpdateStateType::Ready, state.type);
	EXPECT_EQ(EUpdateType::Setup, state.updateType);
	ASSERT_TRUE(state.update.has_value());
	EXPECT_EQ(kNewerVersion, state.update->version);
	EXPECT_EQ(std::wstring(kDownloadUrl), state.update->downloadUrl);
	EXPECT_TRUE(state.reason.empty());

	// The staged payload and the manifest that describes it must both exist, and
	// the update must not be armed until the user asks for the restart.
	EXPECT_EQ(1, harness.store.storeInstallerCalls);
	EXPECT_EQ(Payload(), harness.store.storedBytes);
	ASSERT_TRUE(harness.store.manifest.has_value());
	EXPECT_FALSE(harness.store.manifest->applyOnExit);
	EXPECT_EQ(std::wstring(kInstallDirectory), harness.store.manifest->installDirectory);
	ASSERT_TRUE(harness.store.keptBuild.has_value());
	EXPECT_EQ(kNewerVersion, *harness.store.keptBuild);
	EXPECT_TRUE(harness.requests.sawCancellationToken);
}

TEST(UpdateService, StopsAtAvailableForDownloadUntilTheUserAsksWhenTheModeIsManual)
{
	Harness harness;
	auto options = harness.Options();
	options.mode = EUpdateMode::Manual;
	UpdateService service(harness.Dependencies(), options);
	harness.Observe(service);
	service.Initialize();

	// `manual` suppresses the poll but not the user.
	service.CheckForUpdates(false);
	EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
	EXPECT_TRUE(harness.requests.urls.empty());

	service.CheckForUpdates(true);
	EXPECT_EQ(EUpdateStateType::AvailableForDownload, service.State().type);
	EXPECT_EQ(1u, harness.requests.urls.size());
	EXPECT_EQ(
		L"https://api.github.com/repos/tsuyoshi-otake/sakura-editor-next/releases/latest",
		harness.requests.urls.front());
	EXPECT_EQ(0, harness.store.storeInstallerCalls);

	service.DownloadUpdate();
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	EXPECT_EQ(1, harness.store.storeInstallerCalls);
}

TEST(UpdateService, ReportsDisabledAndChecksNothingWhenTheModeIsNone)
{
	Harness harness;
	auto options = harness.Options();
	options.mode = EUpdateMode::None;
	UpdateService service(harness.Dependencies(), options);
	service.Initialize();

	EXPECT_EQ(EUpdateStateType::Disabled, service.State().type);

	service.CheckForUpdates(true);
	EXPECT_EQ(EUpdateStateType::Disabled, service.State().type);
	EXPECT_TRUE(harness.requests.urls.empty());
}

TEST(UpdateService, ReportsDisabledWhenTheBuildDidNotComposeTheUpdateStack)
{
	Harness harness;
	auto dependencies = harness.Dependencies();
	dependencies.launcher = nullptr;
	UpdateService service(dependencies, harness.Options());
	service.Initialize();

	const UpdateState state = service.State();
	EXPECT_EQ(EUpdateStateType::Disabled, state.type);
	EXPECT_EQ(L"Updating is not available in this build.", state.reason);
}

TEST(UpdateService, DiscardsADownloadWhoseChecksumOrSizeDisagreesWithTheRelease)
{
	{
		Harness harness;
		harness.digest.value = L"0000000000000000000000000000000000000000000000000000000000000000";
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		const UpdateState state = service.State();
		EXPECT_EQ(EUpdateStateType::Idle, state.type);
		EXPECT_EQ(
			L"The downloaded update did not match the checksum the release published, so it was discarded.",
			state.reason);
		// Nothing unverified reaches the profile.
		EXPECT_EQ(0, harness.store.storeInstallerCalls);
		EXPECT_FALSE(harness.store.manifest.has_value());
	}
	{
		Harness harness;
		harness.requests.downloadResult =
			Ok(std::vector<std::uint8_t>(static_cast<std::size_t>(kPayloadSize) + 1, std::uint8_t{ 0x53 }));
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		const UpdateState state = service.State();
		EXPECT_EQ(EUpdateStateType::Idle, state.type);
		EXPECT_EQ(
			L"The downloaded update was not the size the release said it would be, so it was discarded.",
			state.reason);
		EXPECT_EQ(0, harness.store.storeInstallerCalls);
	}
	{
		// A digest that cannot be computed is a failure, not a reason to trust the
		// size alone.
		Harness harness;
		harness.digest.value.reset();
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		const UpdateState state = service.State();
		EXPECT_EQ(EUpdateStateType::Idle, state.type);
		EXPECT_EQ(L"The downloaded update could not be verified, so it was discarded.", state.reason);
	}
	{
		// A release that publishes no digest at all is the same refusal. The size
		// agreeing proves only that the right number of bytes arrived, and the very
		// next step would be running them as an installer. `ParseAssetDigest` also
		// yields no digest for a malformed or non-SHA-256 `digest` field, so this
		// branch covers that shape too.
		Harness harness;
		harness.requests.feedResult = Ok(Bytes(kLatestReleaseWithoutDigest));
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		const UpdateState state = service.State();
		EXPECT_EQ(EUpdateStateType::Idle, state.type);
		EXPECT_EQ(
			L"The release published no checksum for this update, so it was discarded.", state.reason);
		// Nothing unverified reaches the profile.
		EXPECT_EQ(0, harness.store.storeInstallerCalls);
		EXPECT_FALSE(harness.store.manifest.has_value());
	}
}

TEST(UpdateService, SendsAnArchiveBuildToTheReleasePageInsteadOfInstallingOverIt)
{
	Harness harness;
	harness.installLocation.target = UpdateInstallTarget{ EUpdateType::Archive, std::wstring() };
	UpdateService service(harness.Dependencies(), harness.Options());
	service.Initialize();
	service.CheckForUpdates(true);

	UpdateState state = service.State();
	ASSERT_EQ(EUpdateStateType::AvailableForDownload, state.type);
	EXPECT_EQ(EUpdateType::Archive, state.updateType);
	EXPECT_FALSE(state.reason.empty());

	service.DownloadUpdate();

	// The browser opened and the state deliberately did not move: nothing was
	// downloaded, so claiming otherwise would be a fake capability.
	EXPECT_EQ(
		L"https://github.com/tsuyoshi-otake/sakura-editor-next/releases/tag/v3.1.0-build.7300",
		harness.launcher.openedUrl);
	EXPECT_EQ(EUpdateStateType::AvailableForDownload, service.State().type);
	EXPECT_EQ(0, harness.store.storeInstallerCalls);
	EXPECT_EQ(1u, harness.requests.urls.size());
}

TEST(UpdateService, UnwindsToIdleWhenTheCheckIsCancelledWhileItIsInFlight)
{
	Harness harness;
	UpdateService service(harness.Dependencies(), harness.Options());
	harness.Observe(service);
	service.Initialize();

	harness.requests.beforeAnswering = [&service] { service.CancelUpdate(); };
	service.CheckForUpdates(true);

	const std::vector<EUpdateStateType> expected{
		EUpdateStateType::Idle,
		EUpdateStateType::CheckingForUpdates,
		EUpdateStateType::Cancelling,
		EUpdateStateType::Idle,
	};
	EXPECT_EQ(expected, harness.observed);
	EXPECT_EQ(0, harness.store.storeInstallerCalls);

	// The token is reset by the unwind, so the next check is not cancelled by the
	// last one.
	harness.requests.beforeAnswering = nullptr;
	service.CheckForUpdates(true);
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
}

TEST(UpdateService, ArmsTheStagedUpdateOnRestartAndDisarmsItWhenTheQuitIsCancelled)
{
	Harness harness;
	UpdateService service(harness.Dependencies(), harness.Options());
	service.Initialize();
	service.CheckForUpdates(true);
	ASSERT_EQ(EUpdateStateType::Ready, service.State().type);

	service.QuitAndInstall();
	EXPECT_EQ(EUpdateStateType::Restarting, service.State().type);
	ASSERT_TRUE(harness.store.manifest.has_value());
	EXPECT_TRUE(harness.store.manifest->applyOnExit);

	service.AbortQuitAndInstall();
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	ASSERT_TRUE(harness.store.manifest.has_value());
	// The user cancelled the quit, so an ordinary exit minutes from now must not
	// run the installer.
	EXPECT_FALSE(harness.store.manifest->applyOnExit);
}

TEST(UpdateService, ResumesAtReadyFromAStagedManifestWithoutTouchingTheNetwork)
{
	Harness harness;
	harness.store.manifest = StagedManifest();
	UpdateService service(harness.Dependencies(), harness.Options());
	service.Initialize();

	const UpdateState state = service.State();
	EXPECT_EQ(EUpdateStateType::Ready, state.type);
	ASSERT_TRUE(state.update.has_value());
	EXPECT_EQ(kNewerVersion, state.update->version);
	EXPECT_EQ(L"sakura_install3-1-0-7300-x64.exe", state.update->assetName);
	EXPECT_TRUE(harness.requests.urls.empty());
	EXPECT_EQ(0, harness.store.clearManifestCalls);
}

TEST(UpdateService, ForgetsAStagedManifestThatNoLongerDescribesAnythingInstallable)
{
	{
		// The update already applied: this build is no longer older than the stage.
		Harness harness;
		auto manifest = StagedManifest();
		manifest.version = kCurrentVersion;
		harness.store.manifest = manifest;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_TRUE(service.State().reason.empty());
		EXPECT_EQ(1, harness.store.clearManifestCalls);
	}
	{
		// The payload was cleaned up out from under the manifest.
		Harness harness;
		harness.store.manifest = StagedManifest();
		harness.store.installerMatches = false;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_EQ(1, harness.store.clearManifestCalls);
	}
	{
		// A recorded failure is reported once and then forgotten, so a broken
		// installer cannot make every future session open with the same complaint.
		Harness harness;
		auto manifest = StagedManifest();
		manifest.lastFailure = L"The update installer could not be started, so this editor was not updated.";
		harness.store.manifest = manifest;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_EQ(
			L"The update installer could not be started, so this editor was not updated.",
			service.State().reason);
		EXPECT_EQ(1, harness.store.clearManifestCalls);
		EXPECT_FALSE(harness.store.manifest.has_value());
	}
}

TEST(UpdateService, NamesTheTransportFailureRatherThanSayingTheCheckJustFailed)
{
	{
		Harness harness;
		RequestResult refused;
		refused.outcome = ERequestOutcome::TlsCertificateFailure;
		refused.transportFailure = ETransportFailure::TlsCertificateFailure;
		harness.requests.feedResult = refused;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_EQ(L"The update server's certificate could not be verified.", service.State().reason);
	}
	{
		// A repository that has published nothing at all is "no update", not an
		// error worth alarming the user about.
		Harness harness;
		HttpResponse notFound;
		notFound.statusCode = 404;
		RequestResult missing;
		missing.outcome = ERequestOutcome::Success;
		missing.response = notFound;
		harness.requests.feedResult = missing;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_TRUE(service.State().reason.empty());
	}
	{
		Harness harness;
		HttpResponse throttled;
		throttled.statusCode = 403;
		RequestResult refused;
		refused.outcome = ERequestOutcome::Success;
		refused.response = throttled;
		harness.requests.feedResult = refused;
		UpdateService service(harness.Dependencies(), harness.Options());
		service.Initialize();
		service.CheckForUpdates(true);

		EXPECT_EQ(L"The update server answered with status 403.", service.State().reason);
	}
	{
		// A build with no recorded origin checks nothing and says so.
		Harness harness;
		auto options = harness.Options();
		options.remoteUrl.clear();
		UpdateService service(harness.Dependencies(), options);
		service.Initialize();
		service.CheckForUpdates(true);

		EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
		EXPECT_EQ(
			L"This build does not record a GitHub repository to check for updates.",
			service.State().reason);
		EXPECT_TRUE(harness.requests.urls.empty());
	}
}

TEST(UpdateService, RefusesToRestartAnUpdateThatIsAlreadyUnderway)
{
	Harness harness;
	UpdateService service(harness.Dependencies(), harness.Options());
	service.Initialize();
	service.CheckForUpdates(true);
	ASSERT_EQ(EUpdateStateType::Ready, service.State().type);

	// Re-checking from `ready` would throw the finished download away.
	service.CheckForUpdates(true);
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	EXPECT_EQ(2u, harness.requests.urls.size());

	// And the two transitions that only make sense earlier are refused here.
	service.DownloadUpdate();
	service.ApplyUpdate();
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	EXPECT_EQ(1, harness.store.storeInstallerCalls);
}

TEST(UpdateService, ArmsTheStartupCheckOnlyWhenTheModeAllowsAnAutomaticCheck)
{
	{
		// `manual` suppresses the poll entirely, not just the walk past
		// `available for download`.
		Harness harness;
		auto options = harness.Options();
		options.mode = EUpdateMode::Manual;
		UpdateService service(harness.Dependencies(), options);
		service.Initialize();
		EXPECT_FALSE(harness.scheduler.pending.has_value());
	}
	{
		Harness harness;
		auto options = harness.Options();
		options.mode = EUpdateMode::None;
		UpdateService service(harness.Dependencies(), options);
		service.Initialize();
		EXPECT_FALSE(harness.scheduler.pending.has_value());
	}
	{
		Harness harness;
		auto options = harness.Options();
		options.mode = EUpdateMode::Start;
		UpdateService service(harness.Dependencies(), options);
		service.Initialize();
		ASSERT_TRUE(harness.scheduler.pending.has_value());
		EXPECT_EQ(options.initialCheckDelay, harness.scheduler.pending->delay);
	}
	{
		Harness harness;
		auto options = harness.Options();
		options.mode = EUpdateMode::Default;
		UpdateService service(harness.Dependencies(), options);
		service.Initialize();
		ASSERT_TRUE(harness.scheduler.pending.has_value());
		EXPECT_EQ(options.initialCheckDelay, harness.scheduler.pending->delay);
	}
}

TEST(UpdateService, TheAutomaticTimerRunsExactlyOnceWhenTheModeIsStart)
{
	Harness harness;
	auto options = harness.Options();
	options.mode = EUpdateMode::Start;
	UpdateService service(harness.Dependencies(), options);
	service.Initialize();
	ASSERT_TRUE(harness.scheduler.pending.has_value());
	EXPECT_EQ(0u, harness.requests.urls.size());

	// Firing the startup timer is indistinguishable, from the state machine's
	// point of view, from the user clicking Check for Updates: `start` still
	// walks all the way to `ready` because background updates are enabled.
	// That walk costs two requests — the feed, then the download — exactly as
	// an explicit check does.
	harness.scheduler.Fire();
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	EXPECT_EQ(2u, harness.requests.urls.size());
	// `start` checks once and stops: reaching `ready` is not an idle-settle
	// point, so nothing rearms the timer for a second, unrequested check.
	EXPECT_FALSE(harness.scheduler.pending.has_value());
}

TEST(UpdateService, TheAutomaticTimerKeepsRearmingItselfWhenTheModeIsDefault)
{
	Harness harness;
	// A repository with nothing published settles straight back at `idle`,
	// which is the case that must rearm the recurring poll.
	HttpResponse notFound;
	notFound.statusCode = 404;
	RequestResult missing;
	missing.outcome = ERequestOutcome::Success;
	missing.response = notFound;
	harness.requests.feedResult = missing;

	auto options = harness.Options();
	options.mode = EUpdateMode::Default;
	UpdateService service(harness.Dependencies(), options);
	service.Initialize();
	ASSERT_TRUE(harness.scheduler.pending.has_value());
	EXPECT_EQ(options.initialCheckDelay, harness.scheduler.pending->delay);

	harness.scheduler.Fire();
	EXPECT_EQ(EUpdateStateType::Idle, service.State().type);
	EXPECT_EQ(1u, harness.requests.urls.size());
	// The one-time startup delay is gone; `default` rearmed the recurring
	// interval in its place instead of going silent after the first tick.
	ASSERT_TRUE(harness.scheduler.pending.has_value());
	EXPECT_EQ(options.periodicCheckInterval, harness.scheduler.pending->delay);

	harness.scheduler.Fire();
	EXPECT_EQ(2u, harness.requests.urls.size());
	ASSERT_TRUE(harness.scheduler.pending.has_value());
	EXPECT_EQ(options.periodicCheckInterval, harness.scheduler.pending->delay);
}

TEST(UpdateService, TheAutomaticTimerRearmsRatherThanGoingSilentWhenAnExplicitCheckIsAlreadyInFlight)
{
	Harness harness;
	auto options = harness.Options();
	options.mode = EUpdateMode::Default;
	UpdateService service(harness.Dependencies(), options);
	service.Initialize();
	ASSERT_TRUE(harness.scheduler.pending.has_value());

	// The user's own explicit check walks all the way to `ready` before the
	// startup timer this constructor armed ever fires.
	service.CheckForUpdates(true);
	ASSERT_EQ(EUpdateStateType::Ready, service.State().type);
	const auto urlsBeforeTick = harness.requests.urls.size();

	// The startup timer firing while the service sits at `ready`, not `idle`,
	// must not start a second, overlapping check on top of the finished one —
	// and it must not go silent either: `default`'s polling would otherwise
	// permanently stop the first time it raced a user click.
	harness.scheduler.Fire();
	EXPECT_EQ(EUpdateStateType::Ready, service.State().type);
	EXPECT_EQ(urlsBeforeTick, harness.requests.urls.size());
	ASSERT_TRUE(harness.scheduler.pending.has_value());
	EXPECT_EQ(options.periodicCheckInterval, harness.scheduler.pending->delay);
}

TEST(RunPendingUpdate, StartsTheInstallerOnlyWhenTheManifestIsArmedAndStillValid)
{
	{
		FakeStagingStore store;
		FakeLauncher launcher;
		EXPECT_EQ(EPendingUpdateOutcome::NotArmed, RunPendingUpdate(store, launcher));
		EXPECT_FALSE(launcher.launched.has_value());

		store.manifest = StagedManifest();
		EXPECT_EQ(EPendingUpdateOutcome::NotArmed, RunPendingUpdate(store, launcher));
		EXPECT_FALSE(launcher.launched.has_value());
	}
	{
		FakeStagingStore store;
		FakeLauncher launcher;
		auto manifest = StagedManifest();
		manifest.applyOnExit = true;
		store.manifest = manifest;

		EXPECT_EQ(EPendingUpdateOutcome::Launched, RunPendingUpdate(store, launcher));
		ASSERT_TRUE(launcher.launched.has_value());
		EXPECT_EQ(manifest.installerPath, launcher.launched->installerPath);
		EXPECT_EQ(std::wstring(kInstallDirectory), launcher.launched->installDirectory);
		EXPECT_EQ(LR"(C:\stage\7300\install.log)", launcher.launched->logPath);
		EXPECT_TRUE(launcher.launched->relaunchAfterInstall);
		// The manifest deliberately stays armed: the next session's `Initialize`
		// sees a version that is no longer newer and clears it.
		ASSERT_TRUE(store.manifest.has_value());
		EXPECT_TRUE(store.manifest->applyOnExit);
	}
}

TEST(RunPendingUpdate, DisarmsAndExplainsItselfWhenTheStagedInstallerCannotRun)
{
	{
		FakeStagingStore store;
		FakeLauncher launcher;
		auto manifest = StagedManifest();
		manifest.applyOnExit = true;
		store.manifest = manifest;
		store.installerMatches = false;

		EXPECT_EQ(EPendingUpdateOutcome::InvalidManifest, RunPendingUpdate(store, launcher));
		EXPECT_FALSE(launcher.launched.has_value());
		EXPECT_FALSE(store.manifest.has_value());
	}
	{
		// An invocation Setup would misread must never be built at all.
		FakeStagingStore store;
		FakeLauncher launcher;
		auto manifest = StagedManifest();
		manifest.applyOnExit = true;
		manifest.installerPath = L"sakura_install3-1-0-7300-x64.exe";
		store.manifest = manifest;

		EXPECT_EQ(EPendingUpdateOutcome::InvalidManifest, RunPendingUpdate(store, launcher));
		EXPECT_FALSE(launcher.launched.has_value());
		EXPECT_FALSE(store.manifest.has_value());
	}
	{
		FakeStagingStore store;
		FakeLauncher launcher;
		launcher.launchSucceeds = false;
		auto manifest = StagedManifest();
		manifest.applyOnExit = true;
		store.manifest = manifest;

		EXPECT_EQ(EPendingUpdateOutcome::LaunchFailed, RunPendingUpdate(store, launcher));
		ASSERT_TRUE(store.manifest.has_value());
		EXPECT_FALSE(store.manifest->applyOnExit);
		EXPECT_EQ(
			L"The update installer could not be started, so this editor was not updated.",
			store.manifest->lastFailure);
	}
}
