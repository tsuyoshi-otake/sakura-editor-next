/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateService.h"

#include "update/UpdateDigest.h"
#include "update/UpdateFeed.h"

#include <sakura/request/RequestService.h>
#include <sakura/serialization/JsoncDocument.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

namespace update {
namespace {

using platform::request::ERequestOutcome;
using platform::request::ETransportFailure;
using platform::request::HttpHeader;
using platform::request::IRequestCancellation;
using platform::request::Request;
using platform::request::RequestResult;

//! The one cancellation token an update owns. `CancelUpdate` sets it and the
//! worker, wherever it happens to be inside the request stack, unwinds.
class UpdateCancellation final : public IRequestCancellation {
public:
	bool IsCancellationRequested() const noexcept override
	{
		return m_requested.load(std::memory_order_acquire);
	}

	void Request() noexcept { m_requested.store(true, std::memory_order_release); }
	void Reset() noexcept { m_requested.store(false, std::memory_order_release); }

private:
	std::atomic<bool> m_requested{ false };
};

//! Names the transport failure rather than collapsing everything into "the
//! update check failed". A proxy refusal and an expired certificate need
//! different actions from the user.
std::wstring DescribeRequestFailure(const RequestResult& result)
{
	switch (result.outcome) {
	case ERequestOutcome::Cancelled:
		return L"The update check was cancelled.";
	case ERequestOutcome::InvalidRequest:
		return L"The update location is not a valid request.";
	case ERequestOutcome::OfflineCacheMiss:
		return L"No cached update information is available while offline.";
	case ERequestOutcome::RedirectLimitExceeded:
	case ERequestOutcome::InvalidRedirect:
		return L"The update download was redirected in a way that could not be followed.";
	case ERequestOutcome::HttpsDowngradeRejected:
		return L"The update download was redirected to an insecure address and was refused.";
	case ERequestOutcome::Timeout:
		return L"The update request timed out.";
	case ERequestOutcome::ResponseHeaderLimitExceeded:
	case ERequestOutcome::ResponseBodyLimitExceeded:
		return L"The update response was larger than this editor will read.";
	case ERequestOutcome::ServerAuthenticationRequired:
		return L"The update server requires authentication.";
	case ERequestOutcome::ProxyAuthenticationRequired:
		return L"The configured proxy requires authentication.";
	case ERequestOutcome::UnsupportedProxyPolicy:
		return L"The configured proxy policy is not supported.";
	case ERequestOutcome::TlsCertificateFailure:
		return L"The update server's certificate could not be verified.";
	case ERequestOutcome::TransportFailure:
	default:
		break;
	}
	if (result.transportFailure == ETransportFailure::Timeout) {
		return L"The update request timed out.";
	}
	return L"The update server could not be reached.";
}

} // namespace

bool AllowsAutomaticCheck(EUpdateMode mode) noexcept
{
	return mode == EUpdateMode::Start || mode == EUpdateMode::Default;
}

bool AllowsPeriodicCheck(EUpdateMode mode) noexcept
{
	return mode == EUpdateMode::Default;
}

bool UpdateServiceDependencies::IsComplete() const noexcept
{
	return requestService != nullptr
		&& stagingStore != nullptr
		&& installLocation != nullptr
		&& launcher != nullptr
		&& digest != nullptr
		&& executor != nullptr
		&& scheduler != nullptr;
}

struct UpdateService::Impl final {
	UpdateServiceDependencies dependencies;
	UpdateServiceOptions options;

	mutable std::mutex mutex;
	UpdateState state;
	UpdateInstallTarget installTarget;
	std::vector<std::pair<UpdateServiceSubscriptionId, UpdateServiceListener>> subscriptions;
	UpdateServiceSubscriptionId nextSubscriptionId = 1;
	UpdateCancellation cancellation;
	//! Set by `UpdateService`'s constructor, right after the surrounding
	//! `shared_ptr` exists. Lets `Impl`'s own methods arm a delayed self-check
	//! without a raw `this` capture, which would dangle if the service were
	//! destroyed before the timer fired.
	std::weak_ptr<Impl> self;

