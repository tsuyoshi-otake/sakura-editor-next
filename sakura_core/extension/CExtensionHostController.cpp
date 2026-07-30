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
		m_broker->Tick();
		PublishSnapshot();
	} catch (...) {
		PublishUnavailable("extension host broker tick failed");
	}
}

bool CExtensionHostController::AcquireLease(std::uint32_t editorProcessId) noexcept
{
	if (!m_broker || m_shutdown || editorProcessId == 0) return false;
	try {
		m_broker->AcquireLease(editorProcessId);
		PublishSnapshot();
		return m_broker->GetSnapshot().state != EExtensionHostState::Stopped;
	} catch (...) {
		PublishUnavailable("extension host lease acquisition failed");
		return false;
	}
}

void CExtensionHostController::ReleaseLease(std::uint32_t editorProcessId) noexcept
{
	if (!m_broker || m_shutdown || editorProcessId == 0) return;
	try {
		m_broker->ReleaseLease(editorProcessId);
		PublishSnapshot();
	} catch (...) {
		PublishUnavailable("extension host lease release failed");
	}
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
	[[maybe_unused]] const SExtensionHostLifecycleAction& action,
	const SExtensionHostBrokerSnapshot& snapshot) noexcept
{
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
