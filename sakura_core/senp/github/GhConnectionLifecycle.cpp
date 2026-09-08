/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhConnectionLifecycle.h"
#include "platform/process/WindowsExecutableResolver.h"
#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <limits>
#include <mutex>

namespace senp::github {
namespace {

using Json = platform::serialization::JsoncValue;
constexpr std::uint32_t kStatusTimeoutMilliseconds = 5000;
constexpr std::uint32_t kTokenTimeoutMilliseconds = 5000;
constexpr std::uint32_t kIdentityTimeoutMilliseconds = 10000;
constexpr std::size_t kStatusOutputBytes = 256u * 1024u;
constexpr std::size_t kTokenOutputBytes = 16u * 1024u;
constexpr std::size_t kIdentityOutputBytes = 64u * 1024u;
constexpr std::size_t kErrorBytes = 64u * 1024u;

class ByteBufferGuard final {
public:
	explicit ByteBufferGuard(std::vector<std::uint8_t>& value) noexcept : m_value(value) {}
	~ByteBufferGuard()
	{
		if (!m_value.empty()) ::SecureZeroMemory(m_value.data(), m_value.size());
	}
	ByteBufferGuard(const ByteBufferGuard&) = delete;
	ByteBufferGuard& operator=(const ByteBufferGuard&) = delete;
private:
	std::vector<std::uint8_t>& m_value;
};

class WideStringGuard final {
public:
	explicit WideStringGuard(std::wstring& value) noexcept : m_value(value) {}
	~WideStringGuard()
	{
		if (!m_value.empty()) ::SecureZeroMemory(m_value.data(), m_value.size() * sizeof(wchar_t));
	}
	WideStringGuard(const WideStringGuard&) = delete;
	WideStringGuard& operator=(const WideStringGuard&) = delete;
private:
	std::wstring& m_value;
};

class SensitiveToken final {
public:
	SensitiveToken() = default;
	explicit SensitiveToken(std::wstring value) noexcept : m_value(std::move(value)) {}
	~SensitiveToken() { Clear(); }
	SensitiveToken(SensitiveToken&& other) noexcept { m_value.swap(other.m_value); }
	SensitiveToken& operator=(SensitiveToken&& other) noexcept
	{
		if (this != &other) {
			Clear();
			m_value.swap(other.m_value);
		}
		return *this;
	}
	SensitiveToken(const SensitiveToken&) = delete;
	SensitiveToken& operator=(const SensitiveToken&) = delete;
	[[nodiscard]] bool Empty() const noexcept { return m_value.empty(); }
	[[nodiscard]] std::wstring_view Text() const noexcept { return m_value; }
	void Clear() noexcept
	{
		if (!m_value.empty()) ::SecureZeroMemory(m_value.data(), m_value.size() * sizeof(wchar_t));
		m_value.clear();
	}
private:
	std::wstring m_value;
};

bool EqualsInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept
{
	return left.size() == right.size() && ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool IsHostname(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 253 || value.front() == L'.' || value.back() == L'.') return false;
	std::size_t begin = 0;
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

bool IsLogin(const std::wstring_view value) noexcept
{
	return !value.empty() && value.size() <= 100 && value.front() != L'-' && value.back() != L'-'
		&& std::ranges::all_of(value, [](const wchar_t character) {
			return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')
				|| (character >= L'0' && character <= L'9') || character == L'-';
		});
}

const Json::Object* AsObject(const Json& value) noexcept
{
	return std::get_if<Json::Object>(&value.Value());
}

const std::wstring* StringMember(const Json::Object& object, const std::wstring_view name) noexcept
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<std::wstring>(&found->second.Value());
}

const bool* BoolMember(const Json::Object& object, const std::wstring_view name) noexcept
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<bool>(&found->second.Value());
}

std::string Text(const std::vector<std::uint8_t>& bytes)
{
	return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

GhConnectionTerminal ProcessTerminal(const platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::TimedOut: return GhConnectionTerminal::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhConnectionTerminal::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhConnectionTerminal::OutputLimitExceeded;
	case platform::process::EBoundedProcessStatus::ObserverRejected: return GhConnectionTerminal::Failed;
	case platform::process::EBoundedProcessStatus::InvalidRequest: return GhConnectionTerminal::InvalidRequest;
	case platform::process::EBoundedProcessStatus::LaunchFailed: return GhConnectionTerminal::Failed;
	case platform::process::EBoundedProcessStatus::Failed: return GhConnectionTerminal::Failed;
	case platform::process::EBoundedProcessStatus::Succeeded: return GhConnectionTerminal::Succeeded;
	}
	return GhConnectionTerminal::Failed;
}

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

std::vector<std::pair<std::wstring, std::wstring>> Environment(
	const std::wstring_view configurationDirectory, std::optional<std::wstring_view> token = std::nullopt)
{
	std::vector<std::pair<std::wstring, std::wstring>> values{
		{ L"GH_PROMPT_DISABLED", L"1" }, { L"GH_NO_UPDATE_NOTIFIER", L"1" },
		{ L"GH_SPINNER_DISABLED", L"1" }, { L"GH_CONFIG_DIR", std::wstring(configurationDirectory) },
	};
	if (token) values.emplace_back(L"GH_TOKEN", *token);
	return values;
}

GhProcessInvocation Invocation(std::wstring executable, std::wstring workingDirectory,
	std::vector<std::wstring> arguments, const std::wstring_view configurationDirectory,
	const std::uint32_t timeout, const std::size_t outputLimit,
	const std::size_t errorLimit, std::optional<std::wstring_view> token = std::nullopt,
	std::shared_ptr<platform::process::IBoundedProcessOutputObserver> outputObserver = nullptr)
{
	return { std::move(executable), std::move(workingDirectory), std::move(arguments),
		Environment(configurationDirectory, token), RemovedEnvironment(), timeout, outputLimit, errorLimit,
		std::move(outputObserver) };
}

class CWindowsGhAccountCredential final : public IGhAccountCredential {
public:
	CWindowsGhAccountCredential(std::shared_ptr<const IGhToolPlatform> platform,
		std::wstring executable, std::wstring workingDirectory,
		std::wstring configurationDirectory, SensitiveToken token) :
		m_platform(std::move(platform)), m_executable(std::move(executable)),
		m_workingDirectory(std::move(workingDirectory)),
		m_configurationDirectory(std::move(configurationDirectory)), m_token(std::move(token)) {}
	~CWindowsGhAccountCredential() override { Revoke(); }
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>& arguments,
		const std::uint32_t timeoutMilliseconds, const std::size_t maximumOutputBytes,
		const std::size_t maximumErrorBytes, HANDLE stop) override
	{
		return Run(arguments, timeoutMilliseconds, maximumOutputBytes,
			maximumErrorBytes, nullptr, stop);
	}
	GhProcessOutcome RunAuthenticatedStreaming(const std::vector<std::wstring>& arguments,
		const std::uint32_t timeoutMilliseconds, const std::size_t maximumOutputBytes,
		const std::size_t maximumErrorBytes,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> outputObserver,
		HANDLE stop) override
	{
		if (!outputObserver) {
			return { platform::process::EBoundedProcessStatus::InvalidRequest, -1, {}, {} };
		}
		return Run(arguments, timeoutMilliseconds, maximumOutputBytes,
			maximumErrorBytes, std::move(outputObserver), stop);
	}
	void Revoke() noexcept override
	{
		std::scoped_lock lock(m_mutex);
		m_revoked = true;
		m_token.Clear();
	}
private:
	GhProcessOutcome Run(const std::vector<std::wstring>& arguments,
		const std::uint32_t timeoutMilliseconds, const std::size_t maximumOutputBytes,
		const std::size_t maximumErrorBytes,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> outputObserver,
		HANDLE stop)
	{
		std::wstring token;
		{
			std::scoped_lock lock(m_mutex);
			if (m_revoked || !m_platform || m_token.Empty()) {
				return { platform::process::EBoundedProcessStatus::InvalidRequest, -1, {}, {} };
			}
			token = m_token.Text();
		}
		try {
			auto invocation = Invocation(m_executable, m_workingDirectory, arguments,
				m_configurationDirectory, timeoutMilliseconds, maximumOutputBytes,
				maximumErrorBytes, token, std::move(outputObserver));
			auto result = m_platform->Run(invocation, stop);
			::SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
			return result;
		} catch (...) {
			::SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
			return { platform::process::EBoundedProcessStatus::InvalidRequest, -1, {}, {} };
		}
	}
	std::shared_ptr<const IGhToolPlatform> m_platform;
	std::wstring m_executable, m_workingDirectory, m_configurationDirectory;
	std::mutex m_mutex;
	SensitiveToken m_token;
	bool m_revoked{};
};

