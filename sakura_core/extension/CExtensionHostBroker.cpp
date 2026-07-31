/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostBroker.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <utility>

#include <bcrypt.h>
#include <windows.h>

namespace {

constexpr std::size_t IdentityBytes = 16;

bool HasNativeExtensionHostSession(EExtensionHostState state) noexcept
{
	return state == EExtensionHostState::Starting || state == EExtensionHostState::Ready ||
		state == EExtensionHostState::KeepAlive || state == EExtensionHostState::Quiescing;
}

bool NtSuccess(NTSTATUS status) noexcept
{
	return status >= 0;
}

std::wstring BytesToHex(const std::uint8_t* bytes, std::size_t count)
{
	static constexpr wchar_t digits[] = L"0123456789abcdef";
	std::wstring result;
	result.reserve(count * 2);
	for (std::size_t i = 0; i < count; ++i) {
		result.push_back(digits[bytes[i] >> 4]);
		result.push_back(digits[bytes[i] & 0x0f]);
	}
	return result;
}

std::wstring NormalizeProfileIdentity(const std::filesystem::path& profileDirectory)
{
	std::error_code error;
	auto identity = std::filesystem::absolute(profileDirectory, error);
	if (error) {
		identity = profileDirectory;
		error.clear();
	}
	const auto canonical = std::filesystem::weakly_canonical(identity, error);
	if (!error) {
		identity = canonical;
	}
	std::wstring normalized = identity.lexically_normal().wstring();
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towlower(character));
	});
	return normalized;
}

class AlgorithmHandle final {
public:
	~AlgorithmHandle()
	{
		if (m_value) {
			::BCryptCloseAlgorithmProvider(m_value, 0);
		}
	}
	BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }
private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

} // namespace

CExtensionHostBroker::CExtensionHostBroker(
	SExtensionHostBrokerConfig config,
	std::unique_ptr<IExtensionHostProcess> process,
	IExtensionHostBrokerObserver* observer)
	: m_config(std::move(config))
	, m_stateMachine(m_config.lifecycle)
	, m_process(std::move(process))
	, m_observer(observer)
	, m_profileHash(ComputeProfileHash(m_config.profileDirectory))
	, m_bootId(m_config.bootIdOverride.empty() ? GenerateBootId() : m_config.bootIdOverride)
{
	if (!m_process) {
		throw std::invalid_argument("extension host process must not be null");
	}
	if (m_config.profileDirectory.empty() || m_profileHash.empty() || m_bootId.empty()) {
		throw std::invalid_argument("extension host profile identity must not be empty");
	}
	if (m_config.brokerProcessId == 0) {
		m_config.brokerProcessId = ::GetCurrentProcessId();
	}
	m_pipeName = L"\\\\.\\pipe\\sakura-exthost-" + m_profileHash + L"-" + m_bootId;
}

CExtensionHostBroker::~CExtensionHostBroker()
{
	m_observer = nullptr;
	if (m_process) {
		m_process->Terminate(ERROR_PROCESS_ABORTED);
	}
}

