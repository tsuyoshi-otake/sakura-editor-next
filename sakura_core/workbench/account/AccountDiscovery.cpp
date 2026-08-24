/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/account/AccountDiscovery.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <utility>

namespace workbench::account {
namespace {

using JsonArray = picojson::array;
using JsonObject = picojson::object;
using JsonValue = picojson::value;

constexpr std::size_t kMaximumRequestPathLength = 32768;
constexpr std::size_t kMaximumTimeoutMilliseconds = 120000;
constexpr std::size_t kMaximumOutputBytes = 1024u * 1024u;
constexpr std::size_t kMaximumNestingDepth = 64;
constexpr std::size_t kMaximumHosts = 1024;
constexpr std::size_t kMaximumAccounts = 4096;
constexpr std::size_t kMaximumStringBytes = 16u * 1024u;

bool IsValidUtf8(std::string_view value, const bool allowEmpty, const std::size_t maximum) noexcept
{
	if ((!allowEmpty && value.empty()) || value.size() > maximum
		|| value.find('\0') != std::string_view::npos) {
		return false;
	}

	const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
	for (std::size_t index = 0; index < value.size();) {
		const auto first = bytes[index];
		if (first <= 0x7f) {
			++index;
			continue;
		}
		std::size_t tails = 0;
		std::uint32_t point = 0;
		if ((first & 0xe0) == 0xc0) {
			tails = 1;
			point = first & 0x1f;
		} else if ((first & 0xf0) == 0xe0) {
			tails = 2;
			point = first & 0x0f;
		} else if ((first & 0xf8) == 0xf0) {
			tails = 3;
			point = first & 0x07;
		} else {
			return false;
		}
		if (index + tails >= value.size()) return false;
		for (std::size_t tail = 1; tail <= tails; ++tail) {
			const auto byte = bytes[index + tail];
			if ((byte & 0xc0) != 0x80) return false;
			point = (point << 6) | (byte & 0x3f);
		}
		const auto minimum = tails == 1 ? 0x80u : tails == 2 ? 0x800u : 0x10000u;
		if (point < minimum || point > 0x10ffff
			|| (point >= 0xd800 && point <= 0xdfff)) return false;
		index += tails + 1;
	}
	return true;
}

bool HasBoundedJsonNesting(std::string_view payload, const std::size_t maximumDepth) noexcept
{
	std::size_t depth = 0;
	bool inString = false;
	bool escaped = false;
	for (const char character : payload) {
		if (inString) {
			if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				inString = false;
			}
			continue;
		}
		if (character == '"') {
			inString = true;
		} else if (character == '{' || character == '[') {
			if (++depth > maximumDepth) return false;
		} else if (character == '}' || character == ']') {
			if (depth == 0) return false;
			--depth;
		}
	}
	return !inString && !escaped && depth == 0;
}

const JsonValue* Find(const JsonObject& object, std::string_view key) noexcept
{
	const auto found = object.find(std::string(key));
	return found == object.end() ? nullptr : &found->second;
}

bool DecodeUtf8(std::string_view value, std::wstring& output, const std::size_t maximum)
{
	if (!IsValidUtf8(value, false, maximum) || value.size() > static_cast<std::size_t>(INT_MAX)) return false;
	const int inputLength = static_cast<int>(value.size());
	const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), inputLength, nullptr, 0);
	if (required <= 0) return false;
	std::wstring decoded(static_cast<std::size_t>(required), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), inputLength, decoded.data(), required) != required) return false;
	output = std::move(decoded);
	return true;
}

bool IsDisplayText(const std::wstring& value, const bool allowEmpty) noexcept
{
	if (!allowEmpty && value.empty()) return false;
	return std::all_of(value.begin(), value.end(), [](const wchar_t character) {
		return character >= L' ' && character != 0x7f;
	});
}

bool ReadText(const JsonObject& object, std::string_view key, std::wstring& output,
	const std::size_t maximum, const bool allowEmpty = false)
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<std::string>()) return false;
	if (!DecodeUtf8(field->get<std::string>(), output, maximum) || !IsDisplayText(output, allowEmpty)) return false;
	return true;
}

