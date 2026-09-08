/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhLoginSession.h"

#include "platform/process/WindowsExecutableResolver.h"

#include <algorithm>
#include <limits>

namespace senp::github {
namespace {

constexpr std::uint32_t kLoginTimeoutMilliseconds = 5u * 60u * 1000u;
constexpr std::size_t kLoginOutputBytes = 64u * 1024u;
constexpr std::size_t kLoginErrorBytes = 64u * 1024u;

const std::vector<std::wstring>& RemovedEnvironment()
{
	static const std::vector<std::wstring> values{
		L"GH_TOKEN", L"GITHUB_TOKEN", L"GH_ENTERPRISE_TOKEN", L"GITHUB_ENTERPRISE_TOKEN",
		L"GH_HOST", L"GH_REPO", L"GH_DEBUG", L"DEBUG", L"GH_PAGER", L"PAGER",
		L"GH_BROWSER", L"BROWSER", L"GH_FORCE_TTY", L"GH_HTTP_UNIX_SOCKET",
		L"GH_EDITOR", L"GIT_EDITOR", L"VISUAL", L"EDITOR",
	};
	return values;
}

bool IsHostname(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 253 || value.front() == L'.' || value.back() == L'.') return false;
	std::size_t begin{};
	while (begin < value.size()) {
		const auto end = value.find(L'.', begin);
		const auto label = value.substr(begin, (end == std::wstring_view::npos ? value.size() : end) - begin);
		if (label.empty() || label.size() > 63 || label.front() == L'-' || label.back() == L'-'
			|| !std::ranges::all_of(label, [](const wchar_t character) {
				return (character >= L'a' && character <= L'z') || (character >= L'0' && character <= L'9')
					|| character == L'-';
			})) return false;
		if (end == std::wstring_view::npos) break;
		begin = end + 1;
	}
	return true;
}

GhLoginTerminal OutcomeTerminal(const platform::process::EBoundedProcessStatus value) noexcept
{
	switch (value) {
	case platform::process::EBoundedProcessStatus::Succeeded: return GhLoginTerminal::Succeeded;
	case platform::process::EBoundedProcessStatus::TimedOut: return GhLoginTerminal::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhLoginTerminal::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhLoginTerminal::OutputLimitExceeded;
	case platform::process::EBoundedProcessStatus::ObserverRejected: return GhLoginTerminal::UnsupportedInteractiveFlow;
	default: return GhLoginTerminal::Failed;
	}
}

GhLoginTerminal ConnectionTerminal(const GhConnectionTerminal value) noexcept
{
	switch (value) {
	case GhConnectionTerminal::Succeeded: return GhLoginTerminal::Succeeded;
	case GhConnectionTerminal::TimedOut: return GhLoginTerminal::TimedOut;
	case GhConnectionTerminal::Cancelled: return GhLoginTerminal::Cancelled;
	case GhConnectionTerminal::OutputLimitExceeded: return GhLoginTerminal::OutputLimitExceeded;
	case GhConnectionTerminal::ToolUnavailable: return GhLoginTerminal::ToolUnavailable;
	case GhConnectionTerminal::UnsupportedVersion: return GhLoginTerminal::UnsupportedVersion;
	case GhConnectionTerminal::InvalidRequest: return GhLoginTerminal::InvalidRequest;
	case GhConnectionTerminal::Stale: return GhLoginTerminal::Stale;
	case GhConnectionTerminal::Closed: return GhLoginTerminal::Closed;
	default: return GhLoginTerminal::Failed;
	}
}

GhLoginPhase PhaseFor(const GhLoginTerminal value) noexcept
{
	if (value == GhLoginTerminal::Succeeded) return GhLoginPhase::Succeeded;
	if (value == GhLoginTerminal::Cancelled) return GhLoginPhase::Cancelled;
	if (value == GhLoginTerminal::Closed) return GhLoginPhase::Closed;
	return GhLoginPhase::Failed;
}

std::wstring StorageNotice(const GhAccountIdentity& account)
{
	return L"Credential storage reported by GitHub CLI: " + account.TokenSource();
}

} // namespace