	Impl(UpdateServiceDependencies suppliedDependencies, UpdateServiceOptions suppliedOptions)
		: dependencies(std::move(suppliedDependencies))
		, options(std::move(suppliedOptions))
	{
	}

	[[nodiscard]] UpdateState Snapshot() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return state;
	}

	//! Assigns and notifies. Listeners are copied under the lock and invoked
	//! without it, so a listener that calls back into the service — which is
	//! exactly what the window's projection does — cannot deadlock.
	void Publish(UpdateState next)
	{
		std::vector<UpdateServiceListener> listeners;
		UpdateState published;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (state == next) return;
			state = std::move(next);
			published = state;
			listeners.reserve(subscriptions.size());
			for (const auto& entry : subscriptions) {
				listeners.push_back(entry.second);
			}
		}
		for (auto& listener : listeners) {
			if (listener) listener(published);
		}
	}

	void PublishIdle(std::wstring reason)
	{
		UpdateState next;
		next.type = EUpdateStateType::Idle;
		next.updateType = installTarget.type;
		next.reason = std::move(reason);
		Publish(std::move(next));
	}

	//! Arms the update service's own delayed self-check. `initial` selects
	//! between `update.mode`'s two distinct timers: the one-time startup check
	//! (`AllowsAutomaticCheck`; `start` and `default`) and the recurring poll
	//! that only `default` rearms after every settle back to `idle`
	//! (`AllowsPeriodicCheck`). A build with an incomplete dependency set —
	//! which `IsComplete` already refuses to run at all — simply never arms
	//! anything.
	void ScheduleNext(bool initial)
	{
		if (!dependencies.scheduler) return;
		const bool eligible = initial ? AllowsAutomaticCheck(options.mode) : AllowsPeriodicCheck(options.mode);
		if (!eligible) return;
		const auto delay = initial ? options.initialCheckDelay : options.periodicCheckInterval;
		std::weak_ptr<Impl> weak = self;
		dependencies.scheduler->PostDelayed(delay, [weak] {
			if (const auto impl = weak.lock()) impl->TriggerCheck(false);
		});
	}

	//! `Initialize()`'s three ways of landing at `idle` all want the startup
	//! timer, never the recurring one — `start` mode must still get its one
	//! automatic check even though `Initialize()` never reaches
	//! `PublishIdleAndSchedulePeriodicCheck`.
	void PublishIdleAndScheduleInitialCheck(std::wstring reason)
	{
		PublishIdle(std::move(reason));
		ScheduleNext(/*initial=*/true);
	}

	//! Every place a check/download/apply cycle (or its cancellation) settles
	//! back at `idle` wants the recurring timer rearmed, regardless of whether
	//! the cycle that just finished was the user's own click or the automatic
	//! poll — upstream's poll timer runs on a fixed cadence, not "N minutes
	//! since the last automatic tick specifically".
	void PublishIdleAndSchedulePeriodicCheck(std::wstring reason)
	{
		PublishIdle(std::move(reason));
		ScheduleNext(/*initial=*/false);
	}

	//! `CancelUpdate` publishes `cancelling`; this is the worker noticing and
	//! finishing the transition to `idle`. Returns whether the caller should stop.
	bool ObserveCancellation()
	{
		if (!cancellation.IsCancellationRequested()) return false;
		cancellation.Reset();
		PublishIdleAndSchedulePeriodicCheck(std::wstring());
		return true;
	}

	[[nodiscard]] std::optional<Update> CurrentUpdate() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return state.update;
	}

	void TriggerCheck(bool explicitRequest);
	void RunCheck();
	void RunDownload();
	void RunApply();

	[[nodiscard]] bool VerifyPayload(const Update& candidate, const std::vector<std::uint8_t>& bytes, std::wstring& reason);
};