bool ReadBoolean(const JsonObject& object, std::string_view key, bool& output) noexcept
{
	const auto* field = Find(object, key);
	if (field == nullptr || !field->is<bool>()) return false;
	output = field->get<bool>();
	return true;
}

bool ReadAccountState(const JsonObject& object, EGitHubAccountState& output,
	const std::size_t maximum)
{
	const auto* field = Find(object, "state");
	if (field == nullptr) {
		output = EGitHubAccountState::Unknown;
		return true;
	}
	if (field->is<std::string>() && field->get<std::string>().empty()) {
		output = EGitHubAccountState::Unknown;
		return true;
	}
	std::wstring state;
	if (!ReadText(object, "state", state, maximum, true)) return false;
	if (state == L"success") {
		output = EGitHubAccountState::Success;
	} else if (state == L"failure") {
		output = EGitHubAccountState::Failure;
	} else {
		output = EGitHubAccountState::Unknown;
	}
	return true;
}

bool ParseHostAccount(const std::string& hostKey, const JsonObject& object,
	const AccountDiscoveryLimits& limits, GitHubCliAccount& account)
{
	std::wstring host;
	const auto* hostField = Find(object, "host");
	if (hostField != nullptr) {
		if (!ReadText(object, "host", host, limits.maximumStringBytes)) return false;
	} else if (!DecodeUtf8(hostKey, host, limits.maximumStringBytes) || !IsDisplayText(host, false)) {
		return false;
	}

	std::wstring login;
	if (!ReadText(object, "login", login, limits.maximumStringBytes)) {
		// Older gh builds used `user` in this nested record. Accept it as a
		// compatibility spelling while retaining the same display-only shape.
		if (!ReadText(object, "user", login, limits.maximumStringBytes)) return false;
	}
	std::wstring protocol;
	if (!ReadText(object, "gitProtocol", protocol, limits.maximumStringBytes)) return false;
	bool active = false;
	if (!ReadBoolean(object, "active", active)) return false;
	EGitHubAccountState state = EGitHubAccountState::Unknown;
	if (!ReadAccountState(object, state, limits.maximumStringBytes)) return false;

	account.host = std::move(host);
	account.login = std::move(login);
	account.gitProtocol = std::move(protocol);
	account.active = active;
	account.state = state;
	return true;
}