class AccountCandidate final {
public:
	AccountCandidate(std::wstring login, std::wstring tokenSource) :
		m_login(std::move(login)), m_tokenSource(std::move(tokenSource)) {}
	[[nodiscard]] const std::wstring& Login() const noexcept { return m_login; }
	[[nodiscard]] const std::wstring& TokenSource() const noexcept { return m_tokenSource; }
private:
	std::wstring m_login;
	std::wstring m_tokenSource;
};

std::optional<AccountCandidate> SelectAccount(const std::vector<std::uint8_t>& bytes,
	const std::wstring_view hostname, const std::optional<std::wstring_view> requestedLogin,
	bool& authenticationRequired)
{
	authenticationRequired = false;
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(Text(bytes));
	const auto* root = parsed.Succeeded() ? AsObject(*parsed.value) : nullptr;
	if (!root) return std::nullopt;
	const auto hosts = root->find(L"hosts");
	const auto* hostObject = hosts == root->end() ? nullptr : std::get_if<Json::Object>(&hosts->second.Value());
	if (!hostObject) return std::nullopt;
	const auto foundHost = hostObject->find(hostname);
	const auto* accounts = foundHost == hostObject->end() ? nullptr : std::get_if<Json::Array>(&foundHost->second.Value());
	if (!accounts || accounts->size() > 32) return std::nullopt;
	std::optional<AccountCandidate> selected;
	for (const auto& value : *accounts) {
		const auto* object = AsObject(value);
		if (!object) return std::nullopt;
		const auto* login = StringMember(*object, L"login");
		const auto* state = StringMember(*object, L"state");
		const auto* tokenSource = StringMember(*object, L"tokenSource");
		const auto* active = BoolMember(*object, L"active");
		const auto* host = StringMember(*object, L"host");
		if (!login || !state || !tokenSource || !active || !host || !IsLogin(*login)
			|| !EqualsInsensitive(*host, hostname)) return std::nullopt;
		const bool requested = requestedLogin && EqualsInsensitive(*login, *requestedLogin);
		if (!(requested || (!requestedLogin && *active))) continue;
		if (selected) return std::nullopt;
		if (*state != L"success") {
			authenticationRequired = true;
			continue;
		}
		if (tokenSource->empty() || tokenSource->size() > 64
			|| !std::ranges::all_of(*tokenSource, [](const wchar_t character) {
				return character >= 0x20 && character != 0x7f;
			})) return std::nullopt;
		selected = AccountCandidate{ *login, *tokenSource };
	}
	if (!selected && requestedLogin) {
		authenticationRequired = true;
	} else if (!selected && !requestedLogin && accounts->empty()) {
		authenticationRequired = true;
	}
	return selected;
}

