/*! @file
 * @brief Issue #274 provider-neutral Output authority measurement harness.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/output/IOutputService.h"
#include "workbench/output/OutputProviderFactory.h"
#include "workbench/output/OutputServiceTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <windows.h>

namespace workbench::output {
namespace {

constexpr std::uint64_t kSchemaVersion = 1;
constexpr std::uint64_t kDefaultSeed = 0x274'2026'0827ULL;
constexpr std::size_t kMaximumBlocks = 100;
constexpr std::size_t kMaximumWarmupBlocks = 20;
constexpr std::size_t kMaximumSnapshotIterations = 10'000;
constexpr std::size_t kDefaultLifecycleIterations = 512;
constexpr std::size_t kMaximumLifecycleIterations = 5'000;
constexpr std::chrono::seconds kCallbackWaitTimeout{ 2 };

using TraceRequest = std::variant<
	OutputCreateChannelRequest,
	OutputTextMutationRequest,
	OutputLogMutationRequest,
	OutputChannelMutationRequest,
	OutputShowChannelRequest,
	OutputDisposeOwnerRequest>;

enum class ETraceKind : std::uint8_t {
	CreateOutput,
	CreateLog,
	AppendShort,
	AppendLong,
	AppendUtf8Boundary,
	ReplaceOutput,
	AppendLog,
	ClearLog,
	ShowOutput,
	HideOutput,
	CreateReplacement,
	OldOwnerFenced,
	NotApplicableEmpty,
	DisposeOutput,
	DisposeOwner,
	ReplayCreateOutput,
	OperationIdConflict,
	StaleRevision,
	RejectedOwner,
	RejectedPayload,
	Stop,
	RepeatedStop,
	PostStopAppend,
};

struct TraceStep final {
	ETraceKind kind{};
	TraceRequest request;
};

struct TraceDefinition final {
	std::uint64_t seed{};
	OutputOwner owner;
	OutputOwner replacementOwner;
	std::string outputChannelId;
	std::string logChannelId;
	std::string shortText;
	std::string longText;
	std::string utf8BoundaryText;
	std::string replacementText;
	std::string oversizedText;
	std::vector<OutputLogEntry> logEntries;
	std::vector<TraceStep> steps;
};

struct MeasurementStatistics final {
	std::size_t count{};
	double median{};
	double p95{};
};

struct TraceOutcome final {
	std::uint64_t resultDigest{};
	std::uint64_t snapshotDigest{};
	std::array<std::uint64_t, 8> statusCounts{};
	std::uint64_t callbackCount{};
	std::uint64_t maximumCallbackDepth{};
	std::uint64_t droppedNotificationCount{};
};

struct QpcSample final {
	std::uint64_t begin{};
	std::uint64_t end{};
	std::uint64_t frequency{};
};

struct BenchmarkSettings final {
	std::filesystem::path outputPath;
	std::uint64_t seed{ kDefaultSeed };
	std::uint64_t affinityMask{ 1 };
	std::size_t warmupBlocks{ 2 };
	std::size_t measuredBlocks{ 10 };
	std::size_t snapshotIterations{ 256 };
	std::size_t lifecycleIterations{ kDefaultLifecycleIterations };
	std::string configuration;
	std::optional<std::string> expectedProvider;
};

struct JsonLinesWriter final {
	explicit JsonLinesWriter(const std::filesystem::path& path)
		: stream(path, std::ios::binary | std::ios::trunc)
	{
		if (!stream) throw std::runtime_error("benchmark output cannot be opened");
	}

	void Write(const std::string& line)
	{
		stream << line << '\n';
		stream.flush();
		if (!stream) throw std::runtime_error("benchmark output cannot be written");
	}

	std::ofstream stream;
};

std::string GetEnvironment(const char* name)
{
	const char* value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string(value);
}

std::uint64_t ParseUnsigned(std::string_view value, const char* name, const std::uint64_t fallback)
{
	if (value.empty()) return fallback;
	std::uint64_t parsed{};
	for (const unsigned char character : value) {
		if (character < '0' || character > '9') {
			throw std::runtime_error(std::string(name) + " must be an unsigned decimal integer");
		}
		if (parsed > (std::numeric_limits<std::uint64_t>::max() - (character - '0')) / 10U) {
			throw std::runtime_error(std::string(name) + " is too large");
		}
		parsed = parsed * 10U + (character - '0');
	}
	return parsed;
}

std::size_t ParseBoundedSize(
	std::string_view value,
	const char* name,
	const std::size_t fallback,
	const std::size_t maximum)
{
	const auto parsed = ParseUnsigned(value, name, fallback);
	if (parsed == 0 || parsed > maximum) {
		throw std::runtime_error(std::string(name) + " is outside its positive bound");
	}
	return static_cast<std::size_t>(parsed);
}

const char* CompiledConfiguration() noexcept
{
#if defined(_DEBUG)
	return "Debug";
#else
	return "Release";
#endif
}

std::string Hex(std::uint64_t value)
{
	std::ostringstream stream;
	stream << std::hex << value;
	return stream.str();
}

std::string Token(std::string_view prefix, const std::uint64_t seed, const std::uint64_t index)
{
	return std::string(prefix) + "." + Hex(seed) + "." + Hex(index);
}

std::string JsonEscape(std::string_view value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for (const unsigned char character : value) {
		switch (character) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (character < 0x20U) {
				std::ostringstream control;
				control << "\\u" << std::hex << std::setfill('0') << std::setw(4)
					<< static_cast<unsigned>(character);
				escaped += control.str();
			} else {
				escaped.push_back(static_cast<char>(character));
			}
			break;
		}
	}
	return escaped;
}

std::uint64_t Fnv1a(std::string_view value, std::uint64_t hash = 1469598103934665603ULL)
{
	for (const unsigned char character : value) {
		hash ^= character;
		hash *= 1099511628211ULL;
	}
	return hash;
}

template <typename Integer>
std::uint64_t HashInteger(std::uint64_t hash, const Integer value)
{
	static_assert(std::is_integral_v<Integer>);
	using Unsigned = std::make_unsigned_t<Integer>;
	const Unsigned unsignedValue = static_cast<Unsigned>(value);
	for (std::size_t index = 0; index < sizeof(Integer); ++index) {
		hash ^= static_cast<unsigned char>(unsignedValue >> (index * 8U));
		hash *= 1099511628211ULL;
	}
	return hash;
}

std::uint64_t HashString(std::uint64_t hash, const std::string& value)
{
	hash = HashInteger(hash, value.size());
	return Fnv1a(value, hash);
}

std::uint64_t HashOwner(std::uint64_t hash, const OutputOwner& owner)
{
	hash = HashString(hash, owner.ownerId);
	return HashInteger(hash, owner.generation);
}

std::uint64_t HashOperation(std::uint64_t hash, const OutputOperation& operation)
{
	hash = HashString(hash, operation.operationId);
	if (operation.expectedRevision) {
		hash = HashInteger(hash, std::uint8_t{ 1 });
		hash = HashInteger(hash, *operation.expectedRevision);
	} else {
		hash = HashInteger(hash, std::uint8_t{ 0 });
	}
	return hash;
}

std::uint64_t HashMetadata(std::uint64_t hash, const OutputChannelMetadata& metadata)
{
	if (metadata.languageId) {
		hash = HashInteger(hash, std::uint8_t{ 1 });
		hash = HashString(hash, *metadata.languageId);
	} else {
		hash = HashInteger(hash, std::uint8_t{ 0 });
	}
	if (metadata.source) {
		hash = HashInteger(hash, std::uint8_t{ 1 });
		hash = HashString(hash, *metadata.source);
	} else {
		hash = HashInteger(hash, std::uint8_t{ 0 });
	}
	return hash;
}

std::uint64_t HashTraceRequest(std::uint64_t hash, const TraceRequest& request)
{
	return std::visit([hash](const auto& value) mutable {
		using Value = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<Value, OutputCreateChannelRequest>) {
			hash = HashInteger(hash, std::uint8_t{ 1 });
			hash = HashOperation(hash, value.operation);
			hash = HashOwner(hash, value.owner);
			hash = HashString(hash, value.channelId);
			hash = HashString(hash, value.label);
			hash = HashInteger(hash, static_cast<std::uint8_t>(value.kind));
			return HashMetadata(hash, value.metadata);
		} else if constexpr (std::is_same_v<Value, OutputTextMutationRequest>) {
			hash = HashInteger(hash, std::uint8_t{ 2 });
			hash = HashOperation(hash, value.operation);
			hash = HashOwner(hash, value.owner);
			hash = HashString(hash, value.channelId);
			return HashString(hash, value.text);
		} else if constexpr (std::is_same_v<Value, OutputLogMutationRequest>) {
			hash = HashInteger(hash, std::uint8_t{ 3 });
			hash = HashOperation(hash, value.operation);
			hash = HashOwner(hash, value.owner);
			hash = HashString(hash, value.channelId);
			hash = HashInteger(hash, value.entries.size());
			for (const auto& entry : value.entries) {
				hash = HashInteger(hash, static_cast<std::uint8_t>(entry.level));
				hash = HashString(hash, entry.message);
				if (entry.source) {
					hash = HashInteger(hash, std::uint8_t{ 1 });
					hash = HashString(hash, *entry.source);
				} else {
					hash = HashInteger(hash, std::uint8_t{ 0 });
				}
			}
			return hash;
		} else if constexpr (std::is_same_v<Value, OutputChannelMutationRequest>) {
			hash = HashInteger(hash, std::uint8_t{ 4 });
			hash = HashOperation(hash, value.operation);
			hash = HashOwner(hash, value.owner);
			return HashString(hash, value.channelId);
		} else if constexpr (std::is_same_v<Value, OutputShowChannelRequest>) {
			hash = HashInteger(hash, std::uint8_t{ 5 });
			hash = HashOperation(hash, value.operation);
			hash = HashOwner(hash, value.owner);
			hash = HashString(hash, value.channelId);
			return HashInteger(hash, static_cast<std::uint8_t>(value.preserveFocus));
		} else {
			static_assert(std::is_same_v<Value, OutputDisposeOwnerRequest>);
			hash = HashInteger(hash, std::uint8_t{ 6 });
			hash = HashOperation(hash, value.operation);
			return HashOwner(hash, value.owner);
		}
	}, request);
}

std::uint64_t HashTrace(const TraceDefinition& trace)
{
	std::uint64_t hash = HashInteger(1469598103934665603ULL, trace.seed);
	hash = HashOwner(hash, trace.owner);
	hash = HashOwner(hash, trace.replacementOwner);
	hash = HashString(hash, trace.outputChannelId);
	hash = HashString(hash, trace.logChannelId);
	hash = HashString(hash, trace.shortText);
	hash = HashString(hash, trace.longText);
	hash = HashString(hash, trace.utf8BoundaryText);
	hash = HashString(hash, trace.replacementText);
	hash = HashString(hash, trace.oversizedText);
	hash = HashInteger(hash, trace.logEntries.size());
	for (const auto& entry : trace.logEntries) {
		hash = HashInteger(hash, static_cast<std::uint8_t>(entry.level));
		hash = HashString(hash, entry.message);
		if (entry.source) hash = HashString(hash, *entry.source);
	}
	for (const auto& step : trace.steps) {
		hash = HashInteger(hash, static_cast<std::uint8_t>(step.kind));
		hash = HashTraceRequest(hash, step.request);
	}
	return hash;
}

OutputOperation Operation(std::string operationId, const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operationId = std::move(operationId), .expectedRevision = expectedRevision };
}

OutputCreateChannelRequest CreateRequest(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const EOutputChannelKind kind,
	std::string label)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.label = std::move(label),
		.kind = kind,
		.metadata = { .languageId = std::string("plaintext"), .source = std::string("issue-274") },
	};
}

OutputTextMutationRequest TextRequest(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	std::string text,
	const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return {
		.operation = Operation(std::move(operationId), expectedRevision),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.text = std::move(text),
	};
}

OutputLogMutationRequest LogRequest(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	std::vector<OutputLogEntry> entries)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.entries = std::move(entries),
	};
}

OutputChannelMutationRequest ChannelRequest(
	std::string operationId,
	OutputOwner owner,
	std::string channelId)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
	};
}

OutputShowChannelRequest ShowRequest(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const bool preserveFocus)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.preserveFocus = preserveFocus,
	};
}

OutputDisposeOwnerRequest DisposeOwnerRequest(std::string operationId, OutputOwner owner)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner) };
}

std::string MakeLongText(const std::uint64_t seed)
{
	constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
	std::string result;
	result.reserve(640);
	std::uint64_t state = seed ^ 0x9e3779b97f4a7c15ULL;
	for (std::size_t index = 0; index < 640; ++index) {
		state ^= state << 7U;
		state ^= state >> 9U;
		result.push_back(alphabet[static_cast<std::size_t>(state % alphabet.size())]);
	}
	return result;
}

TraceDefinition MakeTrace(const std::uint64_t seed)
{
	TraceDefinition trace;
	trace.seed = seed;
	trace.owner = { .ownerId = Token("bench.owner", seed, 1), .generation = 1 };
	trace.replacementOwner = { .ownerId = trace.owner.ownerId, .generation = 2 };
	trace.outputChannelId = Token("bench.output", seed, 1);
	trace.logChannelId = Token("bench.log", seed, 1);
	trace.shortText = "short." + Hex(seed);
	trace.longText = MakeLongText(seed);
	trace.utf8BoundaryText = "utf8.boundary.";
	trace.utf8BoundaryText.append("\xe3\x81\x82", 3);
	trace.utf8BoundaryText.append("\xf0\x9f\x98\x80", 4);
	trace.utf8BoundaryText.append("." + Hex(seed));
	trace.replacementText = "replacement." + Hex(seed);
	trace.oversizedText.assign(4'200, 'x');
	trace.logEntries = {
		{ .level = EOutputLogLevel::Info, .message = "info." + Hex(seed), .source = std::string("issue-274") },
		{ .level = EOutputLogLevel::Warning, .message = "warning." + Hex(seed), .source = std::nullopt },
		{ .level = EOutputLogLevel::Error, .message = "error." + Hex(seed), .source = std::string("issue-274") },
	};

	const auto operation = [&trace](std::string_view suffix, const std::uint64_t index) {
		return Token(std::string("bench.op.") + std::string(suffix), trace.seed, index);
	};
	trace.steps = {
		{ ETraceKind::CreateOutput, CreateRequest(operation("create-output", 1), trace.owner,
			trace.outputChannelId, EOutputChannelKind::Output, "Bench Output") },
		{ ETraceKind::CreateLog, CreateRequest(operation("create-log", 2), trace.owner,
			trace.logChannelId, EOutputChannelKind::Log, "Bench Log") },
		{ ETraceKind::AppendShort, TextRequest(operation("append-short", 3), trace.owner,
			trace.outputChannelId, trace.shortText) },
		{ ETraceKind::AppendLong, TextRequest(operation("append-long", 4), trace.owner,
			trace.outputChannelId, trace.longText) },
		{ ETraceKind::AppendUtf8Boundary, TextRequest(operation("append-utf8", 5), trace.owner,
			trace.outputChannelId, trace.utf8BoundaryText) },
		{ ETraceKind::ReplaceOutput, TextRequest(operation("replace-output", 6), trace.owner,
			trace.outputChannelId, trace.replacementText) },
		{ ETraceKind::AppendLog, LogRequest(operation("append-log", 7), trace.owner,
			trace.logChannelId, trace.logEntries) },
		{ ETraceKind::ClearLog, ChannelRequest(operation("clear-log", 8), trace.owner,
			trace.logChannelId) },
		{ ETraceKind::ShowOutput, ShowRequest(operation("show-output", 9), trace.owner,
			trace.outputChannelId, true) },
		{ ETraceKind::HideOutput, ChannelRequest(operation("hide-output", 10), trace.owner,
			trace.outputChannelId) },
		{ ETraceKind::CreateReplacement, CreateRequest(operation("create-replacement", 11), trace.replacementOwner,
			trace.outputChannelId, EOutputChannelKind::Output, "Replacement Output") },
		{ ETraceKind::OldOwnerFenced, TextRequest(operation("old-owner-fenced", 12), trace.owner,
			trace.outputChannelId, "old-owner") },
		{ ETraceKind::NotApplicableEmpty, TextRequest(operation("not-applicable-empty", 13), trace.replacementOwner,
			trace.outputChannelId, "") },
		{ ETraceKind::DisposeOutput, ChannelRequest(operation("dispose-output", 14), trace.replacementOwner,
			trace.outputChannelId) },
		{ ETraceKind::DisposeOwner, DisposeOwnerRequest(operation("dispose-owner", 15), trace.replacementOwner) },
		{ ETraceKind::ReplayCreateOutput, CreateRequest(operation("create-output", 1), trace.owner,
			trace.outputChannelId, EOutputChannelKind::Output, "Bench Output") },
		{ ETraceKind::OperationIdConflict, CreateRequest(operation("create-output", 1), trace.owner,
			trace.outputChannelId, EOutputChannelKind::Output, "Conflict Label") },
		{ ETraceKind::StaleRevision, TextRequest(operation("stale-revision", 16), trace.replacementOwner,
			trace.outputChannelId, "stale", 1) },
		{ ETraceKind::RejectedOwner, TextRequest(operation("rejected-owner", 17), {},
			trace.outputChannelId, "rejected") },
		{ ETraceKind::RejectedPayload, TextRequest(operation("rejected-payload", 18), trace.replacementOwner,
			trace.outputChannelId, trace.oversizedText) },
		{ ETraceKind::Stop, OutputDisposeOwnerRequest{ .operation = Operation(operation("stop", 19)), .owner = trace.replacementOwner } },
		{ ETraceKind::RepeatedStop, OutputDisposeOwnerRequest{ .operation = Operation(operation("repeated-stop", 20)), .owner = trace.replacementOwner } },
		{ ETraceKind::PostStopAppend, TextRequest(operation("post-stop", 21), trace.replacementOwner,
			trace.outputChannelId, "post-stop") },
	};
	return trace;
}

OutputServiceLimits MeasurementLimits()
{
	return {
		.maximumOwners = 8,
		.maximumChannels = 8,
		.maximumTextBytesPerChannel = 128,
		.maximumPayloadBytes = 4'096,
		.maximumLogEntriesPerChannel = 4,
		.maximumSubscriptions = 8,
		.maximumRememberedOperations = 64,
		.maximumPendingNotifications = 8,
		.maximumAcceptedCommitFeedEntries = 8,
	};
}

std::unique_ptr<IOutputService> CreateSelectedProvider(const OutputServiceLimits& limits)
{
	auto result = CreateOutputProvider({ .kind = DefaultOutputProviderKind(), .limits = limits });
	if (!result.Succeeded()) {
		throw std::runtime_error("compile-selected Output provider factory failed");
	}
	const auto expectedKind = DefaultOutputProviderKind();
	const auto& factoryHealth = result.health;
	if (result.kind != expectedKind
		|| factoryHealth.kind != expectedKind
		|| factoryHealth.factoryStatus != EOutputProviderFactoryStatus::Created
		|| factoryHealth.initializationStage != EOutputProviderInitializationStage::Ready
		|| factoryHealth.lifecycle != EOutputProviderLifecycle::Ready
		|| factoryHealth.fault != EOutputProviderFault::None
		|| !factoryHealth.compiledIn
		|| !factoryHealth.available
		|| factoryHealth.testOverrideActive) {
		throw std::runtime_error("Output provider factory health disagrees with the compile-selected provider");
	}
	if (!result.provider) throw std::runtime_error("Output provider factory returned no provider");
	const auto providerHealth = result.provider->Health();
	if (providerHealth.kind != expectedKind
		|| providerHealth.factoryStatus != EOutputProviderFactoryStatus::Created
		|| providerHealth.initializationStage != EOutputProviderInitializationStage::Ready
		|| providerHealth.lifecycle != EOutputProviderLifecycle::Ready
		|| providerHealth.fault != EOutputProviderFault::None
		|| !providerHealth.compiledIn
		|| !providerHealth.available
		|| providerHealth.testOverrideActive) {
		throw std::runtime_error("Output provider health disagrees with factory health");
	}
	return std::move(result.provider);
}

const char* ProviderName(const EOutputProviderKind kind) noexcept
{
	return kind == EOutputProviderKind::Rust ? "rust" : "cpp";
}

std::uint64_t ReadQpc()
{
	LARGE_INTEGER value{};
	if (::QueryPerformanceCounter(&value) == 0 || value.QuadPart < 0) {
		throw std::runtime_error("QueryPerformanceCounter failed");
	}
	return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t ReadQpcFrequency()
{
	LARGE_INTEGER value{};
	if (::QueryPerformanceFrequency(&value) == 0 || value.QuadPart <= 0) {
		throw std::runtime_error("QueryPerformanceFrequency failed");
	}
	return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t ApplyProcessAffinity(const std::uint64_t requestedMask)
{
	if (requestedMask == 0 || requestedMask > static_cast<std::uint64_t>(std::numeric_limits<DWORD_PTR>::max())) {
		throw std::runtime_error("SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK is unsupported");
	}
	const auto requested = static_cast<DWORD_PTR>(requestedMask);
	DWORD_PTR processMask{};
	DWORD_PTR systemMask{};
	if (::GetProcessAffinityMask(::GetCurrentProcess(), &processMask, &systemMask) == FALSE) {
		throw std::runtime_error("GetProcessAffinityMask failed");
	}
	if ((requested & ~systemMask) != 0) {
		throw std::runtime_error("SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK is not supported by this process");
	}
	if (::SetProcessAffinityMask(::GetCurrentProcess(), requested) == FALSE) {
		throw std::runtime_error("SetProcessAffinityMask failed");
	}
	DWORD_PTR readBackMask{};
	DWORD_PTR readBackSystemMask{};
	if (::GetProcessAffinityMask(::GetCurrentProcess(), &readBackMask, &readBackSystemMask) == FALSE
		|| readBackMask != requested) {
		throw std::runtime_error("process affinity mask read-back did not match the requested mask");
	}
	return static_cast<std::uint64_t>(readBackMask);
}

std::uint64_t HashSnapshot(const OutputServiceSnapshot& snapshot)
{
	std::uint64_t hash = HashInteger(1469598103934665603ULL, snapshot.revision);
	hash = HashInteger(hash, static_cast<std::uint8_t>(snapshot.stopped));
	hash = HashInteger(hash, snapshot.droppedNotificationCount);
	if (snapshot.activeChannelId) {
		hash = HashInteger(hash, std::uint8_t{ 1 });
		hash = HashString(hash, *snapshot.activeChannelId);
	} else {
		hash = HashInteger(hash, std::uint8_t{ 0 });
	}
	hash = HashInteger(hash, snapshot.channels.size());
	for (const auto& channel : snapshot.channels) {
		hash = HashString(hash, channel.channelId);
		hash = HashString(hash, channel.label);
		hash = HashOwner(hash, channel.owner);
		hash = HashInteger(hash, static_cast<std::uint8_t>(channel.kind));
		hash = HashMetadata(hash, channel.metadata);
		hash = HashInteger(hash, static_cast<std::uint8_t>(channel.visible));
		hash = HashInteger(hash, static_cast<std::uint8_t>(channel.lastShowPreservedFocus));
		hash = HashInteger(hash, channel.droppedCharacterCount);
		hash = HashString(hash, channel.text);
		hash = HashInteger(hash, channel.logEntries.size());
		for (const auto& entry : channel.logEntries) {
			hash = HashInteger(hash, static_cast<std::uint8_t>(entry.level));
			hash = HashString(hash, entry.message);
			if (entry.source) hash = HashString(hash, *entry.source);
		}
		hash = HashString(hash, channel.projectedText);
	}
	return hash;
}

std::uint64_t HashResult(const OutputOperationResult& result)
{
	std::uint64_t hash = HashInteger(1469598103934665603ULL,
		static_cast<std::uint8_t>(result.status));
	hash = HashInteger(hash, static_cast<std::uint8_t>(result.reason));
	hash = HashInteger(hash, result.revision);
	return HashInteger(hash, static_cast<std::uint8_t>(result.callbackDrainDeferred));
}

std::size_t StatusIndex(const EOutputOperationStatus status)
{
	return static_cast<std::size_t>(status);
}

void RecordResult(TraceOutcome& outcome, const OutputOperationResult& result)
{
	const auto prior = outcome.resultDigest == 0 ? 1469598103934665603ULL : outcome.resultDigest;
	outcome.resultDigest = HashResult(result) ^ (prior * 1099511628211ULL);
	const auto index = StatusIndex(result.status);
	if (index < outcome.statusCounts.size()) ++outcome.statusCounts[index];
}

OutputOperationResult ExecuteStep(IOutputService& service, const TraceStep& step)
{
	return std::visit([&service, &step](const auto&) {
		switch (step.kind) {
		case ETraceKind::CreateOutput:
		case ETraceKind::CreateLog:
		case ETraceKind::CreateReplacement:
		case ETraceKind::ReplayCreateOutput:
		case ETraceKind::OperationIdConflict:
			return service.CreateChannel(std::get<OutputCreateChannelRequest>(step.request));
		case ETraceKind::AppendShort:
		case ETraceKind::AppendLong:
		case ETraceKind::AppendUtf8Boundary:
		case ETraceKind::OldOwnerFenced:
		case ETraceKind::NotApplicableEmpty:
		case ETraceKind::StaleRevision:
		case ETraceKind::RejectedOwner:
		case ETraceKind::RejectedPayload:
		case ETraceKind::PostStopAppend:
			return service.AppendOutput(std::get<OutputTextMutationRequest>(step.request));
		case ETraceKind::ReplaceOutput:
			return service.ReplaceOutput(std::get<OutputTextMutationRequest>(step.request));
		case ETraceKind::AppendLog:
			return service.AppendLog(std::get<OutputLogMutationRequest>(step.request));
		case ETraceKind::ClearLog:
			return service.Clear(std::get<OutputChannelMutationRequest>(step.request));
		case ETraceKind::ShowOutput:
			return service.Show(std::get<OutputShowChannelRequest>(step.request));
		case ETraceKind::HideOutput:
		case ETraceKind::DisposeOutput:
			return step.kind == ETraceKind::HideOutput
				? service.Hide(std::get<OutputChannelMutationRequest>(step.request))
				: service.Dispose(std::get<OutputChannelMutationRequest>(step.request));
		case ETraceKind::DisposeOwner:
			return service.DisposeOwner(std::get<OutputDisposeOwnerRequest>(step.request));
		case ETraceKind::Stop:
		case ETraceKind::RepeatedStop:
			return service.Stop();
		}
		throw std::runtime_error("unknown benchmark trace step");
	}, step.request);
}

OutputServiceSnapshot PrepareSnapshotState(IOutputService& service, const TraceDefinition& trace)
{
	for (const auto& step : trace.steps) {
		if (step.kind == ETraceKind::CreateReplacement) break;
		if (step.kind == ETraceKind::Stop) break;
		(void)ExecuteStep(service, step);
	}
	return service.Snapshot();
}

struct NormalLifecycle final {
	std::unique_ptr<IOutputService> service;
	TraceOutcome outcome;
	std::uint64_t callbackDepth{};
	std::optional<OutputServiceSubscriptionId> throwingSubscription;
	std::optional<OutputServiceSubscriptionId> recordingSubscription;
};

void PrepareNormalLifecycle(NormalLifecycle& lifecycle)
{
	lifecycle.service = CreateSelectedProvider(MeasurementLimits());
	lifecycle.throwingSubscription = lifecycle.service->Subscribe([](const OutputServiceChange&) {
		throw std::runtime_error("benchmark listener exception");
	});
	if (!lifecycle.throwingSubscription) throw std::runtime_error("throwing listener subscription failed");
	lifecycle.recordingSubscription = lifecycle.service->Subscribe([&lifecycle](const OutputServiceChange&) {
		++lifecycle.callbackDepth;
		++lifecycle.outcome.callbackCount;
		lifecycle.outcome.maximumCallbackDepth = std::max(
			lifecycle.outcome.maximumCallbackDepth, lifecycle.callbackDepth);
		--lifecycle.callbackDepth;
	});
	if (!lifecycle.recordingSubscription) {
		lifecycle.service->Unsubscribe(*lifecycle.throwingSubscription);
		throw std::runtime_error("recording listener subscription failed");
	}
}

void ExecuteNormalLifecycle(NormalLifecycle& lifecycle, const TraceDefinition& trace)
{
	for (const auto& step : trace.steps) {
		const auto result = ExecuteStep(*lifecycle.service, step);
		RecordResult(lifecycle.outcome, result);
	}
}

void FinalizeNormalLifecycle(NormalLifecycle& lifecycle)
{
	lifecycle.outcome.snapshotDigest = HashSnapshot(lifecycle.service->Snapshot());
	lifecycle.outcome.droppedNotificationCount = lifecycle.service->Snapshot().droppedNotificationCount;
	const auto finalSnapshot = lifecycle.service->Snapshot();
	if (!finalSnapshot.stopped) throw std::runtime_error("normal workload did not stop the provider");
	if (lifecycle.outcome.callbackCount == 0 || lifecycle.outcome.maximumCallbackDepth != 1) {
		throw std::runtime_error("normal workload listener accounting failed");
	}
	if (lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::Succeeded)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::Replayed)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::NotApplicable)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::Rejected)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::Conflict)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::StaleRevision)] == 0
		|| lifecycle.outcome.statusCounts[StatusIndex(EOutputOperationStatus::Stopped)] == 0) {
		throw std::runtime_error("normal workload did not cover every result status");
	}
	lifecycle.service->Unsubscribe(*lifecycle.recordingSubscription);
	lifecycle.service->Unsubscribe(*lifecycle.throwingSubscription);
}

void MergeOutcome(TraceOutcome& total, const TraceOutcome& part)
{
	const auto priorResult = total.resultDigest == 0 ? 1469598103934665603ULL : total.resultDigest;
	total.resultDigest = part.resultDigest ^ (priorResult * 1099511628211ULL);
	const auto priorSnapshot = total.snapshotDigest == 0 ? 1469598103934665603ULL : total.snapshotDigest;
	total.snapshotDigest = part.snapshotDigest ^ (priorSnapshot * 1099511628211ULL);
	for (std::size_t index = 0; index < total.statusCounts.size(); ++index) {
		total.statusCounts[index] += part.statusCounts[index];
	}
	total.callbackCount += part.callbackCount;
	total.maximumCallbackDepth = std::max(total.maximumCallbackDepth, part.maximumCallbackDepth);
	total.droppedNotificationCount += part.droppedNotificationCount;
}

QpcSample MeasureNormalBlock(
	const TraceDefinition& trace,
	const std::size_t lifecycleIterations,
	TraceOutcome& outcome)
{
	outcome = {};
	std::vector<NormalLifecycle> lifecycles;
	lifecycles.reserve(lifecycleIterations);
	for (std::size_t index = 0; index < lifecycleIterations; ++index) {
		lifecycles.emplace_back();
		PrepareNormalLifecycle(lifecycles.back());
	}
	QpcSample sample{ .frequency = ReadQpcFrequency() };
	sample.begin = ReadQpc();
	for (auto& lifecycle : lifecycles) {
		ExecuteNormalLifecycle(lifecycle, trace);
	}
	sample.end = ReadQpc();
	for (auto& lifecycle : lifecycles) {
		FinalizeNormalLifecycle(lifecycle);
		MergeOutcome(outcome, lifecycle.outcome);
	}
	return sample;
}

QpcSample MeasureSnapshotBlock(
	IOutputService& service,
	const TraceDefinition& trace,
	const std::size_t iterations,
	TraceOutcome& outcome)
{
	outcome = {};
	(void)PrepareSnapshotState(service, trace);
	QpcSample sample{ .frequency = ReadQpcFrequency() };
	sample.begin = ReadQpc();
	std::uint64_t digest = 1469598103934665603ULL;
	for (std::size_t index = 0; index < iterations; ++index) {
		const auto snapshot = service.Snapshot();
		digest = HashSnapshot(snapshot) ^ (digest * 1099511628211ULL);
	}
	sample.end = ReadQpc();
	outcome.snapshotDigest = digest;
	outcome.droppedNotificationCount = service.Snapshot().droppedNotificationCount;
	return sample;
}

struct DropScenario final {
	QpcSample sample;
	TraceOutcome outcome;
};

DropScenario MeasureDropBlock(const TraceDefinition& trace)
{
	OutputServiceLimits limits = MeasurementLimits();
	limits.maximumPendingNotifications = 1;
	const auto service = CreateSelectedProvider(limits);
	TraceOutcome outcome{};
	std::mutex gateMutex;
	std::condition_variable gateCondition;
	bool callbackEntered{};
	bool releaseCallback{};
	std::uint64_t blockerCallbacks{};

	const auto blocker = service->Subscribe([&](const OutputServiceChange&) {
		std::unique_lock lock(gateMutex);
		++blockerCallbacks;
		if (blockerCallbacks == 1) {
			callbackEntered = true;
			gateCondition.notify_all();
			gateCondition.wait(lock, [&releaseCallback] { return releaseCallback; });
		}
	});
	if (!blocker) throw std::runtime_error("drop blocker subscription failed");
	std::uint64_t removedCallbacks{};
	const auto toUnsubscribe = service->Subscribe([&removedCallbacks](const OutputServiceChange&) {
		++removedCallbacks;
	});
	if (!toUnsubscribe) throw std::runtime_error("drop unsubscribe subscription failed");

	OutputOperationResult createResult{};
	std::thread creator([&service, &trace, &createResult] {
		createResult = service->CreateChannel(std::get<OutputCreateChannelRequest>(trace.steps.front().request));
	});
	{
		std::unique_lock lock(gateMutex);
		if (!gateCondition.wait_for(lock, kCallbackWaitTimeout, [&callbackEntered] { return callbackEntered; })) {
			releaseCallback = true;
			lock.unlock();
			gateCondition.notify_all();
			creator.join();
			service->Unsubscribe(*blocker);
			service->Unsubscribe(*toUnsubscribe);
			(void)service->Stop();
			throw std::runtime_error("drop blocker callback did not start");
		}
	}

	service->Unsubscribe(*toUnsubscribe);
	QpcSample sample{ .frequency = ReadQpcFrequency() };
	sample.begin = ReadQpc();
	const auto append = service->AppendOutput(TextRequest(
		Token("bench.drop.append", trace.seed, 1), trace.owner,
		trace.outputChannelId, "drop-append"));
	const auto replace = service->ReplaceOutput(TextRequest(
		Token("bench.drop.replace", trace.seed, 2), trace.owner,
		trace.outputChannelId, "drop-replace"));
	sample.end = ReadQpc();
	{
		std::lock_guard lock(gateMutex);
		releaseCallback = true;
	}
	gateCondition.notify_all();
	creator.join();
	const auto stop = service->Stop();
	RecordResult(outcome, createResult);
	RecordResult(outcome, append);
	RecordResult(outcome, replace);
	RecordResult(outcome, stop);
	outcome.callbackCount = blockerCallbacks;
	outcome.maximumCallbackDepth = 1;
	outcome.snapshotDigest = HashSnapshot(service->Snapshot());
	outcome.droppedNotificationCount = service->Snapshot().droppedNotificationCount;
	if (removedCallbacks != 0) throw std::runtime_error("unsubscribed listener was invoked");
	if (outcome.droppedNotificationCount == 0) throw std::runtime_error("drop workload did not drop a notification");
	return { .sample = sample, .outcome = outcome };
}

struct CallbackStopScenario final {
	QpcSample sample;
	TraceOutcome outcome;
};

CallbackStopScenario MeasureCallbackStopBlock(const TraceDefinition& trace)
{
	const auto service = CreateSelectedProvider(MeasurementLimits());
	TraceOutcome outcome{};
	std::mutex resultMutex;
	std::optional<OutputOperationResult> callbackStop;
	std::optional<OutputOperationResult> repeatedCallbackStop;
	std::uint64_t callbackCount{};
	const auto subscription = service->Subscribe([&](const OutputServiceChange&) {
		std::lock_guard lock(resultMutex);
		++callbackCount;
		if (callbackCount == 1) {
			callbackStop = service->Stop();
			repeatedCallbackStop = service->Stop();
		}
	});
	if (!subscription) throw std::runtime_error("callback Stop subscription failed");

	const auto create = std::get<OutputCreateChannelRequest>(trace.steps.front().request);
	QpcSample sample{ .frequency = ReadQpcFrequency() };
	sample.begin = ReadQpc();
	const auto createResult = service->CreateChannel(create);
	sample.end = ReadQpc();
	const auto externalRetry = service->Stop();
	RecordResult(outcome, createResult);
	if (!callbackStop || !repeatedCallbackStop) throw std::runtime_error("callback Stop was not observed");
	RecordResult(outcome, *callbackStop);
	RecordResult(outcome, *repeatedCallbackStop);
	RecordResult(outcome, externalRetry);
	outcome.callbackCount = callbackCount;
	outcome.maximumCallbackDepth = 1;
	outcome.snapshotDigest = HashSnapshot(service->Snapshot());
	outcome.droppedNotificationCount = service->Snapshot().droppedNotificationCount;
	if (!callbackStop->callbackDrainDeferred || !repeatedCallbackStop->callbackDrainDeferred
		|| externalRetry.callbackDrainDeferred) {
		throw std::runtime_error("callback Stop drain contract was not observed");
	}
	return { .sample = sample, .outcome = outcome };
}

std::string StatusFields(const TraceOutcome& outcome)
{
	std::ostringstream stream;
	stream << "\"succeeded\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::Succeeded)]
		<< ",\"replayed\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::Replayed)]
		<< ",\"notApplicable\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::NotApplicable)]
		<< ",\"rejected\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::Rejected)]
		<< ",\"conflict\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::Conflict)]
		<< ",\"staleRevision\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::StaleRevision)]
		<< ",\"revisionExhausted\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::RevisionExhausted)]
		<< ",\"stopped\":" << outcome.statusCounts[StatusIndex(EOutputOperationStatus::Stopped)];
	return stream.str();
}

void WriteMetadata(
	JsonLinesWriter& writer,
	const BenchmarkSettings& settings,
	const TraceDefinition& trace,
	const std::uint64_t frequency)
{
	std::ostringstream stream;
	stream << "{\"schemaVersion\":" << kSchemaVersion
		<< ",\"record\":\"metadata\""
		<< ",\"provider\":\"" << JsonEscape(ProviderName(DefaultOutputProviderKind())) << "\""
		<< ",\"configuration\":\"" << JsonEscape(settings.configuration) << "\""
		<< ",\"seed\":" << trace.seed
		<< ",\"affinityMask\":" << settings.affinityMask
		<< ",\"traceDigest\":" << HashTrace(trace)
		<< ",\"traceOperationCount\":" << trace.steps.size()
		<< ",\"warmupBlocks\":" << settings.warmupBlocks
		<< ",\"measuredBlocks\":" << settings.measuredBlocks
		<< ",\"snapshotIterations\":" << settings.snapshotIterations
		<< ",\"lifecycleIterations\":" << settings.lifecycleIterations
		<< ",\"qpcFrequency\":" << frequency
		<< ",\"payloadFree\":true"
		<< "}";
	writer.Write(stream.str());
}

void WriteSample(
	JsonLinesWriter& writer,
	const BenchmarkSettings& settings,
	const TraceDefinition& trace,
	const std::size_t blockIndex,
	const bool warmup,
	const char* block,
	const std::size_t operations,
	const QpcSample& sample,
	const TraceOutcome& outcome)
{
	std::ostringstream stream;
	stream << "{\"schemaVersion\":" << kSchemaVersion
		<< ",\"record\":\"sample\""
		<< ",\"provider\":\"" << JsonEscape(ProviderName(DefaultOutputProviderKind())) << "\""
		<< ",\"configuration\":\"" << JsonEscape(settings.configuration) << "\""
		<< ",\"seed\":" << settings.seed
		<< ",\"affinityMask\":" << settings.affinityMask
		<< ",\"traceDigest\":" << HashTrace(trace)
		<< ",\"block\":\"" << block << "\""
		<< ",\"warmup\":" << (warmup ? "true" : "false")
		<< ",\"blockIndex\":" << blockIndex
		<< ",\"beginQpc\":" << sample.begin
		<< ",\"endQpc\":" << sample.end
		<< ",\"qpcFrequency\":" << sample.frequency
		<< ",\"operations\":" << operations
		<< ",\"resultDigest\":" << outcome.resultDigest
		<< ",\"snapshotDigest\":" << outcome.snapshotDigest
		<< "," << StatusFields(outcome)
		<< ",\"callbacks\":" << outcome.callbackCount
		<< ",\"maximumCallbackDepth\":" << outcome.maximumCallbackDepth
		<< ",\"droppedNotificationCount\":" << outcome.droppedNotificationCount
		<< ",\"setupExcluded\":true"
		<< "}";
	writer.Write(stream.str());
}

void WriteSummary(
	JsonLinesWriter& writer,
	const BenchmarkSettings& settings,
	const TraceDefinition& trace,
	const std::size_t sampleCount)
{
	std::ostringstream stream;
	stream << "{\"schemaVersion\":" << kSchemaVersion
		<< ",\"record\":\"summary\""
		<< ",\"provider\":\"" << JsonEscape(ProviderName(DefaultOutputProviderKind())) << "\""
		<< ",\"configuration\":\"" << JsonEscape(settings.configuration) << "\""
		<< ",\"seed\":" << settings.seed
		<< ",\"affinityMask\":" << settings.affinityMask
		<< ",\"traceDigest\":" << HashTrace(trace)
		<< ",\"sampleCount\":" << sampleCount
		<< ",\"completed\":true"
		<< ",\"payloadFree\":true"
		<< "}";
	writer.Write(stream.str());
}

BenchmarkSettings ReadSettings()
{
	BenchmarkSettings settings;
	const auto output = GetEnvironment("SAKURA_OUTPUT_BENCHMARK_OUTPUT");
	if (output.empty()) throw std::runtime_error("SAKURA_OUTPUT_BENCHMARK_OUTPUT is required");
	settings.outputPath = std::filesystem::path(output);
	settings.seed = ParseUnsigned(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_SEED"),
		"SAKURA_OUTPUT_BENCHMARK_SEED", kDefaultSeed);
	settings.affinityMask = ParseUnsigned(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK"),
		"SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK", 1);
	if (settings.affinityMask == 0) {
		throw std::runtime_error("SAKURA_OUTPUT_BENCHMARK_AFFINITY_MASK must be nonzero");
	}
	settings.warmupBlocks = ParseBoundedSize(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_WARMUP_BLOCKS"),
		"SAKURA_OUTPUT_BENCHMARK_WARMUP_BLOCKS", settings.warmupBlocks, kMaximumWarmupBlocks);
	settings.measuredBlocks = ParseBoundedSize(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_BLOCKS"),
		"SAKURA_OUTPUT_BENCHMARK_BLOCKS", settings.measuredBlocks, kMaximumBlocks);
	settings.snapshotIterations = ParseBoundedSize(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_SNAPSHOT_ITERATIONS"),
		"SAKURA_OUTPUT_BENCHMARK_SNAPSHOT_ITERATIONS", settings.snapshotIterations, kMaximumSnapshotIterations);
	settings.lifecycleIterations = ParseBoundedSize(GetEnvironment("SAKURA_OUTPUT_BENCHMARK_LIFECYCLES"),
		"SAKURA_OUTPUT_BENCHMARK_LIFECYCLES", settings.lifecycleIterations, kMaximumLifecycleIterations);
	settings.configuration = GetEnvironment("SAKURA_OUTPUT_BENCHMARK_CONFIGURATION");
	const auto compiledConfiguration = std::string(CompiledConfiguration());
	if (!settings.configuration.empty() && settings.configuration != compiledConfiguration) {
		throw std::runtime_error("SAKURA_OUTPUT_BENCHMARK_CONFIGURATION does not match the compiled configuration");
	}
	settings.configuration = compiledConfiguration;
	const auto expectedProvider = GetEnvironment("SAKURA_OUTPUT_BENCHMARK_EXPECTED_PROVIDER");
	if (!expectedProvider.empty()) settings.expectedProvider = expectedProvider;
	return settings;
}

void ValidateProviderExpectation(const BenchmarkSettings& settings)
{
	if (settings.expectedProvider && *settings.expectedProvider != ProviderName(DefaultOutputProviderKind())) {
		throw std::runtime_error("benchmark executable provider does not match expected provider");
	}
}

void RunMeasurement()
{
	auto settings = ReadSettings();
	settings.affinityMask = ApplyProcessAffinity(settings.affinityMask);
	ValidateProviderExpectation(settings);
	const auto trace = MakeTrace(settings.seed);
	const auto parent = settings.outputPath.parent_path();
	if (!parent.empty()) std::filesystem::create_directories(parent);
	JsonLinesWriter writer(settings.outputPath);
	const auto frequency = ReadQpcFrequency();
	WriteMetadata(writer, settings, trace, frequency);

	std::size_t sampleCount{};
	for (std::size_t blockIndex = 0; blockIndex < settings.warmupBlocks + settings.measuredBlocks; ++blockIndex) {
		const bool warmup = blockIndex < settings.warmupBlocks;
		TraceOutcome outcome{};
		const auto sample = MeasureNormalBlock(trace, settings.lifecycleIterations, outcome);
		WriteSample(writer, settings, trace, blockIndex, warmup, "mutations",
			trace.steps.size() * settings.lifecycleIterations, sample, outcome);
		++sampleCount;

		const auto snapshotService = CreateSelectedProvider(MeasurementLimits());
		const auto snapshotOutcome = [&]() {
			TraceOutcome value{};
			const auto snapshotSample = MeasureSnapshotBlock(*snapshotService, trace, settings.snapshotIterations, value);
			WriteSample(writer, settings, trace, blockIndex, warmup, "snapshots",
				settings.snapshotIterations, snapshotSample, value);
			return value;
		}();
		(void)snapshotOutcome;
		++sampleCount;

		const auto drop = MeasureDropBlock(trace);
		WriteSample(writer, settings, trace, blockIndex, warmup, "advisory-drop", 2, drop.sample, drop.outcome);
		++sampleCount;

		const auto callbackStop = MeasureCallbackStopBlock(trace);
		WriteSample(writer, settings, trace, blockIndex, warmup, "callback-stop", 1, callbackStop.sample, callbackStop.outcome);
		++sampleCount;
	}
	WriteSummary(writer, settings, trace, sampleCount);
}

std::vector<double> ToDoubles(std::initializer_list<double> values)
{
	return std::vector<double>(values);
}

double PercentileUpper(std::vector<double> values, const double percentile)
{
	if (values.empty() || percentile < 0.0 || percentile > 100.0) {
		throw std::runtime_error("invalid percentile input");
	}
	std::sort(values.begin(), values.end());
	const auto rank = static_cast<std::size_t>(std::ceil((percentile / 100.0) * values.size()));
	const auto index = rank == 0 ? 0U : std::min(rank - 1U, values.size() - 1U);
	return values[index];
}

MeasurementStatistics CalculateStatistics(std::vector<double> values)
{
	if (values.empty()) throw std::runtime_error("statistics require at least one sample");
	std::sort(values.begin(), values.end());
	const auto middle = values.size() / 2U;
	const double median = values.size() % 2U == 0U
		? (values[middle - 1U] + values[middle]) / 2.0
		: values[middle];
	return { .count = values.size(), .median = median, .p95 = PercentileUpper(values, 95.0) };
}

TEST(OutputServiceProviderMeasurement, SeededTraceIsDeterministic)
{
	const auto first = MakeTrace(kDefaultSeed);
	const auto second = MakeTrace(kDefaultSeed);
	const auto different = MakeTrace(kDefaultSeed + 1U);
	EXPECT_EQ(HashTrace(first), HashTrace(second));
	EXPECT_NE(HashTrace(first), HashTrace(different));
	EXPECT_EQ(first.steps.size(), second.steps.size());
	const auto containsKind = [&first](const ETraceKind kind) {
		return std::any_of(first.steps.cbegin(), first.steps.cend(), [kind](const TraceStep& step) {
			return step.kind == kind;
		});
	};
	for (const auto kind : {
		ETraceKind::CreateOutput, ETraceKind::CreateLog, ETraceKind::AppendShort,
		ETraceKind::AppendLong, ETraceKind::AppendUtf8Boundary, ETraceKind::ReplaceOutput,
		ETraceKind::AppendLog, ETraceKind::ClearLog, ETraceKind::ShowOutput,
		ETraceKind::HideOutput, ETraceKind::CreateReplacement, ETraceKind::OldOwnerFenced,
		ETraceKind::NotApplicableEmpty, ETraceKind::DisposeOutput, ETraceKind::DisposeOwner,
		ETraceKind::ReplayCreateOutput, ETraceKind::OperationIdConflict, ETraceKind::StaleRevision,
		ETraceKind::RejectedOwner, ETraceKind::RejectedPayload, ETraceKind::Stop,
		ETraceKind::RepeatedStop, ETraceKind::PostStopAppend }) {
		EXPECT_TRUE(containsKind(kind)) << "trace is missing kind " << static_cast<int>(kind);
	}
	EXPECT_EQ(640U, first.longText.size());
	EXPECT_EQ(4200U, first.oversizedText.size());
	EXPECT_EQ(3U, first.logEntries.size());
}

TEST(OutputServiceProviderMeasurement, StatisticsUseStableMedianAndUpperPercentile)
{
	const auto values = ToDoubles({ 4.0, 1.0, 9.0, 3.0, 2.0 });
	const auto statistics = CalculateStatistics(values);
	EXPECT_EQ(5U, statistics.count);
	EXPECT_DOUBLE_EQ(3.0, statistics.median);
	EXPECT_DOUBLE_EQ(9.0, statistics.p95);
	EXPECT_DOUBLE_EQ(4.0, PercentileUpper(ToDoubles({ 1.0, 2.0, 3.0, 4.0 }), 95.0));
}

TEST(OutputServiceProviderMeasurement, MetadataSchemaIsPayloadFree)
{
	BenchmarkSettings settings;
	settings.configuration = "Debug";
	settings.seed = kDefaultSeed;
	const auto trace = MakeTrace(kDefaultSeed);
	std::ostringstream stream;
	stream << "{\"schemaVersion\":" << kSchemaVersion
		<< ",\"record\":\"metadata\",\"provider\":\"cpp\",\"configuration\":\""
		<< JsonEscape(settings.configuration) << "\",\"seed\":" << settings.seed
		<< ",\"affinityMask\":" << settings.affinityMask
		<< ",\"traceDigest\":" << HashTrace(trace) << ",\"payloadFree\":true}";
	const auto json = stream.str();
	EXPECT_NE(std::string::npos, json.find("\"schemaVersion\":1"));
	EXPECT_NE(std::string::npos, json.find("\"payloadFree\":true"));
	EXPECT_EQ(std::string::npos, json.find("\"channelId\":"));
	EXPECT_EQ(std::string::npos, json.find("\"operationId\":"));
	EXPECT_EQ(std::string::npos, json.find("\"text\":"));
	EXPECT_NE(std::string::npos, json.find("\"affinityMask\":1"));
}

TEST(OutputServiceProviderMeasurement, CompileSelectedProviderHealthIsReady)
{
	const auto service = CreateSelectedProvider(MeasurementLimits());
	const auto health = service->Health();
	EXPECT_EQ(DefaultOutputProviderKind(), health.kind);
	EXPECT_EQ(EOutputProviderFactoryStatus::Created, health.factoryStatus);
	EXPECT_EQ(EOutputProviderLifecycle::Ready, health.lifecycle);
	EXPECT_EQ(EOutputProviderInitializationStage::Ready, health.initializationStage);
	EXPECT_TRUE(health.compiledIn);
	EXPECT_TRUE(health.available);
	EXPECT_FALSE(health.testOverrideActive);
}

TEST(OutputServiceProviderMeasurement, DISABLED_CompileSelectedProviderWorkload)
{
	RunMeasurement();
}

} // namespace
} // namespace workbench::output
