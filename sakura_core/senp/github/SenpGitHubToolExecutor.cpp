/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include "senp/github/GhLogResource.h"
#include "senp/github/GhPageProjection.h"

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

/*!
	@brief Closed repository-read shapes.

	The editor names one of these; it never names a path segment, host, owner or
	repository of its own. Every shape the two GitHub extensions can ask for is
	listed here: a shape they need but this set lacks is not a smaller feature,
	it is a view that fails every read it issues.
*/
std::optional<std::vector<std::wstring>> ResourceSegments(const std::wstring& shape,
	const std::wstring& id, const std::wstring& attempt)
{
	if (shape == L"issues") return std::vector<std::wstring>{ L"issues" };
	if (shape == L"pulls") return std::vector<std::wstring>{ L"pulls" };
	if (shape == L"runs") return std::vector<std::wstring>{ L"actions", L"runs" };
	if (shape == L"workflows") return std::vector<std::wstring>{ L"actions", L"workflows" };
	if (shape == L"issue") return std::vector<std::wstring>{ L"issues", id };
	if (shape == L"pull") return std::vector<std::wstring>{ L"pulls", id };
	// An issue's comment list is keyed by the issue number; one comment is keyed
	// by its own id under a different collection. They are separate shapes
	// because the id means a different thing in each.
	if (shape == L"issueComments") return std::vector<std::wstring>{ L"issues", id, L"comments" };
	if (shape == L"issueComment") return std::vector<std::wstring>{ L"issues", L"comments", id };
	if (shape == L"run") return std::vector<std::wstring>{ L"actions", L"runs", id };
	if (shape == L"runJobs") return std::vector<std::wstring>{ L"actions", L"runs", id, L"jobs" };
	if (shape == L"workflowRuns") return std::vector<std::wstring>{ L"actions", L"workflows", id, L"runs" };
	if (shape == L"runAttempt") {
		return std::vector<std::wstring>{ L"actions", L"runs", id, L"attempts", attempt };
	}
	if (shape == L"runAttemptJobs") {
		return std::vector<std::wstring>{ L"actions", L"runs", id, L"attempts", attempt, L"jobs" };
	}
	if (shape == L"job") return std::vector<std::wstring>{ L"actions", L"jobs", id };
	return std::nullopt;
}

bool ShapeNeedsId(const std::wstring& shape) noexcept
{
	return shape == L"issue" || shape == L"pull" || shape == L"issueComments"
		|| shape == L"issueComment" || shape == L"run" || shape == L"runJobs"
		|| shape == L"workflowRuns" || shape == L"runAttempt"
		|| shape == L"runAttemptJobs" || shape == L"job";
}

//! A run attempt is named by the run and the attempt number together. Neither
//! alone identifies it, so the second number is required exactly here and
//! refused everywhere else rather than being ignored.
bool ShapeNeedsAttempt(const std::wstring& shape) noexcept
{
	return shape == L"runAttempt" || shape == L"runAttemptJobs";
}

//! Query names the editor may forward. They are a subset of the tool policy's own
//! closed set; the policy still validates every value independently.
bool IsForwardedQuery(const std::wstring& name) noexcept
{
	static constexpr std::array<std::wstring_view, 9> forwarded{
		L"actor", L"branch", L"direction", L"event", L"page", L"per_page",
		L"sort", L"state", L"status",
	};
	return std::ranges::find(forwarded, name) != forwarded.end();
}

