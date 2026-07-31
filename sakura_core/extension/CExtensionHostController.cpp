/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostController.h"

#include "util/file.h"

#include <array>
#include <system_error>

namespace {

constexpr std::size_t kMaximumTrackedEditorLeaseOwners = 256;
constexpr std::uint32_t kMaximumEditorLeasesPerOwner = 1024;
constexpr ULONGLONG kEditorLeaseHealthCheckIntervalMilliseconds = 2000;

bool IsRegularFile(const std::filesystem::path& path)
{
	std::error_code error;
	return !path.empty() && std::filesystem::is_regular_file(path, error) && !error;
}

std::filesystem::path FindNodeExecutable(const std::filesystem::path& executableDirectory)
{
	for (const auto& candidate : {
		executableDirectory / L"exthost/node.exe",
		executableDirectory / L"node.exe",
	}) {
		if (IsRegularFile(candidate)) return candidate;
	}
	std::array<wchar_t, 32768> path{};
	const DWORD length = ::SearchPathW(
		nullptr, L"node.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
	return length > 0 && length < path.size()
		? std::filesystem::path(std::wstring_view(path.data(), length)) : std::filesystem::path{};
}

std::filesystem::path FindDevelopmentBundle(std::filesystem::path directory)
{
	for (int parent = 0; parent < 8 && !directory.empty(); ++parent) {
		const auto candidate = directory / L"src/exthost/dist/extension-host.js";
		if (IsRegularFile(candidate)) return candidate;
		const auto next = directory.parent_path();
		if (next == directory) break;
		directory = next;
	}
	return {};
}

std::filesystem::path FindDevelopmentShim(std::filesystem::path directory)
{
	for (int parent = 0; parent < 8 && !directory.empty(); ++parent) {
		const auto candidate = directory / L"src/exthost/dist/sakura_exthost_security.node";
		if (IsRegularFile(candidate)) return candidate;
		const auto next = directory.parent_path();
		if (next == directory) break;
		directory = next;
	}
	return {};
}

std::string NarrowError(std::wstring_view value)
{
	if (value.empty()) return {};
	const int bytes = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return "extension host runtime is unavailable";
	std::string result(static_cast<std::size_t>(bytes), '\0');
	::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
	return result;
}

} // namespace

CExtensionHostController::CExtensionHostController(
	std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle> secretVaultGrantLifecycle)
{
	if (secretVaultGrantLifecycle) {
		m_secretVaultGrantCoordinator = std::make_unique<CExtensionHostSecretVaultGrantCoordinator>(
			std::move(secretVaultGrantLifecycle));
	}
}

CExtensionHostController::~CExtensionHostController()
{
	Shutdown();
}

bool CExtensionHostController::Initialize(
	const std::filesystem::path& profileDirectory,
	std::wstring& diagnostic)
{
	Shutdown();
	m_shutdown = false;
	if (!m_sharedState.CreateForBroker(profileDirectory, diagnostic)) return false;

	const auto executableDirectory = GetExeFileName().parent_path();
	const auto nodeExecutable = FindNodeExecutable(executableDirectory);
	auto hostBundle = executableDirectory / L"exthost/extension-host.js";
	if (!IsRegularFile(hostBundle)) hostBundle = FindDevelopmentBundle(executableDirectory);
	auto securityShim = executableDirectory / L"exthost/sakura_exthost_security.node";
	if (!IsRegularFile(securityShim)) securityShim = FindDevelopmentShim(executableDirectory);

	if (!IsRegularFile(nodeExecutable) || !IsRegularFile(hostBundle) || !IsRegularFile(securityShim)) {
		diagnostic = L"Extension host runtime is incomplete:";
		if (!IsRegularFile(nodeExecutable)) diagnostic += L" node.exe";
		if (!IsRegularFile(hostBundle)) diagnostic += L" extension-host.js";
		if (!IsRegularFile(securityShim)) diagnostic += L" sakura_exthost_security.node";
		PublishUnavailable(NarrowError(diagnostic));
		return false;
	}

	SExtensionHostBrokerConfig config;
	config.nodeExecutable = nodeExecutable;
	config.hostBundle = hostBundle;
	config.securityShim = securityShim;
	config.workingDirectory = hostBundle.parent_path();
	config.profileDirectory = profileDirectory;
	config.brokerProcessId = ::GetCurrentProcessId();
	try {
		m_broker = std::make_unique<CExtensionHostBroker>(
			std::move(config), std::make_unique<CExtensionHostProcess>(),
			static_cast<IExtensionHostBrokerObserver*>(this));
	} catch (const std::exception& error) {
		diagnostic = L"Cannot initialize extension host broker";
		PublishUnavailable(error.what());
		return false;
	}
	PublishSnapshot();
	diagnostic.clear();
	return true;
}

void CExtensionHostController::Shutdown() noexcept
{
	if (m_shutdown) return;
	m_shutdown = true;
	for (const auto& [editorProcessId, lease] : m_editorLeases) {
		(void)editorProcessId;
		CloseLeaseProcessHandle(lease.process);
	}
	m_editorLeases.clear();
	m_nextEditorLeaseHealthCheckTick = 0;
	// Broker::Shutdown clears the native session identity while dispatching Stopped.
	// Fence the Secret Vault capability first so no callback needs that disappearing snapshot field.
	if (m_secretVaultGrantCoordinator) m_secretVaultGrantCoordinator->Shutdown();
	if (m_broker) {
		m_broker->Shutdown();
		PublishSnapshot();
		m_broker.reset();
	}
	m_sharedState.Close();
}

void CExtensionHostController::Tick() noexcept
{
	if (!m_broker || m_shutdown) return;
	try {
		const ULONGLONG now = ::GetTickCount64();
		if (now >= m_nextEditorLeaseHealthCheckTick) {
			m_nextEditorLeaseHealthCheckTick = now + kEditorLeaseHealthCheckIntervalMilliseconds;
			ReclaimTerminatedEditorLeases();
		}
		m_broker->Tick();
		PublishSnapshot();
	} catch (...) {
		RevokeSecretVaultGrant();
		PublishUnavailable("extension host broker tick failed");
	}
}

bool CExtensionHostController::AcquireLease(std::uint32_t editorProcessId) noexcept
{
	if (!m_broker || m_shutdown || editorProcessId == 0) return false;
	const auto found = m_editorLeases.find(editorProcessId);
	if (found == m_editorLeases.end()) {
		if (m_editorLeases.size() >= kMaximumTrackedEditorLeaseOwners) {
			FailClosedSecretVaultGrant("extension host editor lease owner limit exceeded");
			return false;
		}
		const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, editorProcessId);
		if (!process || IsEditorProcessTerminated(process)) {
			CloseLeaseProcessHandle(process);
			return false;
		}
		try {
			m_editorLeases.emplace(editorProcessId, SEditorLeaseOwner{ 1, process });
		} catch (...) {
			CloseLeaseProcessHandle(process);
			FailClosedSecretVaultGrant("extension host editor lease tracking failed");
			return false;
		}
	} else {
		// A PID can be reused after its original editor exits. The pinned handle
		// identifies that original process object, so never extend a dead lease.
		if (IsEditorProcessTerminated(found->second.process)) {
			ReclaimTerminatedEditorLeases();
			return false;
		}
		if (found->second.leaseCount >= kMaximumEditorLeasesPerOwner) {
			FailClosedSecretVaultGrant("extension host editor lease count overflow");
			return false;
		}
		++found->second.leaseCount;
	}

