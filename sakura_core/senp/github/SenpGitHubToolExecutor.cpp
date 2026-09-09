/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <utility>

namespace senp::github {
namespace {

using platform::controlipc::ControlSenpRpcResponse;
using platform::controlipc::EControlSenpAccountState;
using platform::controlipc::EControlSenpRpcStatus;
using platform::controlipc::SenpToolExecutionScope;

//! The wire vocabulary is deliberately its own enum, so the mapping is written
//! out rather than assumed from the two enumerations happening to agree today.
EControlSenpAccountState ToAccountState(const GhConnectionState state) noexcept
{
	switch (state) {
	case GhConnectionState::Checking: return EControlSenpAccountState::Checking;
	case GhConnectionState::Disconnected: return EControlSenpAccountState::Disconnected;
	case GhConnectionState::Connected: return EControlSenpAccountState::Connected;
	case GhConnectionState::ReauthenticationRequired:
		return EControlSenpAccountState::ReauthenticationRequired;
	case GhConnectionState::Unavailable: return EControlSenpAccountState::Unavailable;
	default: return EControlSenpAccountState::Unknown;
	}
}

constexpr std::uint32_t kWorkerWakeMilliseconds = 50;
constexpr std::uint32_t kIdleWaitSliceMilliseconds = 10;

std::uint64_t NowMilliseconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t NowUnixSeconds() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

//! Strict narrowing for the identifiers the text resource store accepts. A value
//! that is not representable is refused rather than silently replaced.
std::optional<std::string> Narrow(const std::wstring& value)
{
	if (value.empty()) return std::string();
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return std::nullopt;
	const auto length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return std::nullopt;
	std::string narrowed(static_cast<std::size_t>(length), 0);
	if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()),
		narrowed.data(), length, nullptr, nullptr) != length) return std::nullopt;
	return narrowed;
}

//! The store identifies a package by its 64 hexadecimal archive digest. A digest
//! of any other shape means the owner was not resolved from installed state.
std::optional<std::string> PackageDigest(const std::wstring& value)
{
	auto narrowed = Narrow(value);
	if (!narrowed || narrowed->size() != 64) return std::nullopt;
	for (auto& character : *narrowed) {
		if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
		const auto digit = character >= '0' && character <= '9';
		const auto hex = character >= 'a' && character <= 'f';
		if (!digit && !hex) return std::nullopt;
	}
	return narrowed;
}

bool IsDecimalId(const std::wstring& value) noexcept
{
	if (value.empty() || value.size() > 19 || (value.size() > 1 && value.front() == L'0')) return false;
	return std::ranges::all_of(value, [](const wchar_t character) { return character >= L'0' && character <= L'9'; });
}

//! Closed repository-read shapes. The editor names one of these; it never names a
//! path segment, host, owner or repository of its own.
std::optional<std::vector<std::wstring>> ResourceSegments(const std::wstring& shape, const std::wstring& id)
{
	if (shape == L"issues") return std::vector<std::wstring>{ L"issues" };
	if (shape == L"pulls") return std::vector<std::wstring>{ L"pulls" };
	if (shape == L"runs") return std::vector<std::wstring>{ L"actions", L"runs" };
	if (shape == L"issue") return std::vector<std::wstring>{ L"issues", id };
	if (shape == L"pull") return std::vector<std::wstring>{ L"pulls", id };
	if (shape == L"run") return std::vector<std::wstring>{ L"actions", L"runs", id };
	if (shape == L"runJobs") return std::vector<std::wstring>{ L"actions", L"runs", id, L"jobs" };
	if (shape == L"job") return std::vector<std::wstring>{ L"actions", L"jobs", id };
	return std::nullopt;
}

bool ShapeNeedsId(const std::wstring& shape) noexcept
{
	return shape == L"issue" || shape == L"pull" || shape == L"run"
		|| shape == L"runJobs" || shape == L"job";
}

//! Query names the editor may forward. They are a subset of the tool policy's own
//! closed set; the policy still validates every value independently.
bool IsForwardedQuery(const std::wstring& name) noexcept
{
	static constexpr std::array<std::wstring_view, 7> forwarded{
		L"actor", L"branch", L"event", L"page", L"per_page", L"state", L"status",
	};
	return std::ranges::find(forwarded, name) != forwarded.end();
}