//! The shape a repositoryRead argument list names, empty when it names none.
//! A list that reached a built request always names exactly one.
std::wstring ShapeArgument(const std::vector<effect::Field>& arguments)
{
	const auto named = std::ranges::find_if(arguments,
		[](const auto& argument) { return argument.name == L"shape"; });
	return named == arguments.end() ? std::wstring() : named->value;
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

//! A log carries its own terminal vocabulary because it never went through the
//! JSON envelope a page does. The two are mapped separately rather than folded
//! into one enum, so neither can quietly inherit the other's meanings.
effect::CompletionStatus ToCompletionStatus(const GhLogResourceStatus status) noexcept
{
	switch (status) {
	case GhLogResourceStatus::Succeeded:
		return effect::CompletionStatus::Succeeded;
	case GhLogResourceStatus::Cancelled:
		return effect::CompletionStatus::Cancelled;
	case GhLogResourceStatus::TimedOut:
		return effect::CompletionStatus::TimedOut;
	case GhLogResourceStatus::ToolUnavailable:
	case GhLogResourceStatus::UnsupportedVersion:
		return effect::CompletionStatus::HostUnavailable;
	default:
		return effect::CompletionStatus::Failed;
	}
}

std::wstring Describe(const GhLogResourceStatus status)
{
	switch (status) {
	case GhLogResourceStatus::UnavailableOrNotFound: return L"unavailable-or-not-found";
	case GhLogResourceStatus::InvalidRequest: return L"invalid-request";
	case GhLogResourceStatus::ToolUnavailable: return L"tool-unavailable";
	case GhLogResourceStatus::UnsupportedVersion: return L"unsupported-version";
	case GhLogResourceStatus::TimedOut: return L"timed-out";
	case GhLogResourceStatus::Cancelled: return L"cancelled";
	case GhLogResourceStatus::LimitExceeded: return L"output-limit-exceeded";
	case GhLogResourceStatus::ResourceUnavailable: return L"resource-unavailable";
	default: return L"failed";
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

//! False when the bytes are not well-formed UTF-8. GitHub answers UTF-8, so a
//! body that is not is not a body this can carry, and guessing an encoding for
//! it would put invented characters inside a document.
bool Widen(const std::string& bytes, std::wstring& widened)
{
	if (bytes.empty()) return true;
	if (bytes.size() > (std::numeric_limits<int>::max)()) return false;
	const auto length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
		static_cast<int>(bytes.size()), nullptr, 0);
	if (length <= 0) return false;
	widened.resize(static_cast<std::size_t>(length));
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
		static_cast<int>(bytes.size()), widened.data(), length) == length;
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
	std::wstring shape, id, attempt;
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
		} else if (argument.name == L"attempt") {
			if (!attempt.empty() || !IsDecimalId(argument.value)) return std::nullopt;
			attempt = argument.value;
		} else if (argument.name == L"etag") {
			if (etag) return std::nullopt;
			etag = argument.value;
		} else if (IsForwardedQuery(argument.name)) {
			query.emplace_back(argument.name, argument.value);
		} else {
			return std::nullopt;
		}
	}
	if (shape.empty() || ShapeNeedsId(shape) != !id.empty()
		|| ShapeNeedsAttempt(shape) != !attempt.empty()) return std::nullopt;
	auto segments = ResourceSegments(shape, id, attempt);
	if (!segments) return std::nullopt;
	// A shape whose fields nobody wrote down cannot be answered: its page would
	// have to travel whole, and a real one does not fit a completion.
	if (!HasRepositoryPageProjection(shape)) return std::nullopt;
	// Deterministic order keeps one logical read on one scheduler resource.
	std::ranges::sort(query, [](const auto& left, const auto& right) { return left.first < right.first; });
	return GhRepositoryReadRequest(repository.Hostname(), repository.Owner(), repository.Repository(),
		std::move(*segments), std::move(query), std::move(etag));
}

std::optional<GhJobLogRequest> BuildJobLogRequest(const GhSelectedRepository& repository,
	const std::vector<effect::Field>& arguments)
{
	// Exactly one argument, and it is the job. There is no shape to choose here:
	// a log is one endpoint, so anything else named would be an argument this
	// operation has no meaning for.
	if (arguments.size() != 1 || arguments.front().name != L"id") return std::nullopt;
	const auto& value = arguments.front().value;
	if (!IsDecimalId(value)) return std::nullopt;
	// IsDecimalId already bounds the value to nineteen digits with no leading
	// zero, which cannot overflow an unsigned 64-bit accumulator.
	std::uint64_t jobId{};
	for (const wchar_t digit : value) jobId = jobId * 10 + static_cast<std::uint64_t>(digit - L'0');
	if (jobId == 0) return std::nullopt;
	return GhJobLogRequest(repository.Hostname(), repository.Owner(), repository.Repository(), jobId);
}

