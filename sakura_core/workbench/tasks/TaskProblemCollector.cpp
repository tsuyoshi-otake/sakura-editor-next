/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/TaskProblemCollector.h"

#include "workbench/tasks/ProblemMatcherEngine.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace workbench::tasks {
namespace {

//! Bounded so one noisy task cannot grow the buffered stream without limit.
constexpr std::size_t kMaximumCapturedOutputBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumStableIdBytes = 160U;
constexpr std::wstring_view kTasksArtifactSuffix = L"/.vscode/tasks.json";

std::atomic<std::uint64_t> s_outputOperationSerial{ 1 };

std::string NextOutputOperationId()
{
	const auto serial = s_outputOperationSerial.fetch_add(1, std::memory_order_relaxed);
	return "tasks." + std::to_string(serial);
}

[[nodiscard]] std::string ToUtf8(const std::wstring_view text)
{
	if (text.empty()) return {};
	const int required = ::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0) return {};
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
	return result;
}

//! Marker and output stable ids are bounded UTF-8; keep the label's readable
//! characters and fold anything else to a dash rather than failing the run.
[[nodiscard]] std::string SanitizeStableId(std::string value)
{
	for (auto& character : value) {
		const unsigned char byte = static_cast<unsigned char>(character);
		const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
			|| (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' || byte == '_' || byte == ':';
		if (!safe) character = '-';
	}
	if (value.size() > kMaximumStableIdBytes) value.resize(kMaximumStableIdBytes);
	return value;
}

const char* ProblemMatchStatusName(const EProblemMatchStatus status) noexcept
{
	switch (status) {
	case EProblemMatchStatus::Ok: return "Ok";
	case EProblemMatchStatus::EmptyPatternList: return "EmptyPatternList";
	case EProblemMatchStatus::MaximumPatternsExceeded: return "MaximumPatternsExceeded";
	case EProblemMatchStatus::InvalidRegexp: return "InvalidRegexp";
	case EProblemMatchStatus::InvalidGroupIndex: return "InvalidGroupIndex";
	case EProblemMatchStatus::MissingRequiredCaptureField: return "MissingRequiredCaptureField";
	case EProblemMatchStatus::MissingWorkspaceRoot: return "MissingWorkspaceRoot";
	case EProblemMatchStatus::InvalidFileLocation: return "InvalidFileLocation";
	case EProblemMatchStatus::InvalidSeverityValue: return "InvalidSeverityValue";
	case EProblemMatchStatus::InvalidLineOrColumnValue: return "InvalidLineOrColumnValue";
	case EProblemMatchStatus::InvalidUtf8Line: return "InvalidUtf8Line";
	case EProblemMatchStatus::MaximumLinesExceeded: return "MaximumLinesExceeded";
	case EProblemMatchStatus::MaximumLineLengthExceeded: return "MaximumLineLengthExceeded";
	case EProblemMatchStatus::MaximumMarkersExceeded: return "MaximumMarkersExceeded";
	}
	return "Unknown";
}

const char* MarkerOperationStatusName(const problems::EMarkerOperationStatus status) noexcept
{
	switch (status) {
	case problems::EMarkerOperationStatus::Replaced: return "Replaced";
	case problems::EMarkerOperationStatus::Deleted: return "Deleted";
	case problems::EMarkerOperationStatus::CollectionCleared: return "CollectionCleared";
	case problems::EMarkerOperationStatus::OwnerDisposed: return "OwnerDisposed";
	case problems::EMarkerOperationStatus::Stopped: return "Stopped";
	case problems::EMarkerOperationStatus::AlreadyStopped: return "AlreadyStopped";
	case problems::EMarkerOperationStatus::NotApplicable: return "NotApplicable";
	case problems::EMarkerOperationStatus::InvalidOwner: return "InvalidOwner";
	case problems::EMarkerOperationStatus::InvalidCollection: return "InvalidCollection";
	case problems::EMarkerOperationStatus::InvalidResource: return "InvalidResource";
	case problems::EMarkerOperationStatus::InvalidMarker: return "InvalidMarker";
	case problems::EMarkerOperationStatus::MaximumOwnersExceeded: return "MaximumOwnersExceeded";
	case problems::EMarkerOperationStatus::MaximumCollectionsExceeded: return "MaximumCollectionsExceeded";
	case problems::EMarkerOperationStatus::MaximumResourcesExceeded: return "MaximumResourcesExceeded";
	case problems::EMarkerOperationStatus::MaximumMarkersExceeded: return "MaximumMarkersExceeded";
	case problems::EMarkerOperationStatus::MaximumPayloadExceeded: return "MaximumPayloadExceeded";
	case problems::EMarkerOperationStatus::StaleGeneration: return "StaleGeneration";
	case problems::EMarkerOperationStatus::StaleRevision: return "StaleRevision";
	case problems::EMarkerOperationStatus::RevisionExhausted: return "RevisionExhausted";
	}
	return "Unknown";
}

//! Splits the captured byte stream into LF-separated lines, dropping a
//! trailing CR. The final unterminated fragment is kept: a task may end
//! without a trailing newline and upstream feeds partial lines too.
[[nodiscard]] std::vector<std::string> SplitOutputLines(const std::vector<std::uint8_t>& buffer)
{
	std::vector<std::string> lines;
	std::string current;
	current.reserve(256);
	for (const auto byte : buffer) {
		if (byte == '\n') {
			if (!current.empty() && current.back() == '\r') current.pop_back();
			lines.push_back(std::move(current));
			current.clear();
			continue;
		}
		current.push_back(static_cast<char>(byte));
	}
	if (!current.empty()) {
		if (current.back() == '\r') current.pop_back();
		lines.push_back(std::move(current));
	}
	return lines;
}

/*!
	@brief One task run's output -> matcher -> markers pipeline.

	The service calls `OnOutput` for each drained chunk outside its lock and
	`OnTerminal` exactly once. All marker application happens inside the
	terminal delivery so Problems updates atomically at run end, the same
	boundary upstream's `problemCollectors` uses (its collector lives until
	the task's process end).
*/
class CTaskProblemCollector final : public ITaskRunObserver {
public:
	CTaskProblemCollector(
		TaskConfigurationDefinition definition,
		std::vector<ProblemMatcherDefinition> matchers,
		problems::MarkerService& markers,
		output::IOutputService& output,
		std::shared_ptr<CTaskMarkerGenerationBook> generations)
		: m_definition(std::move(definition))
		, m_matchers(std::move(matchers))
		, m_markers(markers)
		, m_output(output)
		, m_generations(std::move(generations))
		, m_workspaceRoot(DeriveTaskWorkspaceRootUri(m_definition))
		, m_collectionId(SanitizeStableId("task." + ToUtf8(m_definition.label)))
	{
		// Generations are minted at run start, not terminal delivery: a stale
		// run whose terminal callback lands after a newer run must still fence
		// below the newer run's generation. Matchers sharing an owner mint
		// once, because a second generation would adopt-and-wipe the first
		// matcher's just written data.
		for (const auto& matcher : m_matchers) {
			auto ownerId = ToUtf8(matcher.owner);
			if (ownerId.empty() || !problems::MarkerService::IsValidStableId(ownerId)) continue;
			m_ownerGenerations.try_emplace(std::move(ownerId), 0);
		}
		for (auto& [ownerId, generation] : m_ownerGenerations) {
			generation = m_generations->Next(ownerId);
		}
	}

	void OnOutput(const std::span<const std::uint8_t> bytes) noexcept override
	{
		try {
			const std::lock_guard lock(m_mutex);
			if (m_terminalSeen) return;
			const auto remaining = kMaximumCapturedOutputBytes - m_buffer.size();
			if (bytes.size() <= remaining) {
				m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
				return;
			}
			if (remaining != 0) m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.begin() + remaining);
			m_truncated = true;
		}
		catch (...) { m_truncated = true; }
	}

	void OnTerminal(
		const ETaskExecutionRunState /*terminalState*/, std::uint32_t /*exitCode*/,
		const std::wstring& /*diagnostic*/) noexcept override
	{
		std::vector<std::string> lines;
		bool truncated = false;
		{
			const std::lock_guard lock(m_mutex);
			if (m_terminalSeen) return;
			m_terminalSeen = true;
			truncated = m_truncated;
		}
		try { lines = SplitOutputLines(m_buffer); }
		catch (...) {
			ReportFailure("output capture could not be materialized");
			return;
		}
		if (truncated) {
			ReportFailure("output capture was truncated; reported problems may be incomplete");
		}

		std::map<std::string, std::vector<const ProblemMatcherDefinition*>> byOwner;
		for (const auto& matcher : m_matchers) {
			auto ownerId = ToUtf8(matcher.owner);
			if (ownerId.empty() || !problems::MarkerService::IsValidStableId(ownerId)) {
				ReportFailure("problem matcher owner '" + SanitizeStableId(ownerId)
					+ "' is not a valid marker owner id");
				continue;
			}
			byOwner[std::move(ownerId)].push_back(&matcher);
		}
		for (const auto& [ownerId, matchers] : byOwner) {
			ApplyOwner(ownerId, m_ownerGenerations.at(ownerId), matchers, lines);
		}
	}

private:
	void ApplyOwner(
		const std::string& ownerId,
		const std::uint64_t generation,
		const std::vector<const ProblemMatcherDefinition*>& matchers,
		const std::vector<std::string>& lines) noexcept
	{
		try {
			// The pre-minted generation is the reconciliation: adopting it
			// removes this owner's previous collections entirely, which is
			// upstream's changeOne/remove pair for resources the rerun no
			// longer reports, and a stale generation can never write back.
			const auto previous = m_generations->Resources(ownerId);
			std::vector<platform::uri::Uri> delivered;
			std::size_t failures = 0;
			for (const auto* matcher : matchers) {
				ProblemMatcherRunContext context;
				context.owner = { ownerId, generation };
				context.collectionId = m_collectionId;
				context.workspaceRoot = m_workspaceRoot;
				const auto outcome = ProblemMatcherEngine::ProcessOutputLines(*matcher, lines, context);
				if (!outcome.Succeeded()) {
					++failures;
					ReportFailure("problem matcher '" + SanitizeStableId(ToUtf8(matcher->owner))
						+ "' failed: " + ProblemMatchStatusName(outcome.status));
					continue;
				}
				for (const auto& request : outcome.replacements) {
					const auto result = m_markers.Replace(request);
					if (!result.Succeeded()) {
						ReportFailure("marker replacement failed: "
							+ std::string(MarkerOperationStatusName(result.status)));
						continue;
					}
					if (std::ranges::none_of(delivered, [&request](const auto& uri) {
						return platform::uri::UriIdentityService::MakeComparisonKey(uri)
							== platform::uri::UriIdentityService::MakeComparisonKey(request.resource);
					})) {
						delivered.push_back(request.resource);
					}
				}
			}
			// An owner whose matchers all failed leaves the last accepted marker
			// state untouched: the minted generation was never adopted, so the
			// previous generation stays live and the ledger is unchanged.
			if (failures == matchers.size()) return;
			// Upstream's remove(owner, uris): resources the previous run
			// delivered but this run did not get an explicit empty replace.
			// The first such call is also what adopts the new generation for a
			// zero-match run, which is what clears the prior markers.
			const auto deliveredKeys = [&delivered] {
				std::vector<std::wstring> keys;
				keys.reserve(delivered.size());
				for (const auto& uri : delivered) {
					keys.push_back(platform::uri::UriIdentityService::MakeComparisonKey(uri));
				}
				return keys;
			}();
			for (const auto& uri : previous) {
				const auto key = platform::uri::UriIdentityService::MakeComparisonKey(uri);
				if (std::ranges::find(deliveredKeys, key) != deliveredKeys.end()) continue;
				const problems::ReplaceMarkersRequest remove {
					.collection = { { ownerId, generation }, m_collectionId },
					.resource = uri,
				};
				const auto result = m_markers.Replace(remove);
				if (!result.Succeeded() && result.status != problems::EMarkerOperationStatus::NotApplicable) {
					ReportFailure("marker removal failed: " + std::string(MarkerOperationStatusName(result.status)));
				}
			}
			m_generations->Commit(ownerId, generation, std::move(delivered));
		}
		catch (...) {
			ReportFailure("problem matcher application threw an exception");
		}
	}

	void ReportFailure(const std::string& detail) noexcept
	{
		try {
			(void)AppendTasksOutputLine(m_output,
				"task '" + SanitizeStableId(ToUtf8(m_definition.label)) + "': " + detail);
		}
		catch (...) { /* Diagnostics never own run state. */ }
	}

	std::mutex m_mutex;
	std::vector<std::uint8_t> m_buffer;
	bool m_terminalSeen{};
	bool m_truncated{};
	TaskConfigurationDefinition m_definition;
	std::vector<ProblemMatcherDefinition> m_matchers;
	std::map<std::string, std::uint64_t> m_ownerGenerations;
	problems::MarkerService& m_markers;
	output::IOutputService& m_output;
	std::shared_ptr<CTaskMarkerGenerationBook> m_generations;
	std::optional<platform::uri::Uri> m_workspaceRoot;
	std::string m_collectionId;
};

} // namespace