effect::CompletionStatus ToCompletionStatus(const GhRepositoryResponseStatus status) noexcept
{
	switch (status) {
	case GhRepositoryResponseStatus::Succeeded:
	case GhRepositoryResponseStatus::NotModified:
		return effect::CompletionStatus::Succeeded;
	case GhRepositoryResponseStatus::Cancelled:
		return effect::CompletionStatus::Cancelled;
	case GhRepositoryResponseStatus::TimedOut:
		return effect::CompletionStatus::TimedOut;
	case GhRepositoryResponseStatus::ToolUnavailable:
	case GhRepositoryResponseStatus::UnsupportedVersion:
	case GhRepositoryResponseStatus::Unauthorized:
		return effect::CompletionStatus::HostUnavailable;
	default:
		return effect::CompletionStatus::Failed;
	}
}

std::wstring Describe(const GhRepositoryResponseStatus status)
{
	switch (status) {
	case GhRepositoryResponseStatus::RateLimited: return L"rate-limited";
	case GhRepositoryResponseStatus::Forbidden: return L"forbidden";
	case GhRepositoryResponseStatus::NotFound: return L"not-found";
	case GhRepositoryResponseStatus::Unauthorized: return L"unauthorized";
	case GhRepositoryResponseStatus::InvalidRequest: return L"invalid-request";
	case GhRepositoryResponseStatus::ToolUnavailable: return L"tool-unavailable";
	case GhRepositoryResponseStatus::UnsupportedVersion: return L"unsupported-version";
	case GhRepositoryResponseStatus::TimedOut: return L"timed-out";
	case GhRepositoryResponseStatus::Cancelled: return L"cancelled";
	case GhRepositoryResponseStatus::OutputLimitExceeded: return L"output-limit-exceeded";
	case GhRepositoryResponseStatus::InvalidEnvelope: return L"invalid-envelope";
	case GhRepositoryResponseStatus::InvalidJson: return L"invalid-json";
	case GhRepositoryResponseStatus::Failed: return L"failed";
	default: return L"";
	}
}

//! Only the printable characters an ETag or handle may contribute are kept, so the
//! envelope cannot carry a quote, a reverse solidus or a control character.
bool IsEnvelopeCharacter(const wchar_t character) noexcept
{
	return character >= 0x20 && character <= 0x7e && character != L'"' && character != 0x5c;
}

std::wstring JsonToken(const std::string& value)
{
	std::wstring token;
	for (const unsigned char character : value) {
		const auto wide = static_cast<wchar_t>(character);
		if (!IsEnvelopeCharacter(wide)) continue;
		token.push_back(wide);
		if (token.size() >= 128) break;
	}
	return token;
}

std::wstring JsonToken(const std::wstring& value)
{
	std::wstring token;
	for (const wchar_t character : value) {
		if (!IsEnvelopeCharacter(character)) continue;
		token.push_back(character);
		if (token.size() >= 128) break;
	}
	return token;
}

effect::ToolCompleted Refused(const std::wstring& readId, const std::wstring& message)
{
	return { readId, effect::CompletionStatus::Failed, L"", message };
}

constexpr wchar_t kFieldSeparator = 0x1f;
constexpr wchar_t kSegmentSeparator = 0x1e;
constexpr wchar_t kQuerySeparator = 0x1d;

//! Mirrors GhReadResourceKey::SameResource so one cached page belongs to exactly
//! the scheduler entry that produced it. The conditional header is excluded on
//! purpose: the scheduler deduplicates without it too.
std::wstring CanonicalKey(const GhReadResourceKey& key)
{
	std::wstring canonical = key.ProfileId();
	canonical += kFieldSeparator;
	canonical += std::to_wstring(key.AccountGeneration());
	canonical += kFieldSeparator;
	canonical += key.RepositoryIdentity();
	canonical += kFieldSeparator;
	canonical += key.Request().Owner();
	canonical += kFieldSeparator;
	canonical += key.Request().Repository();
	for (const auto& segment : key.Request().ResourceSegments()) {
		canonical += kSegmentSeparator;
		canonical += segment;
	}
	for (const auto& entry : key.Request().Query()) {
		canonical += kQuerySeparator;
		canonical += entry.first;
		canonical += L'=';
		canonical += entry.second;
	}
	return canonical;
}

} // namespace