void UpdateService::Impl::RunCheck()
{
	if (ObserveCancellation()) return;

	const auto repository = ParseGitHubRemoteUrl(options.remoteUrl);
	if (!repository) {
		PublishIdleAndSchedulePeriodicCheck(L"This build does not record a GitHub repository to check for updates.");
		return;
	}

	Request request;
	request.method = L"GET";
	request.url = BuildLatestReleaseUrl(*repository);
	request.headers.push_back(HttpHeader{ L"Accept", L"application/vnd.github+json" });
	request.headers.push_back(HttpHeader{ L"X-GitHub-Api-Version", L"2022-11-28" });
	request.headers.push_back(HttpHeader{ L"User-Agent", options.userAgent });
	request.limits.timeout = std::chrono::seconds(30);
	request.limits.maxResponseBodyBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes;

	const RequestResult result = dependencies.requestService->Execute(request, &cancellation);
	if (ObserveCancellation()) return;

	if (!result || !result.response) {
		PublishIdleAndSchedulePeriodicCheck(DescribeRequestFailure(result));
		return;
	}
	// GitHub answers 404 for a repository that has published no release at all,
	// which is "no update" rather than a failure worth alarming the user about.
	if (result.response->statusCode == 404) {
		PublishIdleAndSchedulePeriodicCheck(std::wstring());
		return;
	}
	if (result.response->statusCode != 200) {
		PublishIdleAndSchedulePeriodicCheck(L"The update server answered with status "
			+ std::to_wstring(result.response->statusCode) + L".");
		return;
	}

	UpdateFeedRequest feedRequest;
	feedRequest.currentVersion = options.currentVersion;
	feedRequest.architecture = options.architecture;

	const std::string_view body(
		reinterpret_cast<const char*>(result.response->body.data()), result.response->body.size());
	const UpdateFeedResult feed = ParseGitHubReleaseFeed(body, feedRequest);

	switch (feed.outcome) {
	case EUpdateFeedOutcome::NoUpdateAvailable:
		PublishIdleAndSchedulePeriodicCheck(std::wstring());
		return;
	case EUpdateFeedOutcome::UpdateAvailable:
		break;
	default:
		PublishIdleAndSchedulePeriodicCheck(feed.diagnostic);
		return;
	}

	installTarget = dependencies.installLocation->Resolve();

	UpdateState next;
	next.type = EUpdateStateType::AvailableForDownload;
	next.updateType = installTarget.type;
	next.update = feed.update;
	if (installTarget.type == EUpdateType::Archive) {
		next.reason = L"No installed copy of Sakura Editor NEXT could be found on this computer, "
			L"so the update has to be downloaded from the release page.";
	}
	Publish(std::move(next));

	// With no installation to write to there is nothing to walk forward into: the
	// user has to go to the release page, which is upstream's archive behaviour.
	if (installTarget.type != EUpdateType::Setup) return;
	if (!options.enableWindowsBackgroundUpdates) return;
	if (options.mode == EUpdateMode::Manual || options.mode == EUpdateMode::None) return;

	UpdateState downloading;
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (state.type != EUpdateStateType::AvailableForDownload) return;
		downloading = state;
	}
	downloading.type = EUpdateStateType::Downloading;
	downloading.reason.clear();
	Publish(std::move(downloading));
	RunDownload();
}