output::OutputOperationResult EnsureTasksOutputChannel(output::IOutputService& service)
{
	const output::OutputOwner owner { std::string(kTasksOutputOwnerId), kTasksOutputOwnerGeneration };
	const auto snapshot = service.Snapshot();
	for (const auto& channel : snapshot.channels) {
		if (channel.channelId == std::string(kTasksOutputChannelId)
			&& channel.owner == owner && channel.kind == output::EOutputChannelKind::Output) {
			output::OutputOperationResult result;
			result.status = output::EOutputOperationStatus::Succeeded;
			result.revision = snapshot.revision;
			return result;
		}
	}
	output::OutputCreateChannelRequest request;
	request.operation.operationId = NextOutputOperationId();
	request.owner = owner;
	request.channelId = std::string(kTasksOutputChannelId);
	request.label = std::string(kTasksOutputChannelLabel);
	request.kind = output::EOutputChannelKind::Output;
	return service.CreateChannel(request);
}

output::OutputOperationResult AppendTasksOutputLine(output::IOutputService& service, const std::string_view text)
{
	auto ensured = EnsureTasksOutputChannel(service);
	if (!ensured.Succeeded()) return ensured;
	output::OutputTextMutationRequest request;
	request.operation.operationId = NextOutputOperationId();
	request.owner = { std::string(kTasksOutputOwnerId), kTasksOutputOwnerGeneration };
	request.channelId = std::string(kTasksOutputChannelId);
	request.text.assign(text);
	if (request.text.empty() || request.text.back() != '\n') request.text.push_back('\n');
	return service.AppendOutput(request);
}