std::optional<GhRepositoryReadRequest> BuildRepositoryReadRequest(
	const GhSelectedRepository& repository, const std::vector<effect::Field>& arguments)
{
	if (arguments.size() > 16) return std::nullopt;
	std::wstring shape, id;
	std::optional<std::wstring> etag;
	std::vector<std::pair<std::wstring, std::wstring>> query;
	for (const auto& argument : arguments) {
		if (argument.value.empty()) return std::nullopt;
		const auto duplicate = std::ranges::any_of(query,
			[&](const auto& entry) { return entry.first == argument.name; });
		if (duplicate) return std::nullopt;
		if (argument.name == L"shape") {
			if (!shape.empty()) return std::nullopt;
			shape = argument.value;
		} else if (argument.name == L"id") {
			if (!id.empty() || !IsDecimalId(argument.value)) return std::nullopt;
			id = argument.value;
		} else if (argument.name == L"etag") {
			if (etag) return std::nullopt;
			etag = argument.value;
		} else if (IsForwardedQuery(argument.name)) {
			query.emplace_back(argument.name, argument.value);
		} else {
			return std::nullopt;
		}
	}
	if (shape.empty() || ShapeNeedsId(shape) != !id.empty()) return std::nullopt;
	auto segments = ResourceSegments(shape, id);
	if (!segments) return std::nullopt;
	// Deterministic order keeps one logical read on one scheduler resource.
	std::ranges::sort(query, [](const auto& left, const auto& right) { return left.first < right.first; });
	return GhRepositoryReadRequest(repository.Hostname(), repository.Owner(), repository.Repository(),
		std::move(*segments), std::move(query), std::move(etag));
}

CSenpGitHubToolExecutor::CSenpGitHubToolExecutor(std::shared_ptr<const IGhToolPlatform> toolPlatform,
	std::shared_ptr<ISenpGitHubProfileSource> profiles, std::wstring workingDirectory) :
	m_profiles(std::move(profiles)), m_policy(std::move(toolPlatform), std::move(workingDirectory)),
	m_reader(m_policy)
{
	m_worker = std::thread([this]() noexcept { Run(); });
}

CSenpGitHubToolExecutor::~CSenpGitHubToolExecutor()
{
	{
		std::scoped_lock lock(m_mutex);
		m_stopping = true;
	}
	// Close signals every running dispatch; the worker still owns the matching
	// Complete, so physical cleanup finishes before the thread is joined.
	m_scheduler.Close();
	m_wakeWorker.notify_all();
	if (m_worker.joinable()) m_worker.join();
	std::scoped_lock lock(m_mutex);
	for (auto& state : m_scopes) Release(*state);
	m_scopes.clear();
	m_pages.clear();
	m_store.Close();
}