bool IsStopSignalled(HANDLE stop) noexcept
{
	return stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

std::string GitOutput(const scm::GitExecutionResult& result)
{
	if (result.standardOutput.empty()) return {};
	return std::string(reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size());
}

bool DecodeGitValue(const scm::GitExecutionResult& result, const AccountDiscoveryLimits& limits,
	std::wstring& value)
{
	if (!result.Succeeded()) return false;
	const std::size_t maximumBytes = std::min(limits.maximumStringBytes + 2u, kMaximumStringBytes + 2u);
	std::string text = GitOutput(result);
	if (text.size() > maximumBytes) return false;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
	if (text.empty()) {
		value.clear();
		return true;
	}
	if (!DecodeUtf8(text, value, limits.maximumStringBytes) || !IsDisplayText(value, false)) return false;
	return true;
}

struct GitSourceResult final {
	EAccountSourceState state{EAccountSourceState::Unavailable};
	std::optional<AccountGitIdentity> identity;
	bool cancelled{};
};

struct GhSourceResult final {
	EAccountSourceState state{EAccountSourceState::Unavailable};
	std::vector<GitHubCliAccount> accounts;
	bool cancelled{};
};

bool IsGitCancellation(const scm::GitExecutionResult& result) noexcept
{
	return result.status == scm::EGitExecutionStatus::Cancelled;
}

GitSourceResult ReadGitIdentity(const AccountDiscoveryRequest& request,
	const AccountDiscoveryRunners& runners, HANDLE stop)
{
	GitSourceResult source;
	const auto runGit = runners.runGit ? runners.runGit
		: GitAccountRunner([](const scm::GitExecutionRequest& gitRequest, HANDLE gitStop) {
			return scm::RunGit(gitRequest, gitStop);
		});

	std::array<scm::GitExecutionResult, 2> results;
	std::array<std::wstring, 2> values;
	const std::array<std::wstring_view, 2> keys{ L"user.name", L"user.email" };
	std::array<bool, 2> decoded{};
	for (std::size_t index = 0; index < keys.size(); ++index) {
		if (IsStopSignalled(stop)) {
			source.cancelled = true;
			return source;
		}
		scm::GitExecutionRequest gitRequest;
		gitRequest.workingDirectory = request.workingDirectory;
		gitRequest.arguments = { L"config", L"--get", std::wstring(keys[index]) };
		gitRequest.policy = scm::EGitRequestPolicy::PassiveRepositoryRead;
		gitRequest.timeoutMilliseconds = request.timeoutMilliseconds;
		gitRequest.maximumOutputBytes = request.maximumOutputBytes;
		try {
			results[index] = runGit(gitRequest, stop);
		} catch (...) {
			results[index].status = scm::EGitExecutionStatus::LaunchFailed;
		}
		if (IsGitCancellation(results[index])) {
			source.cancelled = true;
			return source;
		}
		if (IsStopSignalled(stop)) {
			source.cancelled = true;
			return source;
		}
		decoded[index] = DecodeGitValue(results[index], request.limits, values[index]);
	}

	const bool namePresent = decoded[0] && !values[0].empty();
	const bool emailPresent = decoded[1] && !values[1].empty();
	if (namePresent || emailPresent) {
		source.identity = AccountGitIdentity{ values[0], values[1] };
		source.state = namePresent && emailPresent ? EAccountSourceState::Ready : EAccountSourceState::Partial;
		return source;
	}
	const auto isConfigMissing = [](const scm::GitExecutionResult& result) {
		return result.status == scm::EGitExecutionStatus::Succeeded
			|| result.status == scm::EGitExecutionStatus::Failed;
	};
	const auto hasNoConfiguredValue = [&decoded, &results, &values](const std::size_t index) {
		return (decoded[index] && values[index].empty())
			|| (results[index].status == scm::EGitExecutionStatus::Failed
				&& results[index].standardOutput.empty());
	};
	if (hasNoConfiguredValue(0) && hasNoConfiguredValue(1)
		&& isConfigMissing(results[0]) && isConfigMissing(results[1])) {
		source.state = EAccountSourceState::Unconfigured;
	} else if (results[0].status == scm::EGitExecutionStatus::GitUnavailable
		|| results[1].status == scm::EGitExecutionStatus::GitUnavailable) {
		source.state = EAccountSourceState::Unavailable;
	} else if (!decoded[0] || !decoded[1]) {
		source.state = EAccountSourceState::Failed;
	}
	return source;
}

GhSourceResult ReadGitHubAccounts(const AccountDiscoveryRequest& request,
	const AccountDiscoveryRunners& runners, HANDLE stop)
{
	GhSourceResult source;
	const auto runGh = runners.runGhAuthStatus ? runners.runGhAuthStatus
		: GhAuthStatusRunner([](const GhAuthStatusRequest& ghRequest, HANDLE ghStop) {
			return RunGhAuthStatus(ghRequest, ghStop);
		});
	GhAuthStatusRequest ghRequest;
	ghRequest.workingDirectory = request.workingDirectory;
	ghRequest.timeoutMilliseconds = request.timeoutMilliseconds;
	ghRequest.maximumOutputBytes = request.maximumOutputBytes;
	ghRequest.limits = request.limits;
	GhAuthStatusResult result;
	try {
		result = runGh(ghRequest, stop);
	} catch (...) {
		result.status = EAccountCommandStatus::LaunchFailed;
	}
	if (result.status == EAccountCommandStatus::Cancelled) {
		source.cancelled = true;
		return source;
	}
	if (IsStopSignalled(stop)) {
		source.cancelled = true;
		return source;
	}
	if (result.parseStatus == EGitHubAuthParseStatus::Invalid
		&& result.accounts.empty()) {
		source.state = result.status == EAccountCommandStatus::Unavailable
			|| result.status == EAccountCommandStatus::LaunchFailed
			? EAccountSourceState::Unavailable : EAccountSourceState::Failed;
		return source;
	}
	source.accounts = result.accounts;
	if (result.parseStatus == EGitHubAuthParseStatus::Ready) {
		if (result.status == EAccountCommandStatus::Succeeded
			|| (result.status == EAccountCommandStatus::Failed && source.accounts.empty())) {
			// gh uses a non-zero exit code for an unauthenticated or invalid-token
			// status even when it returns a valid hosts object. An empty, valid
			// object is therefore a trustworthy Ready/no-accounts result.
			source.state = EAccountSourceState::Ready;
		} else if (result.status == EAccountCommandStatus::Failed) {
			// Keep records from a non-zero status, but make the degraded command
			// outcome visible to the aggregate/source state.
			source.state = EAccountSourceState::Partial;
		} else {
			source.state = EAccountSourceState::Failed;
		}
	} else if (result.parseStatus == EGitHubAuthParseStatus::Partial) {
		source.state = EAccountSourceState::Partial;
	} else {
		source.state = EAccountSourceState::Failed;
	}
	return source;
}

} // namespace