class CGhLoginSession::State final {
private:
	std::mutex mutex;
	GhLoginPhase phase{ GhLoginPhase::Idle };
	std::optional<GhLoginTerminal> terminal;
	std::string output;
	std::wstring storageNotice;
	std::uint64_t epoch{ 1 }, activeOperation{}, nextOperation{ 1 };
	bool closed{};
	friend class CGhLoginSession;
	friend class LoginOutputObserver;
};

class LoginOutputObserver final : public platform::process::IBoundedProcessOutputObserver {
public:
	LoginOutputObserver(std::weak_ptr<CGhLoginSession::State> state,
		const std::uint64_t operation, const std::uint64_t epoch) :
		m_state(std::move(state)), m_operation(operation), m_epoch(epoch) {}
	bool OnOutput(platform::process::EBoundedProcessStream,
		const std::span<const std::uint8_t> bytes) override
	{
		if (std::ranges::any_of(bytes, [](const std::uint8_t value) {
			return value == 0 || (value < 0x20 && value != '\r' && value != '\n' && value != '\t');
		})) return false;
		const auto state = m_state.lock();
		if (!state) return false;
		std::scoped_lock lock(state->mutex);
		if (state->closed || state->activeOperation != m_operation || state->epoch != m_epoch
			|| state->output.size() + bytes.size() > kLoginOutputBytes) return false;
		state->output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return true;
	}
private:
	std::weak_ptr<CGhLoginSession::State> m_state;
	std::uint64_t m_operation{}, m_epoch{};
};

GhLoginSnapshot::GhLoginSnapshot(const GhLoginPhase phase, std::optional<GhLoginTerminal> terminal,
	std::string outputUtf8, std::wstring storageNotice, std::wstring sharedAuthenticationNotice) :
	m_phase(phase), m_terminal(terminal), m_outputUtf8(std::move(outputUtf8)),
	m_storageNotice(std::move(storageNotice)),
	m_sharedAuthenticationNotice(std::move(sharedAuthenticationNotice)) {}

CGhLoginSession::CGhLoginSession(std::shared_ptr<const IGhToolPlatform> platform,
	CGhConnectionLifecycle& lifecycle, std::wstring workingDirectory,
	std::wstring configurationDirectory) : m_platform(std::move(platform)), m_lifecycle(lifecycle),
	m_workingDirectory(std::move(workingDirectory)), m_configurationDirectory(std::move(configurationDirectory)),
	m_state(std::make_shared<State>()) {}

CGhLoginSession::~CGhLoginSession()
{
	Close();
}

