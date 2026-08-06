/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateComposition.h"

#include "config/CConfigurationProxyService.h"
#include "config/IConfigurationService.h"
#include "platform/profiles/UserDataProfileIdentity.h"
#include "update/UpdateAutoCheckTimer.h"
#include "update/UpdateDigest.h"
#include "update/UpdateExecutor.h"
#include "update/UpdateInstallLocation.h"
#include "update/UpdateStagingStore.h"
#include "update/Win32UpdateLauncher.h"
#include <sakura/request/RequestService.h>
#include <sakura/request/win32/WinHttpRequestRuntime.h>
#include <sakura/request/win32/WinHttpSystemProxyResolver.h>

#include "version.h"

#include <chrono>
#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace update {
namespace {

//! GitHub answers the feed in well under a second and the installer download is
//! bounded by `maximumInstallerBytes`, so the redirect/retry budget here only has
//! to cover the release-asset hop to `objects.githubusercontent.com`.
constexpr std::size_t kMaximumRedirects = 5;
constexpr std::size_t kMaximumRetries = 2;

//! The asset suffix this build would accept. An architecture with no published
//! installer simply matches no asset and reports "no update", which is the
//! intended fail-closed behavior rather than an error to report separately.
#if defined(_M_ARM64)
constexpr std::wstring_view kBuildArchitecture = L"arm64";
#elif defined(_M_X64) || defined(__x86_64__)
constexpr std::wstring_view kBuildArchitecture = L"x64";
#else
constexpr std::wstring_view kBuildArchitecture = L"x86";
#endif

const std::vector<std::string> kUpdateConfigurationKeys{
	"update.mode",
	"update.enableWindowsBackgroundUpdates",
	"update.titleBar",
};

//! The update path never authenticates. GitHub's public release feed and its
//! asset redirects are anonymous, and a credential prompt raised from a
//! background poll would be indistinguishable from a phishing dialog.
class NoCredentialService final : public platform::request::ICredentialService {
public:
	std::optional<platform::request::RequestCredential> GetCredential(
		const platform::request::CredentialRequest&,
		std::optional<std::chrono::steady_clock::time_point>,
		const platform::request::IRequestCancellation*
	) override
	{
		return std::nullopt;
	}
};

UpdateCompositionResult Failure(EUpdateCompositionOutcome outcome, const char* diagnostic)
{
	return { outcome, nullptr, diagnostic };
}

} // namespace

struct UpdateComposition::Impl final {
	// Reverse destruction preserves every dependency while its consumer is torn
	// down: service → autoCheckTimer → executor → adapters → request service →
	// WinHTTP boundaries. `Shutdown()` still stops the timer and the executor
	// explicitly, in that order, before any of this runs — see `Shutdown()` for
	// why the order between those two matters even though reverse destruction
	// alone would already get it right.
	platform::request::win32::WinHttpRequestTransport transport;
	platform::request::win32::WinHttpSystemProxyResolver systemProxyResolver;
	config::CConfigurationProxyService proxyService;
	NoCredentialService credentialService;
	platform::request::win32::Win32RequestClock clock;
	platform::request::win32::Win32RequestScheduler scheduler;
	platform::request::win32::ThreadSafeRetryJitterSource jitterSource;
	platform::request::RequestService requestService;
	UpdateStagingStore stagingStore;
	UpdateInstallLocation installLocation;
	Win32UpdateLauncher launcher;
	UpdateDigest digest;
	UpdateWorkerExecutor executor;
	//! Named `autoCheckTimer`, not `scheduler`, only to avoid colliding with the
	//! unrelated `Win32RequestScheduler scheduler` member above (HTTP retry
	//! backoff). It is still bound to `UpdateServiceDependencies::scheduler`
	//! below.
	UpdateAutoCheckTimer autoCheckTimer;
	UpdateConfigurationSnapshot configuration;
	UpdateService service;