bool AccountDiscoveryLimits::IsValid() const noexcept
{
	return maximumJsonBytes != 0 && maximumJsonBytes <= kMaximumOutputBytes
		&& maximumHosts != 0 && maximumHosts <= kMaximumHosts
		&& maximumAccounts != 0 && maximumAccounts <= kMaximumAccounts
		&& maximumStringBytes != 0 && maximumStringBytes <= kMaximumStringBytes
		&& maximumNestingDepth != 0 && maximumNestingDepth <= kMaximumNestingDepth;
}

bool AccountDiscoveryRequest::IsValid() const noexcept
{
	return !workingDirectory.empty() && workingDirectory.size() <= kMaximumRequestPathLength
		&& timeoutMilliseconds != 0 && timeoutMilliseconds <= kMaximumTimeoutMilliseconds
		&& maximumOutputBytes != 0 && maximumOutputBytes <= kMaximumOutputBytes
		&& limits.IsValid();
}

bool GhAuthStatusRequest::IsValid() const noexcept
{
	return !workingDirectory.empty() && workingDirectory.size() <= kMaximumRequestPathLength
		&& timeoutMilliseconds != 0 && timeoutMilliseconds <= kMaximumTimeoutMilliseconds
		&& maximumOutputBytes != 0 && maximumOutputBytes <= kMaximumOutputBytes
		&& limits.IsValid();
}

GitHubAuthParseResult ParseGitHubAuthStatus(std::string_view json, AccountDiscoveryLimits limits)
{
	GitHubAuthParseResult result;
	try {
		if (!limits.IsValid() || json.size() > limits.maximumJsonBytes) {
			result.status = EGitHubAuthParseStatus::Oversized;
			return result;
		}
		if (!IsValidUtf8(json, false, limits.maximumJsonBytes)
			|| !HasBoundedJsonNesting(json, limits.maximumNestingDepth)) return result;

		JsonValue rootValue;
		std::string parseError;
		auto position = picojson::parse(rootValue, json.begin(), json.end(), &parseError);
		while (position != json.end()
			&& (*position == ' ' || *position == '\t' || *position == '\r' || *position == '\n')) ++position;
		if (!parseError.empty() || position != json.end() || !rootValue.is<JsonObject>()) return result;
		const auto* hostsValue = Find(rootValue.get<JsonObject>(), "hosts");
		if (hostsValue == nullptr || !hostsValue->is<JsonObject>()) return result;
		const auto& hosts = hostsValue->get<JsonObject>();
		if (hosts.size() > limits.maximumHosts) {
			result.status = EGitHubAuthParseStatus::Oversized;
			return result;
		}

		bool malformed = false;
		std::size_t accountCount = 0;
		std::vector<const JsonObject::value_type*> orderedHosts;
		orderedHosts.reserve(hosts.size());
		for (const auto& entry : hosts) orderedHosts.push_back(&entry);
		std::ranges::sort(orderedHosts, [](const auto* left, const auto* right) {
			const bool leftIsGitHub = left->first == "github.com";
			const bool rightIsGitHub = right->first == "github.com";
			if (leftIsGitHub != rightIsGitHub) return leftIsGitHub;
			return left->first < right->first;
		});
		for (const auto* hostEntry : orderedHosts) {
			const auto& [hostKey, hostValue] = *hostEntry;
			if (!hostValue.is<JsonArray>()) {
				malformed = true;
				continue;
			}
			const auto& records = hostValue.get<JsonArray>();
			if (records.size() > limits.maximumAccounts - std::min(accountCount, limits.maximumAccounts)) {
				result.status = EGitHubAuthParseStatus::Oversized;
				result.accounts.clear();
				return result;
			}
			for (const auto& record : records) {
				++accountCount;
				GitHubCliAccount account;
				if (!record.is<JsonObject>()
					|| !ParseHostAccount(hostKey, record.get<JsonObject>(), limits, account)) {
					malformed = true;
					continue;
				}
				result.accounts.emplace_back(std::move(account));
			}
		}
		result.status = malformed ? EGitHubAuthParseStatus::Partial : EGitHubAuthParseStatus::Ready;
		return result;
	} catch (...) {
		result.status = EGitHubAuthParseStatus::Invalid;
		result.accounts.clear();
		return result;
	}
}