bool UpdateService::Impl::VerifyPayload(
	const Update& candidate, const std::vector<std::uint8_t>& bytes, std::wstring& reason)
{
	if (candidate.sizeBytes == 0 || bytes.size() != candidate.sizeBytes) {
		reason = L"The downloaded update was not the size the release said it would be, so it was discarded.";
		return false;
	}
	if (!candidate.sha256) {
		// No digest means nothing was verified — a size is not a verification, and
		// the next step after returning true is running this file as an installer
		// against the user's installation. Refusing here is what makes "verified"
		// mean one thing. `ParseAssetDigest` also lands here for a digest that is
		// present but not a well-formed `sha256:<64 hex>`, which must fail for the
		// same reason rather than degrade to the size check.
		reason = L"The release published no checksum for this update, so it was discarded.";
		return false;
	}
	const auto computed = dependencies.digest->ComputeSha256Hex(bytes);
	if (!computed) {
		reason = L"The downloaded update could not be verified, so it was discarded.";
		return false;
	}
	// `DigestsMatch`, not `==`: a digest that is not a well-formed SHA-256 value
	// must fail, and string equality would happily accept two equally malformed
	// ones.
	if (!DigestsMatch(*computed, *candidate.sha256)) {
		reason = L"The downloaded update did not match the checksum the release published, so it was discarded.";
		return false;
	}
	reason.clear();
	return true;
}

void UpdateService::Impl::RunDownload()
{
	if (ObserveCancellation()) return;

	const auto candidate = CurrentUpdate();
	if (!candidate) {
		PublishIdleAndSchedulePeriodicCheck(L"There is no update to download.");
		return;
	}
	if (candidate->sizeBytes > options.maximumInstallerBytes) {
		PublishIdleAndSchedulePeriodicCheck(L"The published update is larger than this editor will download.");
		return;
	}

	Request request;
	request.method = L"GET";
	request.url = candidate->downloadUrl;
	request.headers.push_back(HttpHeader{ L"Accept", L"application/octet-stream" });
	request.headers.push_back(HttpHeader{ L"User-Agent", options.userAgent });
	request.limits.timeout = std::chrono::minutes(10);
	request.limits.maxResponseBodyBytes = options.maximumInstallerBytes;

	const RequestResult result = dependencies.requestService->Execute(request, &cancellation);
	if (ObserveCancellation()) return;

	if (!result || !result.response) {
		PublishIdleAndSchedulePeriodicCheck(DescribeRequestFailure(result));
		return;
	}
	if (result.response->statusCode != 200) {
		PublishIdleAndSchedulePeriodicCheck(L"The update download answered with status "
			+ std::to_wstring(result.response->statusCode) + L".");
		return;
	}

	std::wstring reason;
	if (!VerifyPayload(*candidate, result.response->body, reason)) {
		PublishIdleAndSchedulePeriodicCheck(std::move(reason));
		return;
	}

	const auto installerPath = dependencies.stagingStore->StoreInstaller(
		candidate->version, candidate->assetName, result.response->body);
	if (!installerPath) {
		PublishIdleAndSchedulePeriodicCheck(L"The downloaded update could not be written to this profile.");
		return;
	}

	UpdateManifest manifest;
	manifest.version = candidate->version;
	manifest.tagName = candidate->tagName;
	manifest.installerPath = *installerPath;
	manifest.installDirectory = installTarget.installDirectory;
	manifest.sizeBytes = candidate->sizeBytes;
	manifest.sha256 = candidate->sha256.value_or(std::wstring());
	manifest.releaseUrl = candidate->releaseUrl;
	manifest.applyOnExit = false;
	if (!dependencies.stagingStore->WriteManifest(manifest)) {
		PublishIdleAndSchedulePeriodicCheck(L"The downloaded update could not be recorded in this profile.");
		return;
	}
	dependencies.stagingStore->RemoveOtherStagedBuilds(candidate->version);

	UpdateState next;
	next.type = EUpdateStateType::Downloaded;
	next.updateType = installTarget.type;
	next.update = candidate;
	Publish(std::move(next));

	if (!options.enableWindowsBackgroundUpdates) return;

	UpdateState updating;
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (state.type != EUpdateStateType::Downloaded) return;
		updating = state;
	}
	updating.type = EUpdateStateType::Updating;
	Publish(std::move(updating));
	RunApply();
}