CSenpGitHubToolExecutor::CSenpGitHubToolExecutor(std::shared_ptr<const IGhToolPlatform> toolPlatform,
	std::shared_ptr<ISenpGitHubProfileSource> profiles, std::wstring workingDirectory) :
	m_profiles(std::move(profiles)), m_policy(std::move(toolPlatform), std::move(workingDirectory)),
	m_reader(m_policy)
{
	// Created before the worker exists, so the worker never observes it half set.
	m_logStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	StartWorker();
}

void CSenpGitHubToolExecutor::StartWorker()
{
	m_worker = platform::foundation::CNativeWorkerThread::Start<CSenpGitHubToolExecutor,
		&CSenpGitHubToolExecutor::Run>(this);
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
	// The log download waits on this alone; without it the join would wait out
	// the whole request timeout.
	if (m_logStop) (void)::SetEvent(m_logStop);
	m_wakeWorker.notify_all();
	m_worker.Join();
	std::scoped_lock lock(m_mutex);
	for (auto& state : m_scopes) Release(*state);
	m_scopes.clear();
	m_pages.clear();
	m_logQueue.clear();
	m_store.Close();
	if (m_logStop) (void)::CloseHandle(m_logStop);
}

CSenpGitHubToolExecutor::ScopeState* CSenpGitHubToolExecutor::Find(const SenpToolExecutionScope& scope) noexcept
{
	const auto found = std::ranges::find_if(m_scopes,
		[&](const auto& state) { return state->scope == scope; });
	return found == m_scopes.end() ? nullptr : found->get();
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::Ensure(const SenpToolExecutionScope& scope,
	ScopeState*& state)
{
	state = Find(scope);
	if (state) return EControlSenpRpcStatus::Succeeded;
	if (m_scopes.size() >= MaximumScopes()) return EControlSenpRpcStatus::ResourceExhausted;
	const auto profileId = Narrow(scope.ProfileId());
	const auto extensionId = Narrow(scope.Owner().extensionId);
	const auto digest = PackageDigest(scope.Owner().packageDigest);
	if (!profileId || !extensionId || !digest) return EControlSenpRpcStatus::InvalidRequest;
	auto created = std::make_unique<ScopeState>();
	created->scope = scope;
	// The resource scope names the connection that owns the resources. Grant
	// liveness is proved by the broker before every call reaching this object.
	created->resourceScope = { *profileId, *extensionId, *digest,
		"connection-" + std::to_string(scope.SessionId()) + "-" + std::to_string(scope.ClientProcessId()),
		scope.Owner().generation, scope.Owner().workspaceRevision, scope.Owner().accountGeneration,
		scope.Owner().generation };
	state = created.get();
	m_scopes.push_back(std::move(created));
	return EControlSenpRpcStatus::Succeeded;
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::StartRead(const SenpToolExecutionScope& scope,
	const platform::controlipc::SenpToolReadCommand& command) noexcept
try {
	if (scope.ProfileId().empty() || command.ReadId().empty()) return EControlSenpRpcStatus::InvalidRequest;
	// A zero account generation means no account has been adopted for the profile.
	// That is not an authentication failure and must not be answered as one.
	if (scope.Owner().accountGeneration <= 0 || scope.Owner().generation <= 0) return EControlSenpRpcStatus::NotConnected;
	if (!m_profiles) return EControlSenpRpcStatus::Unavailable;
	// The broker admits the closed operation set before anything reaches here.
	// Naming it again is not distrust of the broker: this object has to know
	// which of the two shapes a read is, and one it cannot name has no shape.
	if (command.ToolId() != platform::controlipc::kSenpGitHubToolId) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	if (command.Operation() == platform::controlipc::kSenpGitHubJobLogOperation) {
		return StartJobLog(scope, command);
	}
	if (command.Operation() != platform::controlipc::kSenpGitHubRepositoryReadOperation) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	const auto repository = m_profiles->Repository(scope.ProfileId());
	if (!repository) return EControlSenpRpcStatus::Unavailable;
	if (!m_profiles->Connection(scope.ProfileId())) return EControlSenpRpcStatus::NotConnected;
	const auto request = BuildRepositoryReadRequest(*repository, command.Arguments());
	if (!request) return EControlSenpRpcStatus::InvalidRequest;
	// The request no longer spells the shape, and the page it comes back with has
	// to be reduced to that shape's fields. Reading the argument again here keeps
	// one definition of what a request is and still remembers the question.
	const auto shape = ShapeArgument(command.Arguments());

	std::unique_lock lock(m_mutex);
	if (m_stopping) return EControlSenpRpcStatus::Closed;
	ScopeState* state = nullptr;
	if (const auto admitted = Ensure(scope, state); admitted != EControlSenpRpcStatus::Succeeded) {
		return admitted;
	}
	// A read identity means one read. One already spent on a log is not a page
	// this could refresh, so it is refused rather than quietly answered twice.
	if (std::ranges::find(state->logReads, command.ReadId()) != state->logReads.end()) {
		return EControlSenpRpcStatus::InvalidRequest;
	}

	const auto key = GhReadResourceKey(scope.ProfileId(),
		static_cast<std::uint64_t>(scope.Owner().accountGeneration),
		repository->RepositoryIdentity(), *request);
	const auto canonical = CanonicalKey(key);
	const auto existing = std::ranges::find_if(state->reads,
		[&](const auto& read) { return read.readId == command.ReadId(); });
	if (existing != state->reads.end()) {
		// A repeated StartRead for the same read identity is a refresh, not a
		// second subscription: the editor keeps exactly one read per identity.
		if (existing->cacheKey != canonical) return EControlSenpRpcStatus::InvalidRequest;
		switch (existing->subscription.RequestRefresh(NowMilliseconds())) {
		case GhReadMutationStatus::Applied: break;
		case GhReadMutationStatus::Closed: return EControlSenpRpcStatus::Closed;
		default: return EControlSenpRpcStatus::NotFound;
		}
		lock.unlock();
		m_wakeWorker.notify_all();
		return EControlSenpRpcStatus::Succeeded;
	}
	if (state->reads.size() + state->logReads.size() >= MaximumReadsPerScope()) {
		return EControlSenpRpcStatus::ResourceExhausted;
	}
	GhReadSubscription subscription(m_scheduler, key, GhReadPollCadence::Manual, true, NowMilliseconds());
	switch (subscription.Status()) {
	case GhReadSubscribeStatus::Accepted: break;
	case GhReadSubscribeStatus::InvalidScope: return EControlSenpRpcStatus::InvalidRequest;
	case GhReadSubscribeStatus::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::ResourceExhausted;
	}
	state->reads.emplace_back(command.ReadId(), shape, canonical, std::move(subscription), 0);
	lock.unlock();
	m_wakeWorker.notify_all();
	return EControlSenpRpcStatus::Succeeded;
} catch (const std::exception&) {
	return EControlSenpRpcStatus::Unavailable;
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::StartJobLog(const SenpToolExecutionScope& scope,
	const platform::controlipc::SenpToolReadCommand& command)
{
	const auto repository = m_profiles->Repository(scope.ProfileId());
	if (!repository) return EControlSenpRpcStatus::Unavailable;
	if (!m_profiles->Connection(scope.ProfileId())) return EControlSenpRpcStatus::NotConnected;
	const auto request = BuildJobLogRequest(*repository, command.Arguments());
	if (!request) return EControlSenpRpcStatus::InvalidRequest;

	std::unique_lock lock(m_mutex);
	if (m_stopping) return EControlSenpRpcStatus::Closed;
	ScopeState* state = nullptr;
	if (const auto admitted = Ensure(scope, state); admitted != EControlSenpRpcStatus::Succeeded) {
		return admitted;
	}
	// A log is refetched by asking for it again, never refreshed in place: there
	// is no conditional request for a stream of bytes. So a repeated identity is
	// a duplicate, whichever kind of read already holds it.
	const auto duplicate = std::ranges::find(state->logReads, command.ReadId()) != state->logReads.end()
		|| std::ranges::any_of(state->reads,
			[&](const auto& read) { return read.readId == command.ReadId(); });
	if (duplicate) return EControlSenpRpcStatus::InvalidRequest;
	if (state->reads.size() + state->logReads.size() >= MaximumReadsPerScope()
		|| state->logs.size() >= MaximumResourcesPerScope()
		|| m_logQueue.size() >= MaximumQueuedLogs()) {
		return EControlSenpRpcStatus::ResourceExhausted;
	}
	// The store is made here so the worker has no allocation left to fail on,
	// and so a job cancelled before it runs is torn down by whoever cancels it.
	m_logQueue.push_back(LogJob{ scope, command.ReadId(), scope.ProfileId(), state->resourceScope,
		*request, std::make_unique<SenpTextResourceStore>(), {} });
	state->logReads.push_back(command.ReadId());
	lock.unlock();
	m_wakeWorker.notify_all();
	return EControlSenpRpcStatus::Succeeded;
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
} catch (const std::exception&) {
	return std::nullopt;
}

void CSenpGitHubToolExecutor::Drain(ScopeState& state)
{
	const auto now = NowMilliseconds();
	for (auto& read : state.reads) {
		const auto observation = read.subscription.Poll(now);
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
	Read& read, const Page& page)
{
	if (page.status != GhRepositoryResponseStatus::Succeeded
		&& page.status != GhRepositoryResponseStatus::NotModified) {
		return effect::ToolCompleted{ read.readId, ToCompletionStatus(page.status), L"", Describe(page.status) };
	}
	// The extension parses this body itself, and the protocol gives it no way to
	// read a resource, so the body has to travel inside the completion. A real
	// list page is several hundred kilobytes and the wire bounds a completion at
	// 256 KiB, so what travels is the page reduced to the fields the shape names -
	// never the page cut short, which would be a body nothing can parse. The
	// bounds it still has to clear are stated rather than discovered: a projected
	// page over the budget and one that is not UTF-8 are refused by name.
	const auto projected = ProjectRepositoryPage(read.shape, page.body);
	if (!projected) return Refused(read.readId, L"page-unprojectable");
	if (projected->size() > MaximumInlineBodyBytes()) return Refused(read.readId, L"page-too-large");
	std::wstring body;
	if (!Widen(*projected, body)) return Refused(read.readId, L"invalid-encoding");
	// A refresh replaces what this read published rather than adding to it, so
	// the slot is given up before the next one is taken.
	ReleasePage(state, read);
	// The resource is the same bytes kept for a document that wants to name the
	// raw response. It is an extra rather than the answer, so a scope with no
	// slot left still gets its page - only without one.
	std::wstring handle;
	if (state.resources.size() < MaximumResourcesPerScope()) {
		const auto created = m_store.Create(state.resourceScope);
		if (created.result == TextResourceResult::Accepted) {
			const std::string_view bytes = page.body;
			std::size_t offset = 0;
			auto accepted = true;
			while (accepted && offset < bytes.size()) {
				const auto size = (std::min)(SenpTextResourceStore::kChunkBytes, bytes.size() - offset);
				accepted = m_store.Append(state.resourceScope, created.handle, offset,
					bytes.substr(offset, size)) == TextResourceResult::Accepted;
				offset += size;
			}
			if (accepted && m_store.Finish(state.resourceScope, created.handle, TextResourceEnd::Complete)
				== TextResourceResult::Accepted) {
				state.resources.push_back(created.handle);
				read.resource = created.handle;
				handle = created.handle;
			} else {
				(void)m_store.Release(state.resourceScope, created.handle);
			}
		}
	}
	std::wstring data = LR"({"bytes":)" + std::to_wstring(page.body.size())
		+ LR"(,"httpStatus":)" + std::to_wstring(page.httpStatus)
		+ LR"(,"page":)" + std::to_wstring(page.currentPage);
	if (!handle.empty()) data += LR"(,"resource":")" + JsonToken(handle) + LR"(")";
	if (page.nextPage) data += LR"(,"nextPage":)" + std::to_wstring(*page.nextPage);
	if (page.etag) data += LR"(,"etag":")" + JsonToken(*page.etag) + LR"(")";
	// A body-less answer is only ever a not-modified one, and saying the page is
	// unchanged is the whole content of that answer. Writing an empty body
	// instead would be handing the extension something to parse that is not JSON.
	if (page.status == GhRepositoryResponseStatus::NotModified) data += LR"(,"notModified":true)";
	if (!body.empty()) data += LR"(,"body":)" + body;
	data += L"}";
	return effect::ToolCompleted{ read.readId, effect::CompletionStatus::Succeeded, std::move(data), L"" };
}

void CSenpGitHubToolExecutor::ReleasePage(ScopeState& state, Read& read) noexcept
{
	if (read.resource.empty()) return;
	(void)m_store.Release(state.resourceScope, read.resource);
	std::erase(state.resources, read.resource);
	read.resource.clear();
}

void CSenpGitHubToolExecutor::CancelRead(const SenpToolExecutionScope& scope,
	const std::wstring_view readId) noexcept
try {
	std::unique_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return;
	if (const auto log = std::ranges::find(state->logReads, readId); log != state->logReads.end()) {
		// Forgetting the identity is the cancellation: a running download finds
		// it gone and discards everything it produced. Signalling only makes
		// that prompt instead of waiting out the request timeout.
		state->logReads.erase(log);
		std::erase_if(m_logQueue,
			[&](const auto& job) { return job.scope == scope && job.readId == readId; });
		if (m_runningLog && m_logStop && m_runningLogReadId == readId
			&& m_runningLogScope == scope) {
			(void)::SetEvent(m_logStop);
		}
		std::erase_if(state->pending, [&](const auto& completed) { return completed.readId == readId; });
		lock.unlock();
		m_wakeWorker.notify_all();
		return;
	}
	const auto found = std::ranges::find_if(state->reads,
		[&](const auto& read) { return read.readId == readId; });
	if (found == state->reads.end()) return;
	ReleasePage(*state, *found);
	// Erasing the read releases its scheduler admission: the subscription is a
	// member, so the record and the admission end together by construction.
	state->reads.erase(found);
	std::erase_if(state->pending, [&](const auto& completed) { return completed.readId == readId; });
	lock.unlock();
	m_wakeWorker.notify_all();
} catch (const std::exception&) {
}

void CSenpGitHubToolExecutor::Release(ScopeState& state) noexcept
{
	for (const auto& handle : state.resources) (void)m_store.Release(state.resourceScope, handle);
	state.resources.clear();
	for (auto& read : state.reads) read.resource.clear();
	// Each log owns the whole store it was written into, so dropping the record
	// is the release. Forgetting the identities is what stops a download that is
	// still running from handing this scope anything else.
	state.logs.clear();
	state.logReads.clear();
}

void CSenpGitHubToolExecutor::CancelScope(const SenpToolExecutionScope& scope) noexcept
try {
	std::unique_lock lock(m_mutex);
	const auto found = std::ranges::find_if(m_scopes,
		[&](const auto& state) { return state->scope == scope; });
	if (found == m_scopes.end()) return;
	std::erase_if(m_logQueue, [&](const auto& job) { return job.scope == (*found)->scope; });
	if (m_runningLog && m_logStop && m_runningLogScope == (*found)->scope) {
		(void)::SetEvent(m_logStop);
	}
	Release(**found);
	// The worker never holds a scope - it only writes keyed pages - so erasing the
	// record under this mutex is by itself proof that no worker can reach it.
	m_scopes.erase(found);
	lock.unlock();
	m_wakeWorker.notify_all();
} catch (const std::exception&) {
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
	// A log lives in the store its own download owned; a page lives in the one
	// shared by every page. The handle names exactly one of them.
	const auto log = std::ranges::find_if(state->logs,
		[&](const auto& entry) { return entry.handle == handle; });
	if (log == state->logs.end()
		&& std::ranges::find(state->resources, handle) == state->resources.end()) {
		return EControlSenpRpcStatus::NotFound;
	}
	const auto chunk = log != state->logs.end()
		? log->store->Read(log->resourceScope, handle, static_cast<std::size_t>(offset), length)
		: m_store.Read(state->resourceScope, handle, static_cast<std::size_t>(offset), length);
	switch (chunk.result) {
	case TextResourceResult::Accepted: break;
	case TextResourceResult::NotFound: return EControlSenpRpcStatus::NotFound;
	case TextResourceResult::Expired: return EControlSenpRpcStatus::Expired;
	case TextResourceResult::Closed: return EControlSenpRpcStatus::Closed;
	default: return EControlSenpRpcStatus::InvalidRequest;
	}
	response.SetResourceHandle(std::wstring(handle));
	response.SetResourceOffset(offset);
	response.SetResourceBytes(chunk.bytes);
	response.SetResourceState(static_cast<std::uint8_t>(chunk.state));
	response.SetResourceEnd(static_cast<std::uint8_t>(chunk.end));
	// The whole chunk as the store answered it. The editor decides from these
	// whether it has reached the end; nothing here decides that on its behalf.
	response.SetResourceLength(chunk.length);
	response.SetResourceRevision(chunk.revision);
	return EControlSenpRpcStatus::Succeeded;
} catch (const std::exception&) {
	return EControlSenpRpcStatus::Unavailable;
}

void CSenpGitHubToolExecutor::ReleaseResource(const SenpToolExecutionScope& scope,
	const std::wstring_view handle) noexcept
try {
	std::scoped_lock lock(m_mutex);
	auto* state = Find(scope);
	if (!state) return;
	if (const auto log = std::ranges::find_if(state->logs,
		[&](const auto& entry) { return entry.handle == handle; }); log != state->logs.end()) {
		state->logs.erase(log);
		return;
	}
	const auto found = std::ranges::find(state->resources, handle);
	if (found == state->resources.end()) return;
	(void)m_store.Release(state->resourceScope, *found);
	state->resources.erase(found);
	// The read that published it keeps running; it simply owns nothing now, so
	// its next page does not try to release a handle the editor already has.
	for (auto& read : state->reads) if (read.resource == handle) read.resource.clear();
} catch (const std::exception&) {
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
		response.SetAccountGeneration(0);
		response.SetAccountState(EControlSenpAccountState::Unknown);
		return EControlSenpRpcStatus::Succeeded;
	}
	const auto snapshot = connection->Snapshot();
	response.SetAccountGeneration(snapshot.AccountGeneration());
	response.SetAccountState(ToAccountState(snapshot.State()));
	return EControlSenpRpcStatus::Succeeded;
} catch (const std::exception&) {
	return EControlSenpRpcStatus::Unavailable;
}

EControlSenpRpcStatus CSenpGitHubToolExecutor::AdoptWorkspace(
	const platform::controlipc::SenpWorkspaceAdoption& adoption) noexcept
try {
	if (adoption.ProfileId().empty()) return EControlSenpRpcStatus::InvalidRequest;
	// The connection is the identity a declaration is withdrawn by, so one that
	// names no connection could never be withdrawn and is refused instead.
	if (adoption.Connection().SessionId() == 0 && adoption.Connection().ClientProcessId() == 0) {
		return EControlSenpRpcStatus::InvalidRequest;
	}
	if (!m_profiles) return EControlSenpRpcStatus::Unavailable;
	// Forwarded, not stored: this executor never reads a folder, so holding the
	// declaration here would only put it a step further from what resolves it.
	return m_profiles->AdoptWorkspace(adoption);
} catch (const std::exception&) {
	return EControlSenpRpcStatus::Unavailable;
}

void CSenpGitHubToolExecutor::WithdrawWorkspace(
	const platform::controlipc::SenpConnectionIdentity& connection) noexcept
try {
	if (!m_profiles) return;
	m_profiles->WithdrawWorkspace(connection);
} catch (const std::exception&) {
}

bool CSenpGitHubToolExecutor::WaitForIdle(const std::uint32_t timeoutMilliseconds) noexcept
{
	const auto deadline = NowMilliseconds() + timeoutMilliseconds;
	for (;;) {
		std::unique_lock lock(m_mutex);
		const auto now = NowMilliseconds();
		if (!m_dispatching && m_logQueue.empty() && !m_runningLog
			&& m_scheduler.QueuedCount(now) == 0 && m_scheduler.RunningCount() == 0) {
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
		std::optional<LogJob> job;
		std::optional<GhReadDispatch> dispatch;
		{
			std::unique_lock lock(m_mutex);
			if (m_stopping) break;
			if (!m_logQueue.empty()) {
				// Claimed and armed under the same lock a cancel takes, so a
				// cancel either removes the job here or signals the download.
				job.emplace(std::move(m_logQueue.front()));
				m_logQueue.pop_front();
				if (m_logStop) (void)::ResetEvent(m_logStop);
				m_runningLogScope = job->scope;
				m_runningLogReadId = job->readId;
				m_runningLog = true;
				m_dispatching = true;
			} else {
				try {
					dispatch = m_scheduler.TryDispatch(NowMilliseconds());
				} catch (const std::exception&) {
					dispatch.reset();
				}
				if (!dispatch) {
					m_idle.notify_all();
					m_wakeWorker.wait_for(lock, std::chrono::milliseconds(kWorkerWakeMilliseconds));
					continue;
				}
				m_dispatching = true;
			}
		}
		if (job) {
			ExecuteLog(*job);
			std::scoped_lock lock(m_mutex);
			m_runningLog = false;
			m_runningLogReadId.clear();
			m_dispatching = false;
			m_idle.notify_all();
			continue;
		}
		try {
			Execute(*dispatch);
		} catch (const std::exception&) {
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
	// Built before the lock is taken: copying a page is the expensive part, and
	// nothing else may read it until it is stored.
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
	// The terminal and the page it answers for become visible together, under
	// the same lock the drain holds while it polls this scheduler. A terminal
	// published first is one the drain can reach before the page exists, and
	// that drain has nothing to answer with but a refusal for a page that was
	// fetched and was about to be stored.
	std::scoped_lock lock(m_mutex);
	const auto applied = m_scheduler.Complete(dispatch.Ticket(), response,
		NowMilliseconds(), NowUnixSeconds());
	if (applied != GhReadMutationStatus::Applied || dispatch.CancellationRequested()) return;
	std::erase_if(m_pages, [&](const auto& entry) {
		return entry.cycle == page.cycle && entry.cacheKey == page.cacheKey;
	});
	m_pages.push_back(std::move(page));
	while (m_pages.size() > MaximumCachedPages()) m_pages.pop_front();
}

void CSenpGitHubToolExecutor::ExecuteLog(LogJob& job) noexcept
try {
	const auto connection = m_profiles ? m_profiles->Connection(job.profileId) : nullptr;
	// The account generation the scope was admitted against is the fence: a
	// reconnect that adopted a new account cannot satisfy this download.
	std::optional<GhAuthenticatedAccount> account;
	if (connection) account = connection->Acquire(job.resourceScope.accountGeneration);
	if (!account) {
		RecordLog(job, { job.readId, effect::CompletionStatus::HostUnavailable, L"", L"unauthorized" }, false);
		return;
	}
	const auto& probe = Probe();
	if (probe.Status() != GhToolAvailability::Available) {
		RecordLog(job, { job.readId, effect::CompletionStatus::HostUnavailable, L"", L"tool-unavailable" }, false);
		return;
	}
	const CGhLogResource resource(m_policy, *job.store);
	const auto downloaded = resource.Download(*account, probe, job.resourceScope,
		job.request, m_logStop);
	if (downloaded.Status() != GhLogResourceStatus::Succeeded) {
		RecordLog(job, { job.readId, ToCompletionStatus(downloaded.Status()), L"",
			Describe(downloaded.Status()) }, false);
		return;
	}
	job.handle = downloaded.Handle();
	// The bytes never travel inside a completion: the editor reads the resource
	// back in bounded chunks through the same grant, exactly as it does a page.
	std::wstring data = LR"({"resource":")" + JsonToken(job.handle) + LR"(","bytes":)"
		+ std::to_wstring(downloaded.AcceptedBytes()) + LR"(,"log":true})";
	RecordLog(job, { job.readId, effect::CompletionStatus::Succeeded, std::move(data), L"" }, true);
} catch (const std::exception&) {
	RecordLog(job, Refused(job.readId, L"failed"), false);
}

void CSenpGitHubToolExecutor::RecordLog(LogJob& job, effect::ToolCompleted completed,
	const bool keepResource) noexcept
try {
	std::scoped_lock lock(m_mutex);
	auto* state = Find(job.scope);
	if (!state) return;
	const auto admitted = std::ranges::find(state->logReads, job.readId);
	// Cancelled while it ran. Nothing is owed for a read nobody is waiting on,
	// and the store goes with the job.
	if (admitted == state->logReads.end()) return;
	state->logReads.erase(admitted);
	if (keepResource) {
		if (state->logs.size() >= MaximumResourcesPerScope()) {
			completed = Refused(job.readId, L"resource-limit");
		} else {
			state->logs.push_back({ std::move(job.handle), job.resourceScope, std::move(job.store) });
		}
	}
	state->pending.push_back(std::move(completed));
} catch (const std::exception&) {
}

} // namespace senp::github