std::vector<std::wstring> BuildGhAuthStatusArguments()
{
	return { L"auth", L"status", L"--json", L"hosts" };
}

AccountDiscoverySnapshot DiscoverAccounts(const AccountDiscoveryRequest& request,
	const AccountDiscoveryRunners& runners, HANDLE stop)
{
	AccountDiscoverySnapshot snapshot;
	if (!request.IsValid()) {
		snapshot.state = EAccountDiscoveryState::Unavailable;
		return snapshot;
	}
	if (IsStopSignalled(stop)) {
		snapshot.state = EAccountDiscoveryState::Stopped;
		return snapshot;
	}

	const auto git = ReadGitIdentity(request, runners, stop);
	if (git.cancelled || IsStopSignalled(stop)) {
		snapshot.state = EAccountDiscoveryState::Stopped;
		snapshot.gitIdentity = git.identity;
		return snapshot;
	}
	const auto gh = ReadGitHubAccounts(request, runners, stop);
	if (gh.cancelled || IsStopSignalled(stop)) {
		snapshot.state = EAccountDiscoveryState::Stopped;
		snapshot.gitIdentity = git.identity;
		snapshot.githubAccounts = gh.accounts;
		return snapshot;
	}
	snapshot.gitIdentity = git.identity;
	snapshot.githubAccounts = gh.accounts;
	snapshot.gitState = git.state;
	snapshot.githubState = gh.state;
	if (git.state == EAccountSourceState::Ready && gh.state == EAccountSourceState::Ready) {
		snapshot.state = EAccountDiscoveryState::Ready;
	} else if (snapshot.gitIdentity || !snapshot.githubAccounts.empty()
		|| git.state == EAccountSourceState::Partial || gh.state == EAccountSourceState::Partial
		|| git.state == EAccountSourceState::Failed || gh.state == EAccountSourceState::Failed
		|| git.state == EAccountSourceState::Ready || gh.state == EAccountSourceState::Ready) {
		snapshot.state = EAccountDiscoveryState::Partial;
	} else {
		snapshot.state = EAccountDiscoveryState::Unavailable;
	}
	return snapshot;
}