std::wstring CExtensionHostBroker::ComputeProfileHash(const std::filesystem::path& profileDirectory)
{
	if (profileDirectory.empty()) {
		return {};
	}
	const auto identity = NormalizeProfileIdentity(profileDirectory);
	if (identity.size() > (std::numeric_limits<ULONG>::max)() / sizeof(wchar_t)) {
		return {};
	}

	AlgorithmHandle algorithm;
	if (!NtSuccess(::BCryptOpenAlgorithmProvider(
		algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return {};
	}
	std::array<std::uint8_t, 32> digest{};
	if (!NtSuccess(::BCryptHash(
		algorithm.Get(),
		nullptr,
		0,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(identity.data())),
		static_cast<ULONG>(identity.size() * sizeof(wchar_t)),
		digest.data(),
		static_cast<ULONG>(digest.size())))) {
		return {};
	}
	return BytesToHex(digest.data(), IdentityBytes);
}

std::wstring CExtensionHostBroker::GenerateBootId()
{
	std::array<std::uint8_t, IdentityBytes> random{};
	if (!NtSuccess(::BCryptGenRandom(
		nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		return {};
	}
	return BytesToHex(random.data(), random.size());
}

std::wstring CExtensionHostBroker::GenerateExtensionHostSessionId()
{
	// This intentionally has its own call site and storage from bootId.  It is a
	// native-editor capability, never a host-process identity or launch input.
	std::array<std::uint8_t, IdentityBytes> random{};
	if (!NtSuccess(::BCryptGenRandom(
		nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		return {};
	}
	return BytesToHex(random.data(), random.size());
}

std::string CExtensionHostBroker::NarrowDiagnostic(std::wstring_view value)
{
	if (value.empty()) {
		return {};
	}
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return "extension host diagnostic is too long";
	}
	const int count = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (count <= 0) {
		return "extension host returned an invalid diagnostic";
	}
	std::string result(static_cast<std::size_t>(count), '\0');
	::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
	return result;
}

void CExtensionHostBroker::NotifyObserver(const SExtensionHostLifecycleAction& action) noexcept
{
	if (m_observer) {
		m_observer->OnExtensionHostLifecycleAction(action, GetSnapshot());
	}
}

void CExtensionHostBroker::ExecuteStart(
	const SExtensionHostLifecycleAction& action,
	TimePoint now,
	double jitterUnit)
{
	if (m_identityGeneration != action.generation) {
		const auto nextBootId = m_config.bootIdOverride.empty() ? GenerateBootId() : m_config.bootIdOverride;
		const auto nextExtensionHostSessionId = GenerateExtensionHostSessionId();
		if (nextBootId.empty() || nextExtensionHostSessionId.empty()) {
			m_extensionHostSessionId.clear();
			m_lastDiagnostic = nextBootId.empty()
				? "failed to generate extension host boot identity"
				: "failed to generate extension host native session identity";
			Dispatch(
				m_stateMachine.OnHostStartFailed(action.generation, now, m_lastDiagnostic, jitterUnit),
				now,
				jitterUnit);
			return;
		}
		m_bootId = nextBootId;
		m_extensionHostSessionId = nextExtensionHostSessionId;
		m_pipeName = L"\\\\.\\pipe\\sakura-exthost-" + m_profileHash + L"-" + m_bootId;
		m_identityGeneration = action.generation;
	}
	SExtensionHostLaunchOptions launch;
	launch.nodeExecutable = m_config.nodeExecutable;
	launch.hostBundle = m_config.hostBundle;
	launch.securityShim = m_config.securityShim;
	launch.workingDirectory = m_config.workingDirectory;
	launch.profileHash = m_profileHash;
	launch.bootId = m_bootId;
	launch.pipeName = m_pipeName;
	launch.generation = action.generation;
	launch.brokerProcessId = m_config.brokerProcessId;
	launch.developerInspect = m_config.developerInspect;

	const auto result = m_process->Start(launch);
	if (result.success) {
		m_lastDiagnostic.clear();
		return;
	}
	m_lastDiagnostic = NarrowDiagnostic(result.diagnostic);
	if (m_lastDiagnostic.empty()) {
		m_lastDiagnostic = "extension host process failed to start (error " +
			std::to_string(result.errorCode) + ")";
	}
	Dispatch(
		m_stateMachine.OnHostStartFailed(action.generation, now, m_lastDiagnostic, jitterUnit),
		now,
		jitterUnit);
}

void CExtensionHostBroker::Dispatch(
	CExtensionHostStateMachine::Actions actions,
	TimePoint now,
	double jitterUnit)
{
	for (auto& action : actions) {
		m_pendingActions.push_back(std::move(action));
	}
	if (m_dispatching) {
		return;
	}

	m_dispatching = true;
	while (!m_pendingActions.empty()) {
		auto action = std::move(m_pendingActions.front());
		m_pendingActions.pop_front();
		if (!action.diagnostic.empty()) {
			m_lastDiagnostic = action.diagnostic;
		}
		switch (action.kind) {
		case EExtensionHostLifecycleActionKind::StartHost:
			ExecuteStart(action, now, jitterUnit);
			break;
		case EExtensionHostLifecycleActionKind::ForceTerminate:
			m_process->Terminate(ERROR_TIMEOUT);
			break;
		case EExtensionHostLifecycleActionKind::Stopped:
			m_process->Terminate(ERROR_SUCCESS);
			m_extensionHostSessionId.clear();
			break;
		case EExtensionHostLifecycleActionKind::ScheduleRetry:
		case EExtensionHostLifecycleActionKind::BeginQuiesce:
		case EExtensionHostLifecycleActionKind::RejectPendingHostLost:
		case EExtensionHostLifecycleActionKind::LeaseRejected:
			break;
		}
		NotifyObserver(action);
	}
	m_dispatching = false;
}

void CExtensionHostBroker::AcquireLease(std::uint32_t editorProcessId, TimePoint now, double jitterUnit)
{
	Dispatch(m_stateMachine.AcquireLease(editorProcessId, now), now, jitterUnit);
}

void CExtensionHostBroker::ReleaseLease(std::uint32_t editorProcessId, TimePoint now, double jitterUnit)
{
	Dispatch(m_stateMachine.ReleaseLease(editorProcessId, now), now, jitterUnit);
}

bool CExtensionHostBroker::AcceptHandshake(
	std::uint64_t generation,
	std::uint32_t serverProcessId,
	std::wstring_view bootId,
	TimePoint now,
	double jitterUnit)
{
	if (generation != m_stateMachine.GetGeneration() ||
		m_stateMachine.GetState() != EExtensionHostState::Starting) {
		return false;
	}
	if (serverProcessId == 0 || serverProcessId != m_process->GetProcessId() || bootId != m_bootId) {
		NotifyHostLost(
			generation,
			EExtensionHostLossKind::ProtocolError,
			"extension host handshake identity mismatch",
			now,
			jitterUnit);
		return false;
	}
	Dispatch(m_stateMachine.OnHostReady(generation, now), now, jitterUnit);
	return m_stateMachine.GetState() == EExtensionHostState::Ready ||
		m_stateMachine.GetState() == EExtensionHostState::KeepAlive;
}

void CExtensionHostBroker::NotifyHostLost(
	std::uint64_t generation,
	EExtensionHostLossKind kind,
	std::string diagnostic,
	TimePoint now,
	double jitterUnit)
{
	if (generation != m_stateMachine.GetGeneration()) {
		return;
	}
	m_process->Terminate(ERROR_PROCESS_ABORTED);
	Dispatch(
		m_stateMachine.OnHostLost(generation, now, kind, std::move(diagnostic), jitterUnit),
		now,
		jitterUnit);
}

void CExtensionHostBroker::NotifyQuiesceCompleted(
	std::uint64_t generation,
	TimePoint now,
	double jitterUnit)
{
	Dispatch(m_stateMachine.OnQuiesceCompleted(generation, now), now, jitterUnit);
}

void CExtensionHostBroker::Tick(TimePoint now, double jitterUnit)
{
	if (const auto exitCode = m_process->PollExitCode()) {
		const auto generation = m_stateMachine.GetGeneration();
		const auto state = m_stateMachine.GetState();
		m_process->Terminate(*exitCode);
		if (state == EExtensionHostState::Quiescing) {
			Dispatch(m_stateMachine.OnQuiesceCompleted(generation, now), now, jitterUnit);
		}
		else if (state == EExtensionHostState::Starting || state == EExtensionHostState::Ready ||
			state == EExtensionHostState::KeepAlive) {
			Dispatch(
				m_stateMachine.OnHostLost(
					generation,
					now,
					EExtensionHostLossKind::HostCrash,
					"extension host exited with code " + std::to_string(*exitCode),
					jitterUnit),
				now,
				jitterUnit);
		}
	}
	Dispatch(m_stateMachine.Tick(now, jitterUnit), now, jitterUnit);
}

void CExtensionHostBroker::Shutdown(TimePoint now, double jitterUnit)
{
	// Shutdown revokes the native-editor capability immediately, even while the
	// child process is completing its orderly quiesce.
	m_extensionHostSessionId.clear();
	Dispatch(m_stateMachine.Shutdown(now), now, jitterUnit);
}

SExtensionHostBrokerSnapshot CExtensionHostBroker::GetSnapshot() const
{
	const auto state = m_stateMachine.GetState();
	return {
		state,
		m_stateMachine.GetGeneration(),
		m_process->GetProcessId(),
		m_stateMachine.GetRetryCount(),
		m_stateMachine.GetLeaseOwnerCount(),
		m_stateMachine.GetLeaseCount(),
		m_profileHash,
		m_bootId,
		HasNativeExtensionHostSession(state) ? m_extensionHostSessionId : std::wstring{},
		m_pipeName,
		m_lastDiagnostic,
	};
}
