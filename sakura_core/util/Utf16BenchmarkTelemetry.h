/*! @file @brief Explicit benchmark-only UTF-16 caller telemetry. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

// This header deliberately has no production implementation.  The build
// definition is added only to a dedicated benchmark/test build.  In an
// ordinary product translation unit the macro call below preprocesses to a
// literal no-op, so there is no environment lookup, branch, atomic, logging,
// or telemetry symbol on any UTF-16 caller path.
#if defined(SAKURA_UTF16_BENCHMARK_TELEMETRY)

#include "util/CpuDispatch.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Utf16BenchmarkTelemetry
{
inline std::ofstream g_output;
inline std::string g_corpus{"unlabeled"};
inline std::uint64_t g_runId{};
inline std::uint64_t g_eventOrder{};
inline bool g_started{};

inline constexpr bool RustCandidateLinked() noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
	return true;
#else
	return false;
#endif
}

inline const char* ImplementationForOperation(
	const CpuDispatch::Dispatch& dispatch, std::string_view operation) noexcept
{
	if (operation == "crlf") return dispatch.utf16CrOrLfImplementation;
	if (operation == "markdown") return dispatch.utf16MarkdownImplementation;
	if (operation == "find_char") return dispatch.utf16FindCharImplementation;
	return "unknown";
}

inline const char* IsaName(CpuDispatch::Isa isa) noexcept
{
	switch (isa) {
	case CpuDispatch::Isa::Avx2: return "avx2";
	case CpuDispatch::Isa::Avx512: return "avx512bw";
	default: return "avx128";
	}
}

inline void Start(const std::filesystem::path& path, std::uint64_t runId)
{
	if (g_output.is_open()) g_output.close();
	g_output.open(path, std::ios::binary | std::ios::trunc);
	if (!g_output) throw std::runtime_error("cannot open UTF-16 telemetry output");
	g_runId = runId;
	g_eventOrder = 0;
	g_corpus = "unlabeled";
	g_started = true;
	const auto& dispatch = CpuDispatch::Get();
	g_output << "{\"kind\":\"metadata\",\"mode\":\"histogram\",\"run_id\":"
		<< g_runId << ",\"pid\":" << ::GetCurrentProcessId()
		<< ",\"backend\":\"" << dispatch.utf16Backend
		<< "\",\"build_mode\":\"" << dispatch.utf16BuildMode
		<< "\",\"selected_isa\":\"" << IsaName(dispatch.isa)
		<< "\",\"crlf_isa\":\"" << IsaName(dispatch.utf16CrOrLfIsa)
		<< "\",\"markdown_isa\":\"" << IsaName(dispatch.utf16MarkdownIsa)
		<< "\",\"find_char_isa\":\"" << IsaName(dispatch.utf16FindCharIsa)
		<< "\",\"crlf_implementation\":\"" << dispatch.utf16CrOrLfImplementation
		<< "\",\"markdown_implementation\":\""
		<< dispatch.utf16MarkdownImplementation
		<< "\",\"find_char_implementation\":\""
		<< dispatch.utf16FindCharImplementation
		<< "\",\"rust_candidate_linked\":"
		<< (RustCandidateLinked() ? "true" : "false")
		<< ",\"abi_version\":" << dispatch.utf16AbiVersion << "}\n";
	if (!g_output) throw std::runtime_error("cannot write UTF-16 telemetry metadata");
}

inline void SetCorpusLabel(std::string_view corpus)
{
	g_corpus.assign(corpus);
}

inline void Stop() noexcept
{
	if (!g_output.is_open()) {
		g_started = false;
		return;
	}
	g_output.flush();
	g_output.close();
	g_started = false;
}

inline void Record(
	std::string_view operation,
	std::size_t remaining,
	std::size_t result,
	const void* data,
	CpuDispatch::Isa isa,
	std::string_view path)
{
	if (!g_started) return;
	const auto alignment = reinterpret_cast<std::uintptr_t>(data) & 63U;
	g_output << "{\"kind\":\"caller_scan\",\"run_id\":" << g_runId
		<< ",\"event_order\":" << g_eventOrder++
		<< ",\"operation\":\"" << operation
		<< "\",\"remaining_length\":" << remaining
		<< ",\"result\":\"" << (result >= remaining ? "not_found" : "found")
		<< "\",\"result_offset\":" << result
		<< ",\"alignment_mod64\":" << alignment
		<< ",\"selected_isa\":\"" << IsaName(isa)
		<< "\",\"backend\":\"" << CpuDispatch::Get().utf16Backend
		<< "\",\"implementation\":\""
		<< ImplementationForOperation(CpuDispatch::Get(), operation)
		<< "\",\"implementation_path\":\"" << path
		<< "\",\"rust_candidate_linked\":"
		<< (RustCandidateLinked() ? "true" : "false")
		<< ",\"corpus_label\":\"" << g_corpus << "\"}\n";
	if (!g_output) throw std::runtime_error("cannot write UTF-16 telemetry output");
}
}

#define SAKURA_UTF16_BENCHMARK_RECORD(operation, remaining, result, data, isa, path) \
	::Utf16BenchmarkTelemetry::Record(operation, remaining, result, data, isa, path)

#else

#define SAKURA_UTF16_BENCHMARK_RECORD(operation, remaining, result, data, isa, path) \
	do { \
	} while (false)

#endif