	bool secretVaultLeaseAcquired = false;
	bool brokerLeaseAcquired = false;
	try {
		if (m_secretVaultGrantCoordinator) {
			secretVaultLeaseAcquired = true;
			const auto result = m_secretVaultGrantCoordinator->AcquireEditorLease(editorProcessId);
			if (result == EExtensionHostSecretVaultLeaseAcquireResult::Rejected) {
				RollbackAcquiredLease(editorProcessId, false, secretVaultLeaseAcquired);
				FailClosedSecretVaultGrant("extension host secret vault lease registration failed");
				return false;
			}
		}
		brokerLeaseAcquired = true;
		m_broker->AcquireLease(editorProcessId);
		PublishSnapshot();
		const auto snapshot = m_broker->GetSnapshot();
		const bool brokerAccepted = snapshot.state != EExtensionHostState::Stopped;
		const bool vaultActive = !m_secretVaultGrantCoordinator ||
			m_secretVaultGrantCoordinator->IsActiveForGeneration(snapshot.generation);
		if (!brokerAccepted || !vaultActive) {
			RollbackAcquiredLease(editorProcessId, brokerLeaseAcquired, secretVaultLeaseAcquired);
			FailClosedSecretVaultGrant("extension host lease acquisition was not accepted");
			return false;
		}
		return true;
	} catch (...) {
		RollbackAcquiredLease(editorProcessId, brokerLeaseAcquired, secretVaultLeaseAcquired);
		RevokeSecretVaultGrant();
		PublishUnavailable("extension host lease acquisition failed");
		return false;
	}
}