GhLoginTerminal CGhLoginSession::Run(const GhToolProbe& probe, std::wstring hostname, HANDLE stop)
try {
	if (!m_platform || probe.Status() == GhToolAvailability::NotInstalled) return GhLoginTerminal::ToolUnavailable;
	if (probe.Status() == GhToolAvailability::UnsupportedVersion) return GhLoginTerminal::UnsupportedVersion;
	if (probe.Status() != GhToolAvailability::Available || !probe.Version() || !probe.Version()->Supported()
		|| !platform::IsAbsoluteWindowsPath(probe.ExecutablePath())
		|| !platform::IsAbsoluteWindowsPath(m_workingDirectory)
		|| !platform::IsAbsoluteWindowsPath(m_configurationDirectory) || !IsHostname(hostname)) {
		return GhLoginTerminal::InvalidRequest;
	}
	std::uint64_t operation{}, epoch{};
	{
		std::scoped_lock lock(m_state->mutex);
		if (m_state->closed) return GhLoginTerminal::Closed;
		if (m_state->activeOperation != 0 || m_state->nextOperation == std::numeric_limits<std::uint64_t>::max()) {
			return GhLoginTerminal::InvalidRequest;
		}
		operation = m_state->nextOperation++;
		epoch = m_state->epoch;
		m_state->activeOperation = operation;
		m_state->phase = GhLoginPhase::SigningIn;
		m_state->terminal.reset();
		m_state->output.clear();
		m_state->storageNotice.clear();
	}
	auto observer = std::make_shared<LoginOutputObserver>(m_state, operation, epoch);
	GhProcessInvocation invocation(probe.ExecutablePath(), m_workingDirectory,
		{ L"auth", L"login", L"--hostname", hostname, L"--web", L"--skip-ssh-key" },
		{ { L"GH_CONFIG_DIR", m_configurationDirectory }, { L"GH_NO_UPDATE_NOTIFIER", L"1" },
			{ L"GH_SPINNER_DISABLED", L"1" } }, RemovedEnvironment(), kLoginTimeoutMilliseconds,
		kLoginOutputBytes, kLoginErrorBytes, observer);
	const auto outcome = m_platform->Run(invocation, stop);
	auto terminal = OutcomeTerminal(outcome.Status());
	if (terminal == GhLoginTerminal::Succeeded) {
		{
			std::scoped_lock lock(m_state->mutex);
			if (m_state->closed || m_state->activeOperation != operation || m_state->epoch != epoch) {
				return m_state->closed ? GhLoginTerminal::Closed : GhLoginTerminal::Stale;
			}
		}
		auto attempt = m_lifecycle.Begin(hostname, std::nullopt);
		if (!attempt) {
			terminal = GhLoginTerminal::Stale;
		} else {
			terminal = ConnectionTerminal(m_lifecycle.Complete(std::move(*attempt), probe, stop).Terminal());
		}
	}
	{
		std::scoped_lock lock(m_state->mutex);
		if (m_state->closed || m_state->activeOperation != operation || m_state->epoch != epoch) {
			return m_state->closed ? GhLoginTerminal::Closed : GhLoginTerminal::Stale;
		}
		m_state->activeOperation = 0;
		m_state->terminal = terminal;
		m_state->phase = PhaseFor(terminal);
		if (terminal == GhLoginTerminal::Succeeded) {
			const auto connected = m_lifecycle.Snapshot();
			if (connected.Account()) m_state->storageNotice = StorageNotice(*connected.Account());
		}
	}
	return terminal;
} catch (...) {
	std::scoped_lock lock(m_state->mutex);
	if (!m_state->closed) {
		m_state->activeOperation = 0;
		m_state->phase = GhLoginPhase::Failed;
		m_state->terminal = GhLoginTerminal::Failed;
	}
	return m_state->closed ? GhLoginTerminal::Closed : GhLoginTerminal::Failed;
}

GhLoginSnapshot CGhLoginSession::Snapshot()
{
	std::scoped_lock lock(m_state->mutex);
	return { m_state->phase, m_state->terminal, m_state->output, m_state->storageNotice,
		L"Disconnecting Sakura Editor keeps GitHub CLI's shared authentication unchanged." };
}

void CGhLoginSession::Disconnect() noexcept
{
	{
		std::scoped_lock lock(m_state->mutex);
		if (m_state->closed) return;
		if (m_state->epoch != std::numeric_limits<std::uint64_t>::max()) ++m_state->epoch;
		m_state->activeOperation = 0;
		m_state->phase = GhLoginPhase::Disconnected;
		m_state->terminal.reset();
		m_state->storageNotice.clear();
	}
	m_lifecycle.Disconnect();
}

void CGhLoginSession::Close() noexcept
{
	{
		std::scoped_lock lock(m_state->mutex);
		if (m_state->closed) return;
		m_state->closed = true;
		m_state->activeOperation = 0;
		m_state->phase = GhLoginPhase::Closed;
		m_state->terminal = GhLoginTerminal::Closed;
		m_state->storageNotice.clear();
	}
	m_lifecycle.Close();
}

} // namespace senp::github