void UpdateService::Impl::RunApply()
{
	const auto manifest = dependencies.stagingStore->ReadManifest();
	if (!manifest) {
		PublishIdleAndSchedulePeriodicCheck(L"The downloaded update is no longer available in this profile.");
		return;
	}
	if (!dependencies.stagingStore->InstallerMatches(manifest->installerPath, manifest->sizeBytes)) {
		dependencies.stagingStore->ClearManifest();
		PublishIdleAndSchedulePeriodicCheck(L"The downloaded update is no longer available in this profile.");
		return;
	}
	if (manifest->installDirectory.empty()) {
		dependencies.stagingStore->ClearManifest();
		PublishIdleAndSchedulePeriodicCheck(
			L"This installation's location could not be determined, so the update was not staged.");
		return;
	}

	UpdateState next;
	next.type = EUpdateStateType::Ready;
	next.updateType = installTarget.type;
	next.update = CurrentUpdate();
	Publish(std::move(next));
}

//! The guarded entry both the public `CheckForUpdates` and the automatic timer's
//! own tick call. `explicitRequest` is exactly upstream's distinction: a user
//! gesture always resets the cancellation token and starts fresh. The timer's
//! own call additionally has to survive finding the service already busy with
//! something else — it rearms itself for the next interval rather than
//! dropping the tick, or `update.mode` = `default` would stop polling forever
//! after the first tick that ever overlapped a user action.
void UpdateService::Impl::TriggerCheck(bool explicitRequest)
{
	if (!dependencies.IsComplete()) return;
	if (options.mode == EUpdateMode::None) return;
	if (!explicitRequest && !AllowsAutomaticCheck(options.mode)) return;

	bool busy = false;
	{
		std::lock_guard<std::mutex> guard(mutex);
		// Only a settled, uninteresting state may start a check. Re-checking while
		// an update is already downloaded would throw that download away.
		busy = state.type != EUpdateStateType::Idle && state.type != EUpdateStateType::Uninitialized;
	}
	if (busy) {
		if (!explicitRequest) ScheduleNext(/*initial=*/false);
		return;
	}
	cancellation.Reset();

	UpdateState next;
	next.type = EUpdateStateType::CheckingForUpdates;
	next.updateType = installTarget.type;
	Publish(std::move(next));

	std::weak_ptr<Impl> weak = self;
	dependencies.executor->Post([weak] {
		if (const auto impl = weak.lock()) impl->RunCheck();
	});
}

UpdateService::UpdateService(UpdateServiceDependencies dependencies, UpdateServiceOptions options)
	: m_impl(std::make_shared<Impl>(std::move(dependencies), std::move(options)))
{
	m_impl->self = m_impl;
}

UpdateService::~UpdateService()
{
	// Work already posted keeps the implementation alive through its own locked
	// reference; requesting cancellation is what makes it stop early.
	if (m_impl) m_impl->cancellation.Request();
}