std::optional<SensitiveToken> ParseToken(std::vector<std::uint8_t> bytes)
{
	if (bytes.empty()) return std::nullopt;
	const ByteBufferGuard guard(bytes);
	if (bytes.size() > 8194) return std::nullopt;
	std::size_t length = bytes.size();
	while (length > 0 && (bytes[length - 1] == '\r' || bytes[length - 1] == '\n')) --length;
	if (length == 0 || length > 8192 || !std::ranges::all_of(bytes.begin(), bytes.begin() + length,
		[](const unsigned char character) {
		return character >= 0x21 && character <= 0x7e;
	})) return std::nullopt;
	return SensitiveToken(std::wstring(bytes.begin(), bytes.begin() + length));
}

std::optional<std::wstring> ParseIdentity(const std::vector<std::uint8_t>& bytes)
{
	const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(Text(bytes));
	const auto* root = parsed.Succeeded() ? AsObject(*parsed.value) : nullptr;
	const auto* login = root ? StringMember(*root, L"login") : nullptr;
	return login && IsLogin(*login) ? std::optional<std::wstring>(*login) : std::nullopt;
}

bool SameAccount(const GhAccountIdentity& account, const std::wstring_view hostname,
	const std::optional<std::wstring>& requestedLogin) noexcept
{
	return EqualsInsensitive(account.Hostname(), hostname)
		&& (!requestedLogin || EqualsInsensitive(account.Login(), *requestedLogin));
}

} // namespace

GhAccountIdentity::GhAccountIdentity(std::wstring hostname, std::wstring login,
	std::wstring tokenSource, std::wstring configurationDirectory) :
	m_hostname(std::move(hostname)), m_login(std::move(login)), m_tokenSource(std::move(tokenSource)),
	m_configurationDirectory(std::move(configurationDirectory)) {}

GhConnectionCheckResult::GhConnectionCheckResult(const GhConnectionTerminal terminal,
	std::optional<GhAccountIdentity> identity, std::shared_ptr<IGhAccountCredential> credential) :
	m_terminal(terminal), m_identity(std::move(identity)), m_credential(std::move(credential)) {}