AccountDiscoveryService::AccountDiscoveryService(AccountDiscoveryFunction discover,
	AccountDiscoveryRequest defaults)
	: m_discover(discover ? std::move(discover)
		: AccountDiscoveryFunction([](const AccountDiscoveryRequest& request, HANDLE stop) {
			return DiscoverAccounts(request, {}, stop);
		}))
	, m_defaults(std::move(defaults))
{
	m_snapshot.state = EAccountDiscoveryState::Stopped;
	m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AccountDiscoveryService::~AccountDiscoveryService()
{
	Stop();
	if (m_stopEvent != nullptr) {
		::CloseHandle(m_stopEvent);
		m_stopEvent = nullptr;
	}
}

std::uint64_t AccountDiscoveryService::NextRevisionLocked() noexcept
{
	if (m_revision != (std::numeric_limits<std::uint64_t>::max)()) ++m_revision;
	return m_revision;
}

void AccountDiscoveryService::PublishWorkerResult(AccountDiscoverySnapshot result) noexcept
{
	std::lock_guard lock(m_mutex);
	m_workerRunning = false;
	if (!m_stopping) {
		if (result.state == EAccountDiscoveryState::Loading
			|| result.state == EAccountDiscoveryState::Stopped) {
			result.state = EAccountDiscoveryState::Unavailable;
		}
		result.revision = NextRevisionLocked();
		m_snapshot = std::move(result);
	}
	m_condition.notify_all();
}

EAccountRefreshResult AccountDiscoveryService::RequestRefresh(std::wstring workingDirectory)
{
	std::unique_lock lock(m_mutex);
	if (m_stopping || m_stopJoining) return EAccountRefreshResult::RejectedStopped;
	if (m_workerRunning) return EAccountRefreshResult::AlreadyInFlight;
	if (workingDirectory.empty()) {
		m_snapshot = {};
		m_snapshot.state = EAccountDiscoveryState::Unavailable;
		m_snapshot.revision = NextRevisionLocked();
		return EAccountRefreshResult::InvalidRequest;
	}
	if (m_worker.joinable()) {
		m_worker.join();
		m_workerId = {};
	}
	if (m_stopEvent == nullptr || !::ResetEvent(m_stopEvent)) {
		m_snapshot = {};
		m_snapshot.state = EAccountDiscoveryState::Unavailable;
		m_snapshot.revision = NextRevisionLocked();
		return EAccountRefreshResult::Unavailable;
	}
	AccountDiscoveryRequest request = m_defaults;
	request.workingDirectory = std::move(workingDirectory);
	if (!request.IsValid()) {
		m_snapshot = {};
		m_snapshot.state = EAccountDiscoveryState::Unavailable;
		m_snapshot.revision = NextRevisionLocked();
		return EAccountRefreshResult::InvalidRequest;
	}
	m_snapshot = {};
	m_snapshot.state = EAccountDiscoveryState::Loading;
	m_snapshot.gitState = EAccountSourceState::Loading;
	m_snapshot.githubState = EAccountSourceState::Loading;
	m_snapshot.revision = NextRevisionLocked();
	m_workerRunning = true;
	try {
		m_worker = std::thread([this, request = std::move(request), stop = m_stopEvent]() mutable {
			AccountDiscoverySnapshot result;
			try {
				result = m_discover(request, stop);
			} catch (...) {
				result.state = EAccountDiscoveryState::Unavailable;
			}
			PublishWorkerResult(std::move(result));
		});
		m_workerId = m_worker.get_id();
	} catch (...) {
		m_workerRunning = false;
		m_workerId = {};
		m_snapshot = {};
		m_snapshot.state = EAccountDiscoveryState::Unavailable;
		m_snapshot.revision = NextRevisionLocked();
		return EAccountRefreshResult::Unavailable;
	}
	return EAccountRefreshResult::Started;
}

AccountDiscoverySnapshot AccountDiscoveryService::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

void AccountDiscoveryService::Stop() noexcept
{
	std::thread worker;
	{
		std::unique_lock lock(m_mutex);
		if (m_stopJoining) {
			if (m_workerRunning && m_workerId == std::this_thread::get_id()) {
				// An outer Stop may already be joining this worker. Waiting from
				// the worker itself would deadlock that join; the outer call has
				// already published Stopped and signalled the event.
				return;
			}
			m_condition.wait(lock, [this] { return !m_stopJoining; });
			return;
		}
		m_stopJoining = true;
		m_stopping = true;
		if (m_stopEvent != nullptr) (void)::SetEvent(m_stopEvent);
		m_snapshot = {};
		m_snapshot.state = EAccountDiscoveryState::Stopped;
		m_snapshot.revision = NextRevisionLocked();
		if (m_workerRunning && m_workerId == std::this_thread::get_id()) {
			// A discovery callback owns no service listener, but an injected test
			// callback may still request Stop reentrantly. A thread cannot join
			// itself; leave the joinable worker for its next external Stop/destructor.
			m_stopJoining = false;
			m_condition.notify_all();
			return;
		}
		worker = std::move(m_worker);
	}
	if (worker.joinable()) worker.join();
	{
		std::lock_guard lock(m_mutex);
		m_workerRunning = false;
		m_workerId = {};
		m_stopJoining = false;
	}
	m_condition.notify_all();
}

} // namespace workbench::account