void UpdateService::Initialize()
{
	if (!m_impl->dependencies.IsComplete()) {
		UpdateState next;
		next.type = EUpdateStateType::Disabled;
		next.reason = L"Updating is not available in this build.";
		m_impl->Publish(std::move(next));
		return;
	}

	m_impl->installTarget = m_impl->dependencies.installLocation->Resolve();

	if (m_impl->options.mode == EUpdateMode::None) {
		UpdateState next;
		next.type = EUpdateStateType::Disabled;
		next.updateType = m_impl->installTarget.type;
		m_impl->Publish(std::move(next));
		return;
	}

	auto manifest = m_impl->dependencies.stagingStore->ReadManifest();
	if (!manifest) {
		m_impl->PublishIdleAndScheduleInitialCheck(std::wstring());
		return;
	}

	// A recorded failure is reported once and then forgotten, so a broken
	// installer cannot make every future session open with the same complaint.
	if (!manifest->lastFailure.empty()) {
		m_impl->dependencies.stagingStore->ClearManifest();
		m_impl->PublishIdleAndScheduleInitialCheck(manifest->lastFailure);
		return;
	}

	const bool stillNewer = IsNewerBuild(manifest->version, m_impl->options.currentVersion);
	const bool stillPresent = m_impl->dependencies.stagingStore->InstallerMatches(
		manifest->installerPath, manifest->sizeBytes);
	if (!stillNewer || !stillPresent || manifest->installDirectory.empty()) {
		// Either the update already applied, or its payload is gone. Both mean the
		// manifest describes nothing, and keeping it would resurrect a dead offer.
		m_impl->dependencies.stagingStore->ClearManifest();
		m_impl->PublishIdleAndScheduleInitialCheck(std::wstring());
		return;
	}

	Update staged;
	staged.version = manifest->version;
	staged.tagName = manifest->tagName;
	staged.assetName = BuildUpdateAssetName(manifest->version, m_impl->options.architecture);
	staged.sizeBytes = manifest->sizeBytes;
	if (!manifest->sha256.empty()) staged.sha256 = manifest->sha256;
	staged.releaseUrl = manifest->releaseUrl;

	UpdateState next;
	next.type = EUpdateStateType::Ready;
	next.updateType = m_impl->installTarget.type;
	next.update = std::move(staged);
	m_impl->Publish(std::move(next));
}

UpdateState UpdateService::State() const
{
	return m_impl->Snapshot();
}

void UpdateService::CheckForUpdates(bool explicitRequest)
{
	m_impl->TriggerCheck(explicitRequest);
}

void UpdateService::DownloadUpdate()
{
	if (!m_impl->dependencies.IsComplete()) return;

	std::optional<Update> candidate;
	{
		std::lock_guard<std::mutex> guard(m_impl->mutex);
		if (m_impl->state.type != EUpdateStateType::AvailableForDownload) return;
		candidate = m_impl->state.update;
	}
	if (!candidate) return;

	// With nowhere to install, "download" is the release page, exactly as upstream.
	// It deliberately leaves the state alone: nothing was downloaded.
	if (m_impl->installTarget.type == EUpdateType::Archive) {
		(void)m_impl->dependencies.launcher->OpenReleasePage(candidate->releaseUrl);
		return;
	}

	m_impl->cancellation.Reset();

	UpdateState next;
	next.type = EUpdateStateType::Downloading;
	next.updateType = m_impl->installTarget.type;
	next.update = candidate;
	m_impl->Publish(std::move(next));

	std::weak_ptr<Impl> weak = m_impl;
	m_impl->dependencies.executor->Post([weak] {
		if (const auto impl = weak.lock()) impl->RunDownload();
	});
}

void UpdateService::ApplyUpdate()
{
	if (!m_impl->dependencies.IsComplete()) return;

	UpdateState updating;
	{
		std::lock_guard<std::mutex> guard(m_impl->mutex);
		if (m_impl->state.type != EUpdateStateType::Downloaded) return;
		updating = m_impl->state;
	}
	updating.type = EUpdateStateType::Updating;
	m_impl->Publish(std::move(updating));

	std::weak_ptr<Impl> weak = m_impl;
	m_impl->dependencies.executor->Post([weak] {
		if (const auto impl = weak.lock()) impl->RunApply();
	});
}

void UpdateService::QuitAndInstall()
{
	if (!m_impl->dependencies.IsComplete()) return;

	{
		std::lock_guard<std::mutex> guard(m_impl->mutex);
		if (m_impl->state.type != EUpdateStateType::Ready) return;
	}

	auto manifest = m_impl->dependencies.stagingStore->ReadManifest();
	if (!manifest) {
		m_impl->PublishIdleAndSchedulePeriodicCheck(L"The downloaded update is no longer available in this profile.");
		return;
	}
	manifest->applyOnExit = true;
	if (!m_impl->dependencies.stagingStore->WriteManifest(*manifest)) {
		m_impl->PublishIdleAndSchedulePeriodicCheck(L"The update could not be armed for the next restart.");
		return;
	}

	UpdateState next = m_impl->Snapshot();
	next.type = EUpdateStateType::Restarting;
	m_impl->Publish(std::move(next));
}