CWindowsGhConnectionPlatform::CWindowsGhConnectionPlatform(std::shared_ptr<const IGhToolPlatform> platform,
	std::wstring workingDirectory) : m_platform(std::move(platform)),
	m_workingDirectory(std::move(workingDirectory)) {}

GhConnectionCheckResult CWindowsGhConnectionPlatform::Check(const GhToolProbe& probe,
	const std::wstring_view configurationDirectory, const std::wstring_view hostname,
	const std::optional<std::wstring_view> requestedLogin, HANDLE stop)
try {
	if (!m_platform || !platform::IsAbsoluteWindowsPath(m_workingDirectory)
		|| !platform::IsAbsoluteWindowsPath(configurationDirectory) || !IsHostname(hostname)
		|| (requestedLogin && !IsLogin(*requestedLogin))) {
		return { GhConnectionTerminal::InvalidRequest, std::nullopt, {} };
	}
	switch (probe.Status()) {
	case GhToolAvailability::NotInstalled: return { GhConnectionTerminal::ToolUnavailable, std::nullopt, {} };
	case GhToolAvailability::UnsupportedVersion: return { GhConnectionTerminal::UnsupportedVersion, std::nullopt, {} };
	case GhToolAvailability::TimedOut: return { GhConnectionTerminal::TimedOut, std::nullopt, {} };
	case GhToolAvailability::Cancelled: return { GhConnectionTerminal::Cancelled, std::nullopt, {} };
	case GhToolAvailability::OutputLimitExceeded: return { GhConnectionTerminal::OutputLimitExceeded, std::nullopt, {} };
	case GhToolAvailability::ProbeFailed: return { GhConnectionTerminal::Failed, std::nullopt, {} };
	case GhToolAvailability::InvalidConfiguration: return { GhConnectionTerminal::InvalidRequest, std::nullopt, {} };
	case GhToolAvailability::Available: break;
	}
	if (!probe.Version() || !probe.Version()->Supported()
		|| !platform::IsAbsoluteWindowsPath(probe.ExecutablePath())) {
		return { GhConnectionTerminal::InvalidRequest, std::nullopt, {} };
	}
	const auto status = m_platform->Run(Invocation(probe.ExecutablePath(), m_workingDirectory,
		{ L"auth", L"status", L"--hostname", std::wstring(hostname), L"--json", L"hosts" },
		configurationDirectory, kStatusTimeoutMilliseconds, kStatusOutputBytes, kErrorBytes), stop);
	if (status.Status() != platform::process::EBoundedProcessStatus::Succeeded) {
		return { ProcessTerminal(status.Status()), std::nullopt, {} };
	}
	bool authenticationRequired{};
	const auto selected = SelectAccount(status.StandardOutput(), hostname, requestedLogin, authenticationRequired);
	if (!selected) return { authenticationRequired ? GhConnectionTerminal::AuthenticationRequired
		: GhConnectionTerminal::Failed, std::nullopt, {} };
	auto tokenResult = m_platform->Run(Invocation(probe.ExecutablePath(), m_workingDirectory,
		{ L"auth", L"token", L"--hostname", std::wstring(hostname), L"--user", selected->Login() },
		configurationDirectory, kTokenTimeoutMilliseconds, kTokenOutputBytes, kErrorBytes), stop);
	if (tokenResult.Status() != platform::process::EBoundedProcessStatus::Succeeded) {
		return { ProcessTerminal(tokenResult.Status()), std::nullopt, {} };
	}
	auto token = ParseToken(tokenResult.TakeStandardOutput());
	if (!token) return { GhConnectionTerminal::Failed, std::nullopt, {} };
	const auto identityResult = m_platform->Run(Invocation(probe.ExecutablePath(), m_workingDirectory,
		{ L"api", L"--hostname", std::wstring(hostname), L"--method", L"GET",
			L"--header", L"X-GitHub-Api-Version: 2022-11-28", L"user" },
		configurationDirectory, kIdentityTimeoutMilliseconds, kIdentityOutputBytes, kErrorBytes, token->Text()), stop);
	if (identityResult.Status() != platform::process::EBoundedProcessStatus::Succeeded) {
		return { ProcessTerminal(identityResult.Status()), std::nullopt, {} };
	}
	const auto verifiedLogin = ParseIdentity(identityResult.StandardOutput());
	if (!verifiedLogin || !EqualsInsensitive(*verifiedLogin, selected->Login())) {
		return { GhConnectionTerminal::IdentityMismatch, std::nullopt, {} };
	}
	auto credential = std::make_shared<CWindowsGhAccountCredential>(m_platform,
		probe.ExecutablePath(), m_workingDirectory, std::wstring(configurationDirectory), std::move(*token));
	return { GhConnectionTerminal::Succeeded,
		GhAccountIdentity(std::wstring(hostname), *verifiedLogin, selected->TokenSource(),
			std::wstring(configurationDirectory)), std::move(credential) };
} catch (...) {
	return { GhConnectionTerminal::Failed, std::nullopt, {} };
}