void CExtensionHostController::ReleaseLease(std::uint32_t editorProcessId) noexcept
{
	if (!m_broker || m_shutdown || editorProcessId == 0) return;
	ReleaseTrackedLease(editorProcessId);
}

void CExtensionHostController::RollbackAcquiredLease(
	std::uint32_t editorProcessId,
	bool releaseBrokerLease,
	bool releaseSecretVaultLease) noexcept
{
	const auto found = m_editorLeases.find(editorProcessId);
	HANDLE processToClose = nullptr;
	if (found != m_editorLeases.end()) {
		if (found->second.leaseCount > 1) {
			--found->second.leaseCount;
		} else {
			processToClose = found->second.process;
			m_editorLeases.erase(found);
		}
	}
	ReleaseLeaseComponents(editorProcessId, releaseBrokerLease, releaseSecretVaultLease);
	CloseLeaseProcessHandle(processToClose);
}

void CExtensionHostController::ReleaseTrackedLease(std::uint32_t editorProcessId) noexcept
{
	const auto found = m_editorLeases.find(editorProcessId);
	if (found == m_editorLeases.end()) return;
	HANDLE processToClose = nullptr;
	if (found->second.leaseCount > 1) {
		--found->second.leaseCount;
	} else {
		processToClose = found->second.process;
		m_editorLeases.erase(found);
	}
	ReleaseLeaseComponents(editorProcessId, true, m_secretVaultGrantCoordinator != nullptr);
	CloseLeaseProcessHandle(processToClose);
}

void CExtensionHostController::ReleaseLeaseComponents(
	std::uint32_t editorProcessId,
	bool releaseBrokerLease,
	bool releaseSecretVaultLease) noexcept
{
	bool releaseFailed = false;
	try {
		if (releaseSecretVaultLease && m_secretVaultGrantCoordinator &&
			!m_secretVaultGrantCoordinator->ReleaseEditorLease(editorProcessId)) {
			releaseFailed = true;
		}
	} catch (...) {
		releaseFailed = true;
	}
	try {
		if (releaseBrokerLease && m_broker) {
			m_broker->ReleaseLease(editorProcessId);
			PublishSnapshot();
		}
	} catch (...) {
		releaseFailed = true;
	}
	if (releaseFailed) {
		RevokeSecretVaultGrant();
		PublishUnavailable("extension host lease release failed");
	}
}

bool CExtensionHostController::IsEditorProcessTerminated(HANDLE process) noexcept
{
	const DWORD waitResult = ::WaitForSingleObject(process, 0);
	// WAIT_FAILED cannot be recovered safely: treat it as an invalid identity.
	return waitResult != WAIT_TIMEOUT;
}

void CExtensionHostController::CloseLeaseProcessHandle(HANDLE process) noexcept
{
	if (process && process != INVALID_HANDLE_VALUE) ::CloseHandle(process);
}

void CExtensionHostController::ReclaimTerminatedEditorLeases() noexcept
{
	std::array<std::uint32_t, kMaximumTrackedEditorLeaseOwners> terminatedProcessIds{};
	std::size_t terminatedCount = 0;
	for (const auto& [editorProcessId, lease] : m_editorLeases) {
		if (lease.leaseCount != 0 && IsEditorProcessTerminated(lease.process)) {
			terminatedProcessIds[terminatedCount++] = editorProcessId;
		}
	}

	for (std::size_t index = 0; index < terminatedCount; ++index) {
		const auto editorProcessId = terminatedProcessIds[index];
		for (;;) {
			const auto found = m_editorLeases.find(editorProcessId);
			if (found == m_editorLeases.end()) break;
			ReleaseTrackedLease(editorProcessId);
			if (m_shutdown || !m_broker) return;
		}
	}
}