std::uint64_t CTaskMarkerGenerationBook::Next(const std::string_view ownerId)
{
	const std::lock_guard lock(m_mutex);
	auto& book = m_owners[std::string(ownerId)];
	if (book.generation != std::numeric_limits<std::uint64_t>::max()) ++book.generation;
	return book.generation;
}

std::vector<platform::uri::Uri> CTaskMarkerGenerationBook::Resources(const std::string_view ownerId) const
{
	const std::lock_guard lock(m_mutex);
	const auto found = m_owners.find(ownerId);
	return found == m_owners.end() ? std::vector<platform::uri::Uri> {} : found->second.resources;
}

void CTaskMarkerGenerationBook::Commit(
	const std::string_view ownerId, const std::uint64_t generation, std::vector<platform::uri::Uri> resources)
{
	const std::lock_guard lock(m_mutex);
	auto& book = m_owners[std::string(ownerId)];
	if (book.generation != generation) return;
	book.resources = std::move(resources);
}

std::optional<platform::uri::Uri> DeriveTaskWorkspaceRootUri(const TaskConfigurationDefinition& definition)
{
	if (!definition.sourceUri) return std::nullopt;
	std::wstring text = definition.sourceUri->ToString();
	if (text.size() <= kTasksArtifactSuffix.size()
		|| text.compare(text.size() - kTasksArtifactSuffix.size(), kTasksArtifactSuffix.size(), kTasksArtifactSuffix) != 0) {
		return std::nullopt;
	}
	text.resize(text.size() - kTasksArtifactSuffix.size());
	auto parsed = platform::uri::Uri::Parse(text);
	if (!parsed) return std::nullopt;
	return std::move(*parsed.value);
}