GhConnectionSnapshot::GhConnectionSnapshot(const GhConnectionState state, std::wstring profileId,
	std::optional<GhAccountIdentity> account, const std::int64_t accountGeneration) : m_state(state),
	m_profileId(std::move(profileId)), m_account(std::move(account)),
	m_accountGeneration(accountGeneration) {}

GhConnectionAttempt::GhConnectionAttempt(const CGhConnectionLifecycle& owner, const std::uint64_t operation,
	const std::uint64_t epoch, std::wstring hostname, std::optional<std::wstring> requestedLogin) :
	m_owner(owner), m_operation(operation), m_epoch(epoch), m_hostname(std::move(hostname)),
	m_requestedLogin(std::move(requestedLogin)) {}

GhConnectionOperationResult::GhConnectionOperationResult(const GhConnectionTerminal terminal,
	GhConnectionSnapshot snapshot) : m_terminal(terminal), m_snapshot(std::move(snapshot)) {}

GhAuthenticatedAccount::GhAuthenticatedAccount(GhAccountIdentity identity,
	const std::int64_t accountGeneration, std::shared_ptr<IGhAccountCredential> credential) :
	m_identity(std::move(identity)), m_accountGeneration(accountGeneration),
	m_credential(std::move(credential)) {}

CGhConnectionLifecycle::CGhConnectionLifecycle(std::shared_ptr<IGhConnectionPlatform> platform,
	CSenpToolGrants& grants, std::wstring profileId, std::wstring configurationDirectory) :
	m_platform(std::move(platform)), m_grants(grants), m_profileId(std::move(profileId)),
	m_configurationDirectory(std::move(configurationDirectory)) {}

CGhConnectionLifecycle::~CGhConnectionLifecycle()
{
	Close();
}