bool CExtensionHostController::RefreshSecretVaultExtensionInventory(
	const std::vector<std::string>& extensionIds) noexcept
{
	if (!m_secretVaultGrantCoordinator) return true;
	if (m_shutdown) return false;
	if (m_secretVaultGrantCoordinator->ReplaceInstalledExtensionInventory(extensionIds)) return true;
	FailClosedSecretVaultGrant("extension host secret vault inventory refresh failed");
	return false;
}

bool CExtensionHostController::AcceptHandshake(
	std::uint64_t generation,
	std::uint32_t serverProcessId) noexcept
{
	if (!m_broker || m_shutdown || generation == 0 || serverProcessId == 0) return false;
	try {
		const auto snapshot = m_broker->GetSnapshot();
		const bool accepted = m_broker->AcceptHandshake(generation, serverProcessId, snapshot.bootId);
		PublishSnapshot();
		return accepted;
	} catch (...) {
		RevokeSecretVaultGrant();
		PublishUnavailable("extension host handshake failed");
		return false;
	}
}

void CExtensionHostController::NotifyHostLost(
	std::uint64_t generation,
	std::uint32_t errorCode) noexcept
{
	if (!m_broker || m_shutdown || generation == 0) return;
	try {
		RevokeSecretVaultGrant();
		m_broker->NotifyHostLost(
			generation, EExtensionHostLossKind::HostCrash,
			"editor connection lost (error " + std::to_string(errorCode) + ")");
		PublishSnapshot();
	} catch (...) {
		PublishUnavailable("extension host loss handling failed");
	}
}

SExtensionHostBrokerSnapshot CExtensionHostController::Snapshot() const
{
	return m_broker ? m_broker->GetSnapshot() : m_unavailableSnapshot;
}

void CExtensionHostController::OnExtensionHostLifecycleAction(
	const SExtensionHostLifecycleAction& action,
	const SExtensionHostBrokerSnapshot& snapshot) noexcept
{
	if (m_secretVaultGrantCoordinator) {
		switch (action.kind) {
		case EExtensionHostLifecycleActionKind::StartHost:
			// Broker invokes its observer synchronously while AcquireLease is still in
			// progress. The session identity exists now, and no per-editor Secret Vault
			// capability is issued until this activation (and its CAS) succeeds.
			if (snapshot.state == EExtensionHostState::Starting &&
				!m_secretVaultGrantCoordinator->Activate(
					{ snapshot.generation, snapshot.extensionHostSessionId })) {
				FailClosedSecretVaultGrant("extension host secret vault activation failed");
			}
			break;
		case EExtensionHostLifecycleActionKind::RejectPendingHostLost:
		case EExtensionHostLifecycleActionKind::BeginQuiesce:
		case EExtensionHostLifecycleActionKind::ForceTerminate:
		case EExtensionHostLifecycleActionKind::Stopped:
			// The coordinator keeps the last valid session identity, allowing this
			// revoke -> deactivate sequence to complete before/after broker cleanup.
			RevokeSecretVaultGrant();
			break;
		case EExtensionHostLifecycleActionKind::ScheduleRetry:
		case EExtensionHostLifecycleActionKind::LeaseRejected:
			break;
		}
	}
	m_sharedState.Publish(snapshot);
}

void CExtensionHostController::PublishSnapshot() noexcept
{
	if (m_broker) m_sharedState.Publish(m_broker->GetSnapshot());
}

void CExtensionHostController::PublishUnavailable(std::string diagnostic) noexcept
{
	m_unavailableSnapshot = {};
	m_unavailableSnapshot.state = EExtensionHostState::Stopped;
	m_unavailableSnapshot.lastDiagnostic = std::move(diagnostic);
	m_sharedState.Publish(m_unavailableSnapshot);
}

void CExtensionHostController::RevokeSecretVaultGrant() noexcept
{
	if (m_secretVaultGrantCoordinator) m_secretVaultGrantCoordinator->RevokeAndDeactivate();
}

void CExtensionHostController::FailClosedSecretVaultGrant(std::string diagnostic) noexcept
{
	RevokeSecretVaultGrant();
	if (!m_broker || m_shutdown) return;
	try {
		const auto snapshot = m_broker->GetSnapshot();
		if (snapshot.generation != 0) {
			m_broker->NotifyHostLost(
				snapshot.generation,
				EExtensionHostLossKind::ProtocolError,
				std::move(diagnostic));
		}
	} catch (...) {
		PublishUnavailable("extension host secret vault fail-closed handling failed");
	}
}
