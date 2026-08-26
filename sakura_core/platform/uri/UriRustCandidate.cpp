/*! @file @brief C++ two-pass adapter for the Rust URI shadow candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "UriRustCandidate.h"

#include <limits>
#include <utility>

namespace platform::uri::rust_candidate {
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
namespace {

static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));

SakuraUriCandidateSpanV1 Span(std::wstring_view value) noexcept
{
	SakuraUriCandidateSpanV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	result.data = reinterpret_cast<const std::uint16_t*>(value.data());
	result.length = static_cast<std::uint64_t>(value.size());
	return result;
}

SakuraUriCandidateBufferV1 Buffer(std::wstring& value) noexcept
{
	SakuraUriCandidateBufferV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	result.data = value.empty() ? nullptr : reinterpret_cast<std::uint16_t*>(value.data());
	result.capacity = static_cast<std::uint64_t>(value.size());
	return result;
}

SakuraUriCandidateMeasureV1 EmptyMeasure() noexcept
{
	SakuraUriCandidateMeasureV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	return result;
}

bool IsValidMeasure(const SakuraUriCandidateMeasureV1& measure) noexcept
{
	return measure.struct_size == sizeof(measure)
		&& measure.abi_version == SAKURA_URI_CANDIDATE_ABI_VERSION_V1
		&& measure.has_authority <= 1
		&& measure.has_query <= 1
		&& measure.has_fragment <= 1
		&& measure.reserved == 0
		&& measure.reserved64[0] == 0
		&& measure.reserved64[1] == 0
		&& (measure.has_authority != 0 || measure.authority_length == 0)
		&& (measure.has_query != 0 || measure.query_length == 0)
		&& (measure.has_fragment != 0 || measure.fragment_length == 0);
}

bool Resize(std::wstring& value, std::uint64_t length) noexcept
{
	if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
	try {
		value.resize(static_cast<std::size_t>(length));
		return true;
	} catch (...) {
		return false;
	}
}

bool ResizeBuffers(
	const SakuraUriCandidateMeasureV1& measure,
	std::wstring& scheme,
	std::wstring& authority,
	std::wstring& path,
	std::wstring& query,
	std::wstring& fragment,
	std::wstring& serialized) noexcept
{
	return Resize(scheme, measure.scheme_length)
		&& Resize(authority, measure.authority_length)
		&& Resize(path, measure.path_length)
		&& Resize(query, measure.query_length)
		&& Resize(fragment, measure.fragment_length)
		&& Resize(serialized, measure.serialized_length);
}

SakuraUriCandidateBuffersV1 Buffers(
	std::wstring& scheme,
	std::wstring& authority,
	std::wstring& path,
	std::wstring& query,
	std::wstring& fragment,
	std::wstring& serialized) noexcept
{
	SakuraUriCandidateBuffersV1 result{};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	result.scheme = Buffer(scheme);
	result.authority = Buffer(authority);
	result.path = Buffer(path);
	result.query = Buffer(query);
	result.fragment = Buffer(fragment);
	result.serialized = Buffer(serialized);
	return result;
}

Result MakeResult(
	SakuraUriCandidateStatus status,
	const SakuraUriCandidateOutputV1& output,
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::wstring query,
	std::wstring fragment,
	std::wstring serialized) noexcept
{
	if (status != SakuraUriCandidateStatus::Ok
		|| !IsValidMeasure(output)
		|| output.scheme_length != scheme.size()
		|| output.authority_length != authority.size()
		|| output.path_length != path.size()
		|| output.query_length != query.size()
		|| output.fragment_length != fragment.size()
		|| output.serialized_length != serialized.size()) {
		return { std::nullopt, status == SakuraUriCandidateStatus::Ok
			? SakuraUriCandidateStatus::InternalError : status };
	}

	Value value;
	value.scheme = std::move(scheme);
	value.authority = std::move(authority);
	value.path = std::move(path);
	if (output.has_query != 0) value.query = std::move(query);
	if (output.has_fragment != 0) value.fragment = std::move(fragment);
	value.hasAuthority = output.has_authority != 0;
	value.serialized = std::move(serialized);
	return { std::move(value), SakuraUriCandidateStatus::Ok };
}

Result ParseImpl(std::wstring_view text) noexcept
{
	SakuraUriCandidateSpanV1 input = Span(text);
	SakuraUriCandidateMeasureV1 measure = EmptyMeasure();
	const auto measured = sakura_uri_candidate_parse_measure_v1(&input, &measure);
	if (measured != SakuraUriCandidateStatus::Ok) return { std::nullopt, measured };
	if (!IsValidMeasure(measure)) return { std::nullopt, SakuraUriCandidateStatus::InternalError };

	std::wstring scheme;
	std::wstring authority;
	std::wstring path;
	std::wstring query;
	std::wstring fragment;
	std::wstring serialized;
	if (!ResizeBuffers(measure, scheme, authority, path, query, fragment, serialized)) {
		return { std::nullopt, SakuraUriCandidateStatus::InternalError };
	}
	const auto buffers = Buffers(scheme, authority, path, query, fragment, serialized);
	SakuraUriCandidateOutputV1 output = EmptyMeasure();
	const auto status = sakura_uri_candidate_parse_write_v1(&input, &buffers, &output);
	return MakeResult(status, output, std::move(scheme), std::move(authority), std::move(path),
		std::move(query), std::move(fragment), std::move(serialized));
}

Result FromComponentsImpl(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query,
	std::optional<std::wstring> fragment,
	bool hasAuthority) noexcept
{
	SakuraUriCandidateComponentsV1 input{};
	input.struct_size = sizeof(input);
	input.abi_version = SAKURA_URI_CANDIDATE_ABI_VERSION_V1;
	input.scheme = Span(scheme);
	input.authority = Span(authority);
	input.path = Span(path);
	input.query = Span(query ? std::wstring_view(*query) : std::wstring_view{});
	input.fragment = Span(fragment ? std::wstring_view(*fragment) : std::wstring_view{});
	input.has_authority = hasAuthority ? 1u : 0u;
	input.has_query = query ? 1u : 0u;
	input.has_fragment = fragment ? 1u : 0u;

	SakuraUriCandidateMeasureV1 measure = EmptyMeasure();
	const auto measured = sakura_uri_candidate_from_components_measure_v1(&input, &measure);
	if (measured != SakuraUriCandidateStatus::Ok) return { std::nullopt, measured };
	if (!IsValidMeasure(measure)) return { std::nullopt, SakuraUriCandidateStatus::InternalError };

	std::wstring outputScheme;
	std::wstring outputAuthority;
	std::wstring outputPath;
	std::wstring outputQuery;
	std::wstring outputFragment;
	std::wstring serialized;
	if (!ResizeBuffers(measure, outputScheme, outputAuthority, outputPath, outputQuery, outputFragment, serialized)) {
		return { std::nullopt, SakuraUriCandidateStatus::InternalError };
	}
	const auto buffers = Buffers(outputScheme, outputAuthority, outputPath, outputQuery, outputFragment, serialized);
	SakuraUriCandidateOutputV1 output = EmptyMeasure();
	const auto status = sakura_uri_candidate_from_components_write_v1(&input, &buffers, &output);
	return MakeResult(status, output, std::move(outputScheme), std::move(outputAuthority), std::move(outputPath),
		std::move(outputQuery), std::move(outputFragment), std::move(serialized));
}

} // namespace
#endif

Result Parse(std::wstring_view text) noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	try {
		return ParseImpl(text);
	} catch (...) {
		return { std::nullopt, SakuraUriCandidateStatus::InternalError };
	}
#else
	(void)text;
	return { std::nullopt, SakuraUriCandidateStatus::Unsupported };
#endif
}

Result FromComponents(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query,
	std::optional<std::wstring> fragment,
	bool hasAuthority) noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	try {
		return FromComponentsImpl(std::move(scheme), std::move(authority), std::move(path),
			std::move(query), std::move(fragment), hasAuthority);
	} catch (...) {
		return { std::nullopt, SakuraUriCandidateStatus::InternalError };
	}
#else
	(void)scheme;
	(void)authority;
	(void)path;
	(void)query;
	(void)fragment;
	(void)hasAuthority;
	return { std::nullopt, SakuraUriCandidateStatus::Unsupported };
#endif
}

} // namespace platform::uri::rust_candidate