void UpdateService::AbortQuitAndInstall()
{
	if (!m_impl->dependencies.IsComplete()) return;

	{
		std::lock_guard<std::mutex> guard(m_impl->mutex);
		if (m_impl->state.type != EUpdateStateType::Restarting) return;
	}

	// The user cancelled the quit, so the update must not run on the next exit —
	// which may be an ordinary exit minutes from now.
	auto manifest = m_impl->dependencies.stagingStore->ReadManifest();
	if (manifest && manifest->applyOnExit) {
		manifest->applyOnExit = false;
		(void)m_impl->dependencies.stagingStore->WriteManifest(*manifest);
	}

	UpdateState next = m_impl->Snapshot();
	next.type = EUpdateStateType::Ready;
	m_impl->Publish(std::move(next));
}

void UpdateService::CancelUpdate()
{
	{
		std::lock_guard<std::mutex> guard(m_impl->mutex);
		if (m_impl->state.type != EUpdateStateType::CheckingForUpdates
			&& m_impl->state.type != EUpdateStateType::Downloading) {
			return;
		}
	}
	m_impl->cancellation.Request();

	UpdateState next = m_impl->Snapshot();
	next.type = EUpdateStateType::Cancelling;
	m_impl->Publish(std::move(next));
}

std::optional<UpdateServiceSubscriptionId> UpdateService::Subscribe(UpdateServiceListener listener)
{
	if (!listener) return std::nullopt;

	std::lock_guard<std::mutex> guard(m_impl->mutex);
	if (m_impl->subscriptions.size() >= m_impl->options.maximumSubscriptions) return std::nullopt;
	const UpdateServiceSubscriptionId id = m_impl->nextSubscriptionId++;
	m_impl->subscriptions.emplace_back(id, std::move(listener));
	return id;
}

void UpdateService::Unsubscribe(UpdateServiceSubscriptionId subscriptionId) noexcept
{
	std::lock_guard<std::mutex> guard(m_impl->mutex);
	for (auto it = m_impl->subscriptions.begin(); it != m_impl->subscriptions.end(); ++it) {
		if (it->first == subscriptionId) {
			m_impl->subscriptions.erase(it);
			return;
		}
	}
}

EPendingUpdateOutcome RunPendingUpdate(IUpdateStagingStore& stagingStore, IUpdateLauncher& launcher)
{
	auto manifest = stagingStore.ReadManifest();
	if (!manifest || !manifest->applyOnExit) return EPendingUpdateOutcome::NotArmed;

	if (!stagingStore.InstallerMatches(manifest->installerPath, manifest->sizeBytes)) {
		stagingStore.ClearManifest();
		return EPendingUpdateOutcome::InvalidManifest;
	}

	InstallerInvocation invocation;
	invocation.installerPath = manifest->installerPath;
	invocation.installDirectory = manifest->installDirectory;
	invocation.logPath = stagingStore.InstallLogPath(manifest->version);
	invocation.relaunchAfterInstall = true;

	if (!BuildInstallerArguments(invocation)) {
		stagingStore.ClearManifest();
		return EPendingUpdateOutcome::InvalidManifest;
	}

	if (!launcher.LaunchInstaller(invocation)) {
		// Disarm and record why, so the next session explains itself instead of
		// trying the same failing launch on every exit.
		manifest->applyOnExit = false;
		manifest->lastFailure = L"The update installer could not be started, so this editor was not updated.";
		(void)stagingStore.WriteManifest(*manifest);
		return EPendingUpdateOutcome::LaunchFailed;
	}

	// The manifest stays on disk with `applyOnExit` set: the installer replaces
	// this installation and the next session's `Initialize` sees a version that is
	// no longer newer, which is what clears it.
	return EPendingUpdateOutcome::Launched;
}

} // namespace update