	Impl(
		config::ConfigurationNetworkPolicySnapshot networkPolicy,
		UpdateConfigurationSnapshot configurationSnapshot,
		std::filesystem::path stagingRoot,
		std::filesystem::path executablePath,
		UpdateServiceOptions options)
		: proxyService(networkPolicy, systemProxyResolver)
		, requestService(
			transport,
			proxyService,
			credentialService,
			clock,
			scheduler,
			jitterSource,
			nullptr,
			{ kMaximumRedirects, kMaximumRetries, std::chrono::milliseconds(250), std::chrono::seconds(30) })
		, stagingStore(std::move(stagingRoot))
		, installLocation(std::move(executablePath))
		, configuration(configurationSnapshot)
		, service(
			UpdateServiceDependencies{
				.requestService = &requestService,
				.stagingStore = &stagingStore,
				.installLocation = &installLocation,
				.launcher = &launcher,
				.digest = &digest,
				.executor = &executor,
				.scheduler = &autoCheckTimer,
			},
			std::move(options))
	{
	}
};

UpdateComposition::UpdateComposition(
	config::ConfigurationNetworkPolicySnapshot networkPolicy,
	UpdateConfigurationSnapshot configuration,
	std::filesystem::path stagingRoot,
	std::filesystem::path executablePath,
	UpdateServiceOptions options)
	: m_impl(std::make_unique<Impl>(
		std::move(networkPolicy),
		configuration,
		std::move(stagingRoot),
		std::move(executablePath),
		std::move(options)))
{
	m_impl->service.Initialize();
}

UpdateComposition::~UpdateComposition()
{
	Shutdown();
}

IUpdateService& UpdateComposition::Service() noexcept
{
	return m_impl->service;
}

const UpdateConfigurationSnapshot& UpdateComposition::Configuration() const noexcept
{
	return m_impl->configuration;
}

IUpdateLauncher& UpdateComposition::Launcher() noexcept
{
	return m_impl->launcher;
}

void UpdateComposition::Shutdown() noexcept
{
	if (!m_impl) return;
	// Stop the timer before the worker: a tick that is already running
	// `TriggerCheck` posts to the executor, and stopping the executor first
	// would just have that post silently dropped instead of never being made.
	m_impl->autoCheckTimer.Stop();
	m_impl->executor.Stop();
}

UpdateVersion CurrentBuildVersion() noexcept
{
	return UpdateVersion{
		static_cast<std::uint32_t>(VER_A),
		static_cast<std::uint32_t>(VER_B),
		static_cast<std::uint32_t>(VER_C),
		static_cast<std::uint32_t>(VER_D),
	};
}

std::wstring CurrentBuildRemoteUrl()
{
#ifdef GIT_REMOTE_ORIGIN_URL
	const std::string_view utf8(GIT_REMOTE_ORIGIN_URL);
	// The origin URL is generated ASCII, so a byte-wise widen is exact here and
	// avoids pulling a code-page conversion into a startup path.
	std::wstring wide;
	wide.reserve(utf8.size());
	for (const char character : utf8) {
		if (static_cast<unsigned char>(character) > 0x7Fu) return {};
		wide.push_back(static_cast<wchar_t>(character));
	}
	return wide;
#else
	return {};
#endif
}