TaskRunObserverFactory MakeTaskRunObserverFactory(
	problems::MarkerService& markers,
	output::IOutputService& output,
	std::shared_ptr<CTaskMarkerGenerationBook> generations)
{
	return [&markers, &output, generations = std::move(generations)](
		const std::uint64_t /*runId*/, const TaskConfigurationDefinition& definition) -> std::shared_ptr<ITaskRunObserver> {
		if (definition.problemMatchers.empty()) return {};
		std::vector<ProblemMatcherDefinition> matchers;
		matchers.reserve(definition.problemMatchers.size());
		for (const auto& name : definition.problemMatchers) {
			const auto resolved = BuiltinProblemMatchers::Resolve(name);
			if (!resolved.Succeeded()) {
				// The catalog already flags an unknown name, so Start rejects
				// such a task before this factory ever runs. Keep the guard so
				// the seam stays fail-closed for direct service callers.
				(void)AppendTasksOutputLine(output,
					"task '" + SanitizeStableId(ToUtf8(definition.label))
						+ "': problem matcher '" + SanitizeStableId(ToUtf8(name)) + "' is not supported");
				return {};
			}
			matchers.push_back(*resolved.definition);
		}
		try {
			return std::make_shared<CTaskProblemCollector>(
				definition, std::move(matchers), markers, output, generations);
		}
		catch (...) {
			return {};
		}
	};
}

} // namespace workbench::tasks
