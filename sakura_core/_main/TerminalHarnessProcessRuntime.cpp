/*! @file @brief Editor-process owner for the integrated terminal control plane. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "_main/TerminalHarnessProcessRuntime.h"

#include <sakura/harnessbridge/HarnessBridgeAuthenticatedSession.h>
#include <sakura/harnessbridge/HarnessBridgeCapability.h>
#include <sakura/harnessbridge/HarnessBridgeEnvironment.h>
#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>
#include <sakura/harnessbridge/HarnessBridgeSecurity.h>
#include <sakura/harnessbridge/HarnessBridgeServiceHost.h>

#include "terminal/DefaultTerminalLaunchProfileService.h"
#include "terminal/tmux/TmuxRuntimeAdapter.h"
#include "terminal/tmux/TmuxWaitChannelService.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <span>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace {

using namespace platform::harnessbridge;

bool FillRandom(std::span<std::uint8_t> value) noexcept
{
	return !value.empty() && BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, value.data(),
		static_cast<ULONG>(value.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
		&& std::any_of(value.begin(), value.end(), [](const auto byte) { return byte != 0; });
}

std::optional<std::uint64_t> RandomNonzeroU64() noexcept
{
	std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
	if (!FillRandom(bytes)) return std::nullopt;
	std::uint64_t value{};
	for (std::size_t index = 0; index < bytes.size(); ++index) {
		value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
	}
	return value == 0 ? std::nullopt : std::optional<std::uint64_t>(value);
}

std::optional<std::uint64_t> QueryProcessCreationTime(const std::uint32_t pid) noexcept
{
	if (pid == 0) return std::nullopt;
	const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process) return std::nullopt;
	FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
	const bool succeeded = ::GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) != FALSE;
	::CloseHandle(process);
	if (!succeeded) return std::nullopt;
	ULARGE_INTEGER value{};
	value.LowPart = creation.dwLowDateTime;
	value.HighPart = creation.dwHighDateTime;
	return value.QuadPart == 0 ? std::nullopt : std::optional<std::uint64_t>(value.QuadPart);
}

std::optional<std::wstring> Utf8ToWide(const std::string_view value)
{
	if (value.empty() || value.size() > 127) return std::nullopt;
	const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (count <= 0) return std::nullopt;
	std::wstring result(static_cast<std::size_t>(count), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), count) != count) return std::nullopt;
	return result;
}

bool HasTrustedTools(const std::filesystem::path& directory) noexcept
{
	try {
		if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) return false;
		for (const auto* name : { L"sakura-tmux.exe", L"tmux.exe", L"sakura-harness.exe" }) {
			if (!std::filesystem::is_regular_file(directory / name)) return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

HarnessBridgeTargetDescriptor ToBridgeTarget(const terminal::TerminalTargetCoordinate& source)
{
	HarnessBridgeTargetDescriptor target;
	target.profileId = source.profileId;
	target.profileGeneration = source.profileGeneration.value;
	target.editorId = source.editorId.value;
	target.bridgeEpoch = source.bridgeEpoch.value;
	target.runtimeGeneration = source.runtimeGeneration.value;
	target.instanceGeneration = source.instanceGeneration;
	target.sessionId = source.sessionId.value;
	target.windowId = source.windowId.value;
	target.paneId = source.paneId.value;
	target.instanceId = source.instanceId.value;
	return target;
}

terminal::TerminalTargetCoordinate ToTerminalTarget(const HarnessBridgeTargetDescriptor& source)
{
	terminal::TerminalTargetCoordinate target;
	target.profileId = source.profileId;
	target.profileGeneration = { source.profileGeneration };
	target.editorId.value = source.editorId;
	target.bridgeEpoch = { source.bridgeEpoch };
	target.runtimeGeneration = { source.runtimeGeneration };
	target.instanceGeneration = source.instanceGeneration;
	target.sessionId = { source.sessionId };
	target.windowId = { source.windowId };
	target.paneId = { source.paneId };
	target.instanceId = { source.instanceId };
	return target;
}

HarnessCapabilityContext ToCapabilityContext(const HarnessBridgeTargetDescriptor& target)
{
	HarnessCapabilityContext context;
	context.bridgeEpoch = target.bridgeEpoch;
	context.runtimeGeneration = target.runtimeGeneration;
	context.sessionId = target.sessionId;
	context.paneId = target.paneId;
	context.profileId = target.profileId;
	context.profileGeneration = target.profileGeneration;
	context.editorId = target.editorId;
	context.instanceGeneration = target.instanceGeneration;
	context.windowId = target.windowId;
	context.instanceId = target.instanceId;
	return context;
}

} // namespace

struct CTerminalHarnessProcessRuntime::Impl final {
	explicit Impl(TerminalHarnessProcessRuntimeOptions value) : options(std::move(value)) {}

	bool InitializeIdentity(std::wstring& diagnostic) noexcept
	{
		if (options.profileId.empty() || options.profileGeneration == 0) {
			diagnostic = L"Terminal Harness identity is incomplete.";
			return false;
		}
		const auto profileWide = Utf8ToWide(options.profileId);
		const auto epoch = RandomNonzeroU64();
		const auto generation = RandomNonzeroU64();
		if (!profileWide || !epoch || !generation || !FillRandom(editorId) || !FillRandom(bridgeId)) {
			diagnostic = L"Terminal Harness identity generation failed.";
			return false;
		}
		profileIdWide = *profileWide;
		bridgeEpoch = *epoch;
		runtimeGeneration = *generation;
		endpointHash = ComputeHarnessEndpointHash(profileIdWide, editorId, bridgeEpoch);
		if (endpointHash.empty()) {
			diagnostic = L"Terminal Harness endpoint identity generation failed.";
			return false;
		}
		return true;
	}

	void DecorateLaunch(const terminal::TerminalCreateRequest& request,
		terminal::TerminalLaunchOptions& launch)
	{
		const auto eraseHarness = [&launch] {
			launch.environmentOverrides.push_back({ std::wstring(kHarnessEndpointEnvironmentName), std::nullopt });
			launch.environmentOverrides.push_back({ std::wstring(kHarnessTargetEnvironmentName), std::nullopt });
			launch.environmentOverrides.push_back({ std::wstring(kHarnessCapabilityEnvironmentName), std::nullopt });
		};
		if (request.origin != terminal::TerminalInstanceOrigin::Interactive
			|| request.environmentPolicy != terminal::TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim) {
			eraseHarness();
			return;
		}
		if (!ready || !trustedTools || !runtime || !request.instanceId.IsValid()
			|| request.instanceGeneration == 0 || !request.sessionId.IsValid()
			|| !request.paneId.IsValid() || !request.windowId || !request.windowId->IsValid()) {
			launch.executablePath.clear();
			return;
		}

		auto coordinate = coordinateBase;
		coordinate.instanceGeneration = request.instanceGeneration;
		coordinate.sessionId = request.sessionId;
		coordinate.windowId = *request.windowId;
		coordinate.paneId = request.paneId;
		coordinate.instanceId = request.instanceId;
		const auto target = ToBridgeTarget(coordinate);
		const auto credential = capabilities.Issue(ToCapabilityContext(target),
			EHarnessGrant::Message | EHarnessGrant::ConsoleRead | EHarnessGrant::SendInput
				| EHarnessGrant::ManageTerminal,
			std::chrono::steady_clock::time_point::max());
		const auto endpointValue = EncodeHarnessEndpointEnvironment(endpointHash);
		const auto targetValue = EncodeHarnessTargetEnvironment(target);
		const auto credentialValue = credential ? EncodeHarnessCapabilityEnvironment(*credential) : std::nullopt;
		if (!credential || !endpointValue || !targetValue || !credentialValue) {
			if (credential) capabilities.Revoke(credential->id);
			launch.executablePath.clear();
			return;
		}
		{
			const std::lock_guard lock(capabilityMutex);
			instanceCapabilities[request.instanceId] = credential->id;
		}
		launch.environmentOverrides.push_back({ std::wstring(kHarnessEndpointEnvironmentName), *endpointValue });
		launch.environmentOverrides.push_back({ std::wstring(kHarnessTargetEnvironmentName), *targetValue });
		launch.environmentOverrides.push_back({ std::wstring(kHarnessCapabilityEnvironmentName), *credentialValue });
		launch.environmentOverrides.push_back({ L"NoDefaultCurrentDirectoryInExePath", std::wstring(L"1") });
		launch.prependPathDirectories.push_back(options.terminalToolsDirectory.native());
	}

	void RevokeInstance(const terminal::TerminalInstanceId instanceId) noexcept
	{
		HarnessOpaqueId credential;
		bool found = false;
		{
			const std::lock_guard lock(capabilityMutex);
			const auto current = instanceCapabilities.find(instanceId);
			if (current != instanceCapabilities.end()) {
				credential = current->second;
				instanceCapabilities.erase(current);
				found = true;
			}
		}
		if (found) capabilities.Revoke(credential);
	}

	TerminalHarnessProcessRuntimeOptions options;
	std::wstring profileIdWide;
	std::array<std::uint8_t, 16> editorId{};
	std::array<std::uint8_t, 16> bridgeId{};
	std::uint64_t bridgeEpoch{};
	std::uint64_t runtimeGeneration{};
	std::wstring endpointHash;
	terminal::TerminalTargetCoordinate coordinateBase;
	std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles;
	std::shared_ptr<terminal::CTerminalRuntimeService> runtime;
	terminal::TerminalSubscription runtimeSubscription;
	terminal::tmux::TmuxWaitChannelService waitChannels;
	std::unique_ptr<terminal::tmux::TmuxRuntimeAdapter> tmux;
	CHarnessBridgeCapabilityStore capabilities;
	std::shared_ptr<CHarnessBridgeAuthenticatedSessionFactory> sessionFactory;
	std::unique_ptr<CHarnessBridgeServiceHost> host;
	std::mutex capabilityMutex;
	std::map<terminal::TerminalInstanceId, HarnessOpaqueId> instanceCapabilities;
	bool trustedTools{};
	bool ready{};
};

CTerminalHarnessProcessRuntime::CTerminalHarnessProcessRuntime(TerminalHarnessProcessRuntimeOptions options)
	: m_impl(std::make_unique<Impl>(std::move(options)))
{
}

CTerminalHarnessProcessRuntime::~CTerminalHarnessProcessRuntime()
{
	Stop();
}

bool CTerminalHarnessProcessRuntime::Start(std::wstring& diagnostic) noexcept
{
	if (!m_impl) {
		diagnostic = L"Terminal Harness composition is unavailable.";
		return false;
	}
	if (m_impl->ready) {
		diagnostic.clear();
		return true;
	}
	try {
		if (!m_impl->InitializeIdentity(diagnostic)) return false;
		m_impl->trustedTools = HasTrustedTools(m_impl->options.terminalToolsDirectory);
		m_impl->profiles = std::make_shared<terminal::CDefaultTerminalLaunchProfileService>();
		m_impl->coordinateBase.profileId = m_impl->options.profileId;
		m_impl->coordinateBase.profileGeneration = { m_impl->options.profileGeneration };
		m_impl->coordinateBase.editorId.value = m_impl->editorId;
		m_impl->coordinateBase.bridgeEpoch = { m_impl->bridgeEpoch };
		m_impl->coordinateBase.runtimeGeneration = { m_impl->runtimeGeneration };

		terminal::TerminalRuntimeServiceDependencies dependencies;
		dependencies.createSession = [](terminal::TerminalSessionCallbacks callbacks) {
			return std::make_unique<terminal::CTerminalSession>(
				terminal::CreateConPtyTerminalBackend(), std::move(callbacks));
		};
		const auto profiles = m_impl->profiles;
		dependencies.resolveLaunch = [profiles](const terminal::TerminalSize size,
			const std::wstring_view workingDirectory) {
			return profiles->Resolve(size, workingDirectory);
		};
		dependencies.decorateLaunch = [impl = m_impl.get()](const terminal::TerminalCreateRequest& request,
			terminal::TerminalLaunchOptions& launch) { impl->DecorateLaunch(request, launch); };
		dependencies.defaultWorkingDirectory = m_impl->options.defaultWorkingDirectory.native();
		dependencies.coordinateBase = m_impl->coordinateBase;
		m_impl->runtime = std::make_shared<terminal::CTerminalRuntimeService>(
			std::move(dependencies), terminal::TerminalRuntimeGeneration{ m_impl->runtimeGeneration });
		m_impl->runtimeSubscription = m_impl->runtime->Subscribe([impl = m_impl.get()](
			const terminal::TerminalInstanceEvent& event) {
			if (event.kind == terminal::TerminalInstanceEventKind::Completed) {
				impl->RevokeInstance(event.coordinate.instanceId);
			}
		});
		const auto runtime = m_impl->runtime;
		m_impl->tmux = std::make_unique<terminal::tmux::TmuxRuntimeAdapter>(
			*runtime,
			[runtime] { return runtime->CollectionSnapshot(); },
			[runtime](const terminal::TerminalInstanceId id)
				-> std::optional<terminal::TerminalInstanceSnapshot> {
				const auto* instance = runtime->Instance(id);
				return instance ? std::optional<terminal::TerminalInstanceSnapshot>(instance->Snapshot())
					: std::nullopt;
			},
			m_impl->waitChannels);

		HarnessBridgeSessionFences fences;
		fences.target = ToBridgeTarget(m_impl->coordinateBase);
		fences.bridgeId = m_impl->bridgeId;
		HarnessBridgePeerProcessCallbacks processCallbacks;
		processCallbacks.queryCreationTime = QueryProcessCreationTime;
		processCallbacks.isJobMember = [impl = m_impl.get()](const HarnessBridgeTargetDescriptor& target,
			const std::uint32_t pid, const std::uint64_t creationTime) {
			return impl->runtime && impl->runtime->OwnsProcess(ToTerminalTarget(target), pid, creationTime);
		};
		m_impl->sessionFactory = std::make_shared<CHarnessBridgeAuthenticatedSessionFactory>(
			std::move(fences), m_impl->capabilities, std::move(processCallbacks),
			[impl = m_impl.get()](const HarnessBridgeSessionContext&, const HarnessBridgeFrame&) {
				if (!impl->host || !impl->tmux) return std::shared_ptr<IHarnessBridgeOperationDispatcher>{};
				return std::static_pointer_cast<IHarnessBridgeOperationDispatcher>(
					std::make_shared<CHarnessBridgeOperationDispatcher>(
						&impl->host->Broker(), impl->tmux.get()));
			});
		m_impl->host = std::make_unique<CHarnessBridgeServiceHost>(m_impl->sessionFactory);

		HarnessEditorEndpointDescriptor descriptor;
		descriptor.endpointHash = m_impl->endpointHash;
		descriptor.profileId = m_impl->profileIdWide;
		descriptor.editorId = m_impl->editorId;
		descriptor.bridgeId = m_impl->bridgeId;
		descriptor.profileGeneration = m_impl->options.profileGeneration;
		descriptor.bridgeEpoch = m_impl->bridgeEpoch;
		descriptor.runtimeGeneration = m_impl->runtimeGeneration;
		descriptor.lifecycle = EHarnessBridgeLifecycle::Starting;
		descriptor.serverPid = ::GetCurrentProcessId();
		descriptor.serverProcessCreationTime = QueryProcessCreationTime(descriptor.serverPid).value_or(0);
		descriptor.pipeName = BuildHarnessPipeName(m_impl->endpointHash);
		if (descriptor.serverProcessCreationTime == 0
			|| m_impl->host->Start(std::move(descriptor), diagnostic) != EHarnessBridgeHostStartResult::Started) {
			if (diagnostic.empty()) diagnostic = L"Terminal Harness Bridge failed to start.";
			return false;
		}
		if (!m_impl->trustedTools) {
			diagnostic = L"Terminal Harness tools are missing from the trusted Sakura terminal-tools directory.";
			m_impl->host->Stop();
			return false;
		}
		m_impl->ready = true;
		diagnostic.clear();
		return true;
	} catch (...) {
		diagnostic = L"Terminal Harness composition raised an unexpected error.";
		Stop();
		return false;
	}
}

void CTerminalHarnessProcessRuntime::Stop() noexcept
{
	if (!m_impl) return;
	m_impl->ready = false;
	if (m_impl->host) m_impl->host->Stop();
	if (m_impl->tmux) m_impl->tmux->BeginShutdown();
	m_impl->runtimeSubscription.Reset();
	m_impl->capabilities.RevokeAll();
	if (m_impl->runtime) {
		m_impl->runtime->BeginClose();
		static_cast<void>(m_impl->runtime->WaitForClose(std::chrono::steady_clock::now()
			+ terminal::CTerminalSession::kGracefulCloseTimeout
			+ terminal::CTerminalSession::kForcedCloseTimeout));
	}
	m_impl->host.reset();
	m_impl->sessionFactory.reset();
	m_impl->tmux.reset();
	m_impl->runtime.reset();
	m_impl->profiles.reset();
}

std::shared_ptr<terminal::CTerminalRuntimeService>
CTerminalHarnessProcessRuntime::RuntimeService() const noexcept
{
	return m_impl ? m_impl->runtime : nullptr;
}

std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService>
CTerminalHarnessProcessRuntime::LaunchProfiles() const noexcept
{
	return m_impl ? m_impl->profiles : nullptr;
}

bool CTerminalHarnessProcessRuntime::IsReady() const noexcept
{
	return m_impl && m_impl->ready;
}