std::optional<UpdateConfigurationSnapshot> ReadUpdateConfiguration(
	config::IConfigurationService& configurationService,
	const std::wstring& userDataProfileId)
{
	const config::ConfigurationTarget target{ userDataProfileId, std::nullopt, std::nullopt, std::nullopt };
	const auto read = configurationService.ReadSnapshot(kUpdateConfigurationKeys, target);
	if (read.outcome != config::EConfigurationOutcome::Applied || !read.snapshot
		|| read.snapshot->values.size() != kUpdateConfigurationKeys.size()) {
		return std::nullopt;
	}

	const auto& values = read.snapshot->values;
	const auto* const mode = std::get_if<std::wstring>(&values[0].Value());
	const auto* const backgroundUpdates = std::get_if<bool>(&values[1].Value());
	const auto* const titleBar = std::get_if<bool>(&values[2].Value());
	if (mode == nullptr || backgroundUpdates == nullptr || titleBar == nullptr) return std::nullopt;

	// `update.mode` is a string enum, and its descriptor already refuses a value
	// outside the four upstream ids. An unparseable value here would therefore be
	// an internal inconsistency, so it fails the whole read rather than silently
	// becoming `default` and checking for updates the user asked not to have.
	std::string modeId;
	modeId.reserve(mode->size());
	for (const wchar_t character : *mode) {
		if (character > 0x7F) return std::nullopt;
		modeId.push_back(static_cast<char>(character));
	}
	const auto parsedMode = ParseUpdateModeId(modeId);
	if (!parsedMode) return std::nullopt;

	return UpdateConfigurationSnapshot{ *parsedMode, *backgroundUpdates, *titleBar };
}

UpdateCompositionResult CreateUpdateComposition(
	config::IConfigurationService& configurationService,
	std::wstring userDataProfileId) noexcept
{
	// The selected user-data profile, never the control authority; its identity
	// space is opaque rather than canonical-hex. See `config/CLAUDE.md`.
	if (!platform::profiles::IsOpaqueUserDataProfileId(userDataProfileId)) {
		return Failure(EUpdateCompositionOutcome::InvalidProfileId,
			"updates require a valid user-data profile identity");
	}

	try {
		const auto configuration = ReadUpdateConfiguration(configurationService, userDataProfileId);
		if (!configuration) {
			return Failure(EUpdateCompositionOutcome::ConfigurationInvalid,
				"update configuration could not be read as one coherent snapshot");
		}

		config::CConfigurationNetworkPolicy networkPolicy(configurationService, std::move(userDataProfileId));
		const auto policy = networkPolicy.Snapshot();
		if (policy.outcome == config::EConfigurationNetworkPolicyOutcome::Unsupported) {
			return Failure(EUpdateCompositionOutcome::ConfigurationUnavailable,
				"update network configuration is unavailable");
		}
		if (!policy.snapshot.has_value() || policy.outcome != config::EConfigurationNetworkPolicyOutcome::Ready) {
			return Failure(EUpdateCompositionOutcome::ConfigurationInvalid,
				"update network configuration is invalid");
		}
		// The production WinHTTP transport provides no TLS-validation escape
		// hatch, so this compatibility setting must terminate here rather than
		// silently downloading an installer over an unvalidated connection.
		if (!policy.snapshot->proxyStrictSSL) {
			return Failure(EUpdateCompositionOutcome::ConfigurationInvalid,
				"update network configuration is unsupported");
		}

		auto stagingRoot = UpdateStagingStore::DefaultRoot();
		if (stagingRoot.empty()) {
			return Failure(EUpdateCompositionOutcome::StagingUnavailable,
				"updates have nowhere to stage a download on this machine");
		}

		UpdateServiceOptions options;
		options.currentVersion = CurrentBuildVersion();
		options.remoteUrl = CurrentBuildRemoteUrl();
		options.architecture = std::wstring(kBuildArchitecture);
		options.mode = configuration->mode;
		options.enableWindowsBackgroundUpdates = configuration->enableWindowsBackgroundUpdates;

		UpdateCompositionResult result;
		result.outcome = EUpdateCompositionOutcome::Ready;
		result.composition = std::make_unique<UpdateComposition>(
			std::move(*policy.snapshot),
			*configuration,
			std::move(stagingRoot),
			UpdateInstallLocation::CurrentExecutablePath(),
			std::move(options));
		result.diagnostic = "the update service is ready";
		return result;
	}
	catch (...) {
		return Failure(EUpdateCompositionOutcome::InternalFailure, "update service initialization failed");
	}
}

} // namespace update