CSenpGitHubToolExecutor::ScopeState* CSenpGitHubToolExecutor::Find(const SenpToolExecutionScope& scope) noexcept
{
	const auto found = std::ranges::find_if(m_scopes,
		[&](const auto& state) { return state->scope == scope; });
	return found == m_scopes.end() ? nullptr : found->get();
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::StartRead(const SenpToolExecutionScope& scope,
	const platform::controlipc::SenpToolReadCommand& command) noexcept
try {
	if (scope.profileId.empty() || command.readId.empty()) return EControlSenpRpcStatus::InvalidRequest;
	// A zero account generation means no account has been adopted for the profile.
	// That is not an authentication failure and must not be answered as one.
	if (scope.owner.accountGeneration <= 0 || scope.owner.generation <= 0) return EControlSenpRpcStatus::NotConnected;
	if (!m_profiles) return EControlSenpRpcStatus::Unavailable;
	const auto repository = m_profiles->Repository(scope.profileId);
	if (!repository) return EControlSenpRpcStatus::Unavailable;
	if (!m_profiles->Connection(scope.profileId)) return EControlSenpRpcStatus::NotConnected;
	const auto request = BuildRepositoryReadRequest(*repository, command.arguments);
	if (!request) return EControlSenpRpcStatus::InvalidRequest;

	std::unique_lock lock(m_mutex);
	if (m_stopping) return EControlSenpRpcStatus::Closed;
	auto* state = Find(scope);
	if (!state) {
		if (m_scopes.size() >= MaximumScopes()) return EControlSenpRpcStatus::ResourceExhausted;
		const auto profileId = Narrow(scope.profileId);
		const auto extensionId = Narrow(scope.owner.extensionId);
		const auto digest = PackageDigest(scope.owner.packageDigest);
		if (!profileId || !extensionId || !digest) return EControlSenpRpcStatus::InvalidRequest;
		auto created = std::make_unique<ScopeState>();
		created->scope = scope;
		// The resource scope names the connection that owns the resources. Grant
		// liveness is proved by the broker before every call reaching this object.
		created->resourceScope = { *profileId, *extensionId, *digest,
			"connection-" + std::to_string(scope.sessionId) + "-" + std::to_string(scope.clientProcessId),
			scope.owner.generation, scope.owner.workspaceRevision, scope.owner.accountGeneration,
			scope.owner.generation };
		state = created.get();
		m_scopes.push_back(std::move(created));
	}

	const auto key = GhReadResourceKey(scope.profileId,
		static_cast<std::uint64_t>(scope.owner.accountGeneration),
		repository->RepositoryIdentity(), *request);
	const auto canonical = CanonicalKey(key);
	const auto existing = std::ranges::find_if(state->reads,
		[&](const auto& read) { return read.readId == command.readId; });
	if (existing != state->reads.end()) {
		// A repeated StartRead for the same read identity is a refresh, not a
		// second subscription: the editor keeps exactly one read per identity.
		if (existing->cacheKey != canonical) return EControlSenpRpcStatus::InvalidRequest;
		switch (m_scheduler.RequestRefresh(existing->subscriptionId, NowMilliseconds())) {
		case GhReadMutationStatus::Applied: break;
		case GhReadMutationStatus::Closed: return EControlSenpRpcStatus::Closed;
		default: return EControlSenpRpcStatus::NotFound;
		}
		lock.unlock();
		m_wakeWorker.notify_all();
		return EControlSenpRpcStatus::Succeeded;
	}
	if (state->reads.size() >= MaximumReadsPerScope()) return EControlSenpRpcStatus::ResourceExhausted;
	const auto subscribed = m_scheduler.Subscribe(key, GhReadPollCadence::Manual, true, NowMilliseconds());
	switch (subscribed.Status()) {
	case GhReadSubscribeStatus::Accepted: break;
	case GhReadSubscribeStatus::InvalidScope: return EControlSenpRpcStatus::InvalidRequest;
	case GhReadSubscribeStatus::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::ResourceExhausted;
	}
	state->reads.push_back({ command.readId, canonical, subscribed.SubscriptionId(), 0 });
	lock.unlock();
	m_wakeWorker.notify_all();
	return EControlSenpRpcStatus::Succeeded;
} catch (...) {
	return EControlSenpRpcStatus::Unavailable;
}

std::optional<effect::ToolCompleted> CSenpGitHubToolExecutor::TakeCompleted(
	const SenpToolExecutionScope& scope) noexcept
try {
	std::scoped_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return std::nullopt;
	if (state->pending.empty()) Drain(*state);
	if (state->pending.empty()) return std::nullopt;
	auto completed = std::move(state->pending.front());
	state->pending.pop_front();
	return completed;
} catch (...) {
	return std::nullopt;
}

void CSenpGitHubToolExecutor::Drain(ScopeState& state)
{
	const auto now = NowMilliseconds();
	for (auto& read : state.reads) {
		const auto observation = m_scheduler.Poll(read.subscriptionId, now);
		if (!observation || !observation->Terminal()) continue;
		if (observation->Cycle() == read.deliveredCycle) continue;
		read.deliveredCycle = observation->Cycle();
		const auto page = std::ranges::find_if(m_pages, [&](const auto& entry) {
			return entry.cycle == observation->Cycle() && entry.cacheKey == read.cacheKey;
		});
		if (page == m_pages.end()) {
			// The scheduler reached a terminal that retained no page. Failing here
			// is honest: nothing was fetched that could be shown.
			state.pending.push_back(Refused(read.readId, L"page-unavailable"));
			continue;
		}
		if (auto completed = Publish(state, read, *page)) state.pending.push_back(std::move(*completed));
	}
}

std::optional<effect::ToolCompleted> CSenpGitHubToolExecutor::Publish(ScopeState& state,
	const Read& read, const Page& page)
{
	if (page.status != GhRepositoryResponseStatus::Succeeded
		&& page.status != GhRepositoryResponseStatus::NotModified) {
		return effect::ToolCompleted{ read.readId, ToCompletionStatus(page.status), L"", Describe(page.status) };
	}
	if (state.resources.size() >= MaximumResourcesPerScope()) {
		return Refused(read.readId, L"resource-limit");
	}
	const auto created = m_store.Create(state.resourceScope);
	if (created.result != TextResourceResult::Accepted) return Refused(read.readId, L"resource-unavailable");
	const std::string_view body = page.body;
	std::size_t offset = 0;
	auto accepted = true;
	while (accepted && offset < body.size()) {
		const auto size = (std::min)(SenpTextResourceStore::kChunkBytes, body.size() - offset);
		accepted = m_store.Append(state.resourceScope, created.handle, offset, body.substr(offset, size))
			== TextResourceResult::Accepted;
		offset += size;
	}
	if (accepted) {
		accepted = m_store.Finish(state.resourceScope, created.handle, TextResourceEnd::Complete)
			== TextResourceResult::Accepted;
	}
	if (!accepted) {
		(void)m_store.Release(state.resourceScope, created.handle);
		return Refused(read.readId, L"resource-unavailable");
	}
	state.resources.push_back(created.handle);
	// The body never travels inside a completion: the editor reads the resource
	// back in bounded chunks through the same connection-bound grant.
	std::wstring data = LR"({"resource":")" + JsonToken(created.handle) + LR"(","bytes":)"
		+ std::to_wstring(body.size()) + LR"(,"httpStatus":)" + std::to_wstring(page.httpStatus)
		+ LR"(,"page":)" + std::to_wstring(page.currentPage);
	if (page.nextPage) data += LR"(,"nextPage":)" + std::to_wstring(*page.nextPage);
	if (page.etag) data += LR"(,"etag":")" + JsonToken(*page.etag) + LR"(")";
	if (page.status == GhRepositoryResponseStatus::NotModified) data += LR"(,"notModified":true)";
	data += L"}";
	return effect::ToolCompleted{ read.readId, effect::CompletionStatus::Succeeded, std::move(data), L"" };
}

void CSenpGitHubToolExecutor::CancelRead(const SenpToolExecutionScope& scope,
	const std::wstring_view readId) noexcept
try {
	std::unique_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return;
	const auto found = std::ranges::find_if(state->reads,
		[&](const auto& read) { return read.readId == readId; });
	if (found == state->reads.end()) return;
	(void)m_scheduler.Unsubscribe(found->subscriptionId);
	state->reads.erase(found);
	std::erase_if(state->pending, [&](const auto& completed) { return completed.readId == readId; });
	lock.unlock();
	m_wakeWorker.notify_all();
} catch (...) {
}

void CSenpGitHubToolExecutor::Release(ScopeState& state) noexcept
{
	for (const auto& handle : state.resources) (void)m_store.Release(state.resourceScope, handle);
	state.resources.clear();
}

void CSenpGitHubToolExecutor::CancelScope(const SenpToolExecutionScope& scope) noexcept
try {
	std::unique_lock lock(m_mutex);
	const auto found = std::ranges::find_if(m_scopes,
		[&](const auto& state) { return state->scope == scope; });
	if (found == m_scopes.end()) return;
	for (const auto& read : (*found)->reads) (void)m_scheduler.Unsubscribe(read.subscriptionId);
	Release(**found);
	// The worker never holds a scope - it only writes keyed pages - so erasing the
	// record under this mutex is by itself proof that no worker can reach it.
	m_scopes.erase(found);
	lock.unlock();
	m_wakeWorker.notify_all();
} catch (...) {
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::ReadResource(const SenpToolExecutionScope& scope,
	const std::wstring_view handle, const std::uint64_t offset, const std::uint32_t length,
	ControlSenpRpcResponse& response) noexcept
try {
	if (handle.empty() || length == 0
		|| length > platform::controlipc::kControlSenpRpcMaximumResourceChunkBytes) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	std::scoped_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return EControlSenpRpcStatus::Unauthorized;
	if (std::ranges::find(state->resources, handle) == state->resources.end()) {
		return EControlSenpRpcStatus::NotFound;
	}
	const auto chunk = m_store.Read(state->resourceScope, handle,
		static_cast<std::size_t>(offset), length);
	switch (chunk.result) {
	case TextResourceResult::Accepted: break;
	case TextResourceResult::NotFound: return EControlSenpRpcStatus::NotFound;
	case TextResourceResult::Expired: return EControlSenpRpcStatus::Expired;
	case TextResourceResult::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::InvalidRequest;
	}
	response.resourceHandle = std::wstring(handle);
	response.resourceOffset = offset;
	response.resourceBytes = chunk.bytes;
	response.resourceState = static_cast<std::uint8_t>(chunk.state);
	response.resourceFinal = chunk.state == TextResourceState::Complete
		&& static_cast<std::size_t>(offset) + chunk.bytes.size() >= chunk.length;
	return EControlSenpRpcStatus::Succeeded;
} catch (...) {
	return EControlSenpRpcStatus::Unavailable;
}

void CSenpGitHubToolExecutor::ReleaseResource(const SenpToolExecutionScope& scope,
	const std::wstring_view handle) noexcept
try {
	std::scoped_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return;
	const auto found = std::ranges::find(state->resources, handle);
	if (found == state->resources.end()) return;
	(void)m_store.Release(state->resourceScope, *found);
	state->resources.erase(found);
} catch (...) {
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::QueryAccount(const std::wstring_view profileId,
	ControlSenpRpcResponse& response) noexcept
try {
	if (profileId.empty()) return EControlSenpRpcStatus::InvalidRequest;
	if (!m_profiles) return EControlSenpRpcStatus::Unavailable;
	const auto connection = m_profiles->Connection(profileId);
	// A profile the control side has never adopted a connection for answers
	// Unknown, not Disconnected: nothing has been checked, so nothing entitles
	// this executor to report the account as signed out.
	if (!connection) {
		response.accountGeneration = 0;
		response.accountState = EControlSenpAccountState::Unknown;
		return EControlSenpRpcStatus::Succeeded;
	}
	const auto snapshot = connection->Snapshot();
	response.accountGeneration = snapshot.AccountGeneration();
	response.accountState = ToAccountState(snapshot.State());
	return EControlSenpRpcStatus::Succeeded;
} catch (...) {
	return EControlSenpRpcStatus::Unavailable;
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::AdoptWorkspace(
	const platform::controlipc::SenpWorkspaceAdoption& adoption) noexcept
try {
	if (adoption.profileId.empty()) return EControlSenpRpcStatus::InvalidRequest;
	// The connection is the identity a declaration is withdrawn by, so one that
	// names no connection could never be withdrawn and is refused instead.
	if (adoption.connection.sessionId == 0 && adoption.connection.clientProcessId == 0) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	if (!m_profiles) return EControlSenpRpcStatus::Unavailable;
	// Forwarded, not stored: this executor never reads a folder, so holding the
	// declaration here would only put it a step further from what resolves it.
	return m_profiles->AdoptWorkspace(adoption);
} catch (...) {
	return EControlSenpRpcStatus::Unavailable;
}

void CSenpGitHubToolExecutor::WithdrawWorkspace(
	const platform::controlipc::SenpConnectionIdentity& connection) noexcept
try {
	if (!m_profiles) return;
	m_profiles->WithdrawWorkspace(connection);
} catch (...) {
}

bool CSenpGitHubToolExecutor::WaitForIdle(const std::uint32_t timeoutMilliseconds) noexcept
{
	const auto deadline = NowMilliseconds() + timeoutMilliseconds;
	for (;;) {
		std::unique_lock lock(m_mutex);
		const auto now = NowMilliseconds();
		if (!m_dispatching && m_scheduler.QueuedCount(now) == 0 && m_scheduler.RunningCount() == 0) {
			return true;
		}
		if (now >= deadline) return false;
		m_idle.wait_for(lock, std::chrono::milliseconds(kIdleWaitSliceMilliseconds));
	}
}

const GhToolProbe& CSenpGitHubToolExecutor::Probe()
{
	if (!m_probe || m_probe->Status() != GhToolAvailability::Available) m_probe = m_policy.Probe(nullptr);
	return *m_probe;
}

void CSenpGitHubToolExecutor::Run() noexcept
{
	for (;;) {
		std::optional<GhReadDispatch> dispatch;
		{
			std::unique_lock lock(m_mutex);
			if (m_stopping) break;
			try {
				dispatch = m_scheduler.TryDispatch(NowMilliseconds());
			} catch (...) {
				dispatch.reset();
			}
			if (!dispatch) {
				m_idle.notify_all();
				m_wakeWorker.wait_for(lock, std::chrono::milliseconds(kWorkerWakeMilliseconds));
				continue;
			}
			m_dispatching = true;
		}
		try {
			Execute(*dispatch);
		} catch (...) {
			// The scheduler requires a Complete for every dispatch it handed out.
			(void)m_scheduler.Complete(dispatch->Ticket(),
				GhRepositoryResponse(GhRepositoryResponseStatus::Failed, 0, {}, std::nullopt, 1, std::nullopt),
				NowMilliseconds(), NowUnixSeconds());
		}
		{
			std::scoped_lock lock(m_mutex);
			m_dispatching = false;
		}
		m_idle.notify_all();
	}
}

void CSenpGitHubToolExecutor::Execute(const GhReadDispatch& dispatch)
{
	const auto& key = dispatch.Key();
	GhRepositoryResponse response(GhRepositoryResponseStatus::ToolUnavailable, 0, {}, std::nullopt, 1, std::nullopt);
	const auto connection = m_profiles ? m_profiles->Connection(key.ProfileId()) : nullptr;
	if (connection) {
		// The account generation of the queued read is the fence: a reconnect that
		// adopted a new account cannot satisfy a read admitted against the old one.
		auto account = connection->Acquire(static_cast<std::int64_t>(key.AccountGeneration()));
		if (!account) {
			response = GhRepositoryResponse(GhRepositoryResponseStatus::Unauthorized, 0, {}, std::nullopt, 1, std::nullopt);
		} else {
			const auto& probe = Probe();
			response = probe.Status() == GhToolAvailability::Available
				? m_reader.Read(*account, probe, key.Request(), dispatch.StopHandle())
				: GhRepositoryResponse(GhRepositoryResponseStatus::ToolUnavailable, 0, {}, std::nullopt, 1, std::nullopt);
		}
	}
	const auto applied = m_scheduler.Complete(dispatch.Ticket(), response,
		NowMilliseconds(), NowUnixSeconds());
	if (applied != GhReadMutationStatus::Applied || dispatch.CancellationRequested()) return;

	Page page;
	page.cacheKey = CanonicalKey(key);
	page.cycle = dispatch.Cycle();
	page.status = response.Status();
	page.httpStatus = response.HttpStatus();
	page.currentPage = response.CurrentPage();
	page.nextPage = response.NextPage();
	page.etag = response.ETag();
	if (response.Body().size() > MaximumPageBytes()) {
		page.status = GhRepositoryResponseStatus::OutputLimitExceeded;
	} else if (response.Status() == GhRepositoryResponseStatus::Succeeded) {
		page.body.assign(response.Body().begin(), response.Body().end());
	}
	std::scoped_lock lock(m_mutex);
	std::erase_if(m_pages, [&](const auto& entry) {
		return entry.cycle == page.cycle && entry.cacheKey == page.cacheKey;
	});
	m_pages.push_back(std::move(page));
	while (m_pages.size() > MaximumCachedPages()) m_pages.pop_front();
}

} // namespace senp::github
