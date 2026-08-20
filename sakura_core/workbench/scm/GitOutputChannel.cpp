/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/scm/GitOutputChannel.h"

#include "workbench/scm/GitRefModel.h"

#include <utility>

namespace workbench::scm {
namespace {

[[nodiscard]] std::string ToUtf8(std::wstring_view text)
{
	if (text.empty()) {
		return {};
	}
	const int required = ::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0) {
		return {};
	}
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
	return result;
}

//! Upstream's raw `args.join(' ')`: a literal space-joined argv, not a
//! reparsable command line. This is the text VS Code's own Git Output channel
//! shows, so no quoting is added here either.
[[nodiscard]] std::wstring JoinArguments(const std::vector<std::wstring>& arguments)
{
	std::wstring joined;
	for (std::size_t index = 0; index < arguments.size(); ++index) {
		if (index != 0) {
			joined.push_back(L' ');
		}
		joined += arguments[index];
	}
	return joined;
}

//!
//! @brief Reproduces `extensions/git/src/main.ts`'s log listener: split the
//! raw logged text on `\r?\n`, drop every trailing blank line, then rejoin
//! the remainder with `\n`. A single-line input is unaffected; this only
//! matters for multi-line stderr that ends with one or more blank lines,
//! which `appendLine` would otherwise render as trailing empty rows.
//!
[[nodiscard]] std::wstring TrimTrailingBlankLines(std::wstring_view text)
{
	std::vector<std::wstring_view> lines;
	std::size_t start = 0;
	for (std::size_t index = 0; index <= text.size(); ++index) {
		if (index == text.size() || text[index] == L'\n') {
			std::wstring_view line = text.substr(start, index - start);
			if (!line.empty() && line.back() == L'\r') {
				line.remove_suffix(1);
			}
			lines.push_back(line);
			start = index + 1;
		}
	}
	while (!lines.empty() && lines.back().empty()) {
		lines.pop_back();
	}
	std::wstring rejoined;
	for (std::size_t index = 0; index < lines.size(); ++index) {
		if (index != 0) {
			rejoined.push_back(L'\n');
		}
		rejoined += lines[index];
	}
	return rejoined;
}

} // namespace

output::OutputOperationResult EnsureGitOutputChannel(
	output::OutputService& service,
	const output::OutputOwner& owner,
	const std::string& operationId)
{
	// Snapshot-based existence check first: `OutputService`'s remembered-operation
	// replay cache is bounded (`maximumRememberedOperations`), so relying on it
	// alone would make a long-lived owner generation eventually see a spurious
	// `Conflict` on its Nth `RunGitLogged` call once the original create
	// operation has been evicted. A channel that already exists for this exact
	// owner/generation/kind needs no further mutation.
	const auto snapshot = service.Snapshot();
	for (const auto& channel : snapshot.channels) {
		if (channel.channelId == std::string(kGitOutputChannelId)
			&& channel.owner == owner
			&& channel.kind == output::EOutputChannelKind::Log) {
			output::OutputOperationResult result;
			result.status = output::EOutputOperationStatus::Succeeded;
			result.revision = snapshot.revision;
			return result;
		}
	}

	output::OutputCreateChannelRequest request;
	request.operation.operationId = operationId;
	request.owner = owner;
	request.channelId = std::string(kGitOutputChannelId);
	request.label = std::string(kGitOutputChannelLabel);
	request.kind = output::EOutputChannelKind::Log;
	return service.CreateChannel(request);
}

std::vector<output::OutputLogEntry> BuildGitOutputLogEntries(
	const std::vector<std::wstring>& arguments,
	std::chrono::milliseconds elapsed,
	const GitExecutionResult& result)
{
	std::vector<output::OutputLogEntry> entries;

	std::wstring header = L"> git ";
	header += JoinArguments(arguments);
	header += L" [";
	header += std::to_wstring(elapsed.count());
	header += L"ms]";

	output::OutputLogEntry headerEntry;
	headerEntry.level = output::EOutputLogLevel::Info;
	headerEntry.message = ToUtf8(TrimTrailingBlankLines(header));
	if (headerEntry.message.empty()) {
		// `IsValidBoundedText` rejects an empty message; an unusable line (for
		// example an encoding failure) still leaves a non-empty marker rather
		// than silently dropping the whole invocation from the channel.
		headerEntry.message = "> git";
	}
	entries.push_back(std::move(headerEntry));

	// Matches upstream's own gate: `if (bufferResult.stderr.length > 0)`, on the
	// raw byte length, before any trimming.
	if (!result.standardError.empty()) {
		const std::wstring decoded = DecodeGitOutput(std::string_view(result.standardError));
		std::wstring trimmed = TrimTrailingBlankLines(decoded);
		if (!trimmed.empty()) {
			output::OutputLogEntry errorEntry;
			errorEntry.level = output::EOutputLogLevel::Info;
			errorEntry.message = ToUtf8(trimmed);
			if (!errorEntry.message.empty()) {
				entries.push_back(std::move(errorEntry));
			}
		}
	}

	return entries;
}

output::OutputOperationResult AppendGitOutputLogEntries(
	output::OutputService& service,
	const output::OutputOwner& owner,
	const std::string& operationId,
	std::vector<output::OutputLogEntry> entries,
	std::optional<std::uint64_t> expectedRevision)
{
	output::OutputLogMutationRequest request;
	request.operation.operationId = operationId;
	request.operation.expectedRevision = expectedRevision;
	request.owner = owner;
	request.channelId = std::string(kGitOutputChannelId);
	request.entries = std::move(entries);
	return service.AppendLog(request);
}

GitExecutionResult RunGitLogged(
	const GitExecutionRequest& request,
	HANDLE stop,
	const GitOutputSink& sink)
{
	const auto start = std::chrono::steady_clock::now();
	GitExecutionResult result = RunGit(request, stop);

	// Logging is strictly best-effort: nothing below this point may change the
	// git command's own result, and any failure to log is silently swallowed.
	if (sink.service == nullptr) {
		return result;
	}

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start);

	const auto ensured = EnsureGitOutputChannel(*sink.service, sink.owner, sink.createOperationId);
	if (!ensured.Succeeded() && ensured.status != output::EOutputOperationStatus::Conflict) {
		return result;
	}

	if (!sink.nextAppendOperationId) {
		return result;
	}
	const auto operationId = sink.nextAppendOperationId();
	if (!operationId.has_value()) {
		return result;
	}

	auto entries = BuildGitOutputLogEntries(BuildEffectiveGitArguments(request), elapsed, result);
	(void)AppendGitOutputLogEntries(*sink.service, sink.owner, *operationId, std::move(entries));

	return result;
}

} // namespace workbench::scm