std::optional<GhConnectionAttempt> CGhConnectionLifecycle::Begin(
	std::wstring hostname, std::optional<std::wstring> requestedLogin)
{
	std::scoped_lock lock(m_mutex);
	if (m_closed || !m_platform || m_profileId.empty()
		|| !platform::IsAbsoluteWindowsPath(m_configurationDirectory)
		|| !IsHostname(hostname) || (requestedLogin && !IsLogin(*requestedLogin))
		|| m_activeOperation != 0 || m_nextOperation == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	m_activeOperation = m_nextOperation++;
	m_state = GhConnectionState::Checking;
	return GhConnectionAttempt(*this, m_activeOperation, m_epoch,
		std::move(hostname), std::move(requestedLogin));
}

GhConnectionOperationResult CGhConnectionLifecycle::Complete(
	GhConnectionAttempt attempt, const GhToolProbe& probe, HANDLE stop)
{
	{
		std::scoped_lock lock(m_mutex);
		if (m_closed) return { GhConnectionTerminal::Closed, SnapshotLocked() };
		if (&attempt.m_owner.get() != this || attempt.m_operation == 0
			|| attempt.m_operation != m_activeOperation || attempt.m_epoch != m_epoch) {
			return { GhConnectionTerminal::Stale, SnapshotLocked() };
		}
	}
	GhConnectionCheckResult checked(GhConnectionTerminal::Failed, std::nullopt, {});
	try {
		checked = m_platform->Check(probe, m_configurationDirectory, attempt.m_hostname,
			attempt.m_requestedLogin ? std::optional<std::wstring_view>(*attempt.m_requestedLogin) : std::nullopt, stop);
	} catch (...) {
		checked = { GhConnectionTerminal::Failed, std::nullopt, {} };
	}
	bool revoke{};
	std::shared_ptr<IGhAccountCredential> revokedCredential;
	GhConnectionTerminal terminal = checked.Terminal();
	GhConnectionSnapshot snapshot(GhConnectionState::Unavailable, m_profileId, std::nullopt, 0);
	{
		std::scoped_lock lock(m_mutex);
		if (m_closed || &attempt.m_owner.get() != this || attempt.m_operation != m_activeOperation
			|| attempt.m_epoch != m_epoch) {
			return { m_closed ? GhConnectionTerminal::Closed : GhConnectionTerminal::Stale, SnapshotLocked() };
		}
		m_activeOperation = 0;
		if (terminal == GhConnectionTerminal::Succeeded) {
			if (!checked.Identity() || !checked.Credential()
				|| !EqualsInsensitive(checked.Identity()->Hostname(), attempt.m_hostname)
				|| (attempt.m_requestedLogin
					&& !EqualsInsensitive(checked.Identity()->Login(), *attempt.m_requestedLogin))
				|| m_nextAccountGeneration <= 0) {
				terminal = GhConnectionTerminal::IdentityMismatch;
			} else {
				revoke = m_account.has_value();
				revokedCredential = m_credential;
				m_account = checked.Identity();
				m_credential = checked.Credential();
				m_accountGeneration = m_nextAccountGeneration;
				m_nextAccountGeneration = m_nextAccountGeneration == std::numeric_limits<std::int64_t>::max()
					? 0 : m_nextAccountGeneration + 1;
				m_state = GhConnectionState::Connected;
			}
		}
		if (terminal != GhConnectionTerminal::Succeeded) {
			const bool currentRejected = terminal == GhConnectionTerminal::AuthenticationRequired
				&& m_account && SameAccount(*m_account, attempt.m_hostname, attempt.m_requestedLogin);
			if (currentRejected) {
				revoke = true;
				revokedCredential = m_credential;
				m_account.reset();
				m_credential.reset();
				m_accountGeneration = 0;
				m_state = GhConnectionState::ReauthenticationRequired;
			} else if (m_account) {
				m_state = GhConnectionState::Connected;
			} else if (terminal == GhConnectionTerminal::AuthenticationRequired) {
				m_state = GhConnectionState::Disconnected;
			} else {
				m_state = GhConnectionState::Unavailable;
			}
		}
		snapshot = SnapshotLocked();
	}
	if (revokedCredential) revokedCredential->Revoke();
	if (revoke) m_grants.RevokeProfile(m_profileId);
	return { terminal, std::move(snapshot) };
}

GhConnectionSnapshot CGhConnectionLifecycle::SnapshotLocked() const
{
	return { m_state, m_profileId, m_account, m_accountGeneration };
}

GhConnectionSnapshot CGhConnectionLifecycle::Snapshot()
{
	std::scoped_lock lock(m_mutex);
	return SnapshotLocked();
}

std::optional<GhAuthenticatedAccount> CGhConnectionLifecycle::Acquire(
	const std::int64_t expectedAccountGeneration)
{
	std::scoped_lock lock(m_mutex);
	if (m_closed || (m_state != GhConnectionState::Connected && m_state != GhConnectionState::Checking)
		|| !m_account || !m_credential
		|| expectedAccountGeneration <= 0 || expectedAccountGeneration != m_accountGeneration) return std::nullopt;
	return GhAuthenticatedAccount(*m_account, m_accountGeneration, m_credential);
}

void CGhConnectionLifecycle::Disconnect() noexcept
{
	bool revoke{};
	std::shared_ptr<IGhAccountCredential> revokedCredential;
	{
		std::scoped_lock lock(m_mutex);
		if (m_closed) return;
		if (m_epoch != std::numeric_limits<std::uint64_t>::max()) ++m_epoch;
		m_activeOperation = 0;
		revoke = m_account.has_value();
		revokedCredential = m_credential;
		m_account.reset();
		m_credential.reset();
		m_accountGeneration = 0;
		m_state = GhConnectionState::Disconnected;
	}
	if (revokedCredential) revokedCredential->Revoke();
	if (revoke) m_grants.RevokeProfile(m_profileId);
}

void CGhConnectionLifecycle::Close() noexcept
{
	bool revoke{};
	std::shared_ptr<IGhAccountCredential> revokedCredential;
	{
		std::scoped_lock lock(m_mutex);
		if (m_closed) return;
		m_closed = true;
		m_activeOperation = 0;
		revoke = m_account.has_value();
		revokedCredential = m_credential;
		m_account.reset();
		m_credential.reset();
		m_accountGeneration = 0;
		m_state = GhConnectionState::Unavailable;
	}
	if (revokedCredential) revokedCredential->Revoke();
	if (revoke) m_grants.RevokeProfile(m_profileId);
}

} // namespace senp::github
