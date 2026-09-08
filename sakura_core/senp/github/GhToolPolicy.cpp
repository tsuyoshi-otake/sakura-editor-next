/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/github/GhToolPolicy.h"
#include "platform/process/WindowsExecutableResolver.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace senp::github {
namespace {

constexpr std::uint32_t kSupportedMajor = 2, kSupportedMinor = 93, kSupportedPatch = 0;
constexpr std::uint32_t kProbeTimeoutMilliseconds = 5000;
constexpr std::uint32_t kReadTimeoutMilliseconds = 30000;
constexpr std::size_t kProbeOutputBytes = 16u * 1024u;
constexpr std::size_t kReadOutputBytes = 4u * 1024u * 1024u;
constexpr std::size_t kErrorBytes = 64u * 1024u;

const std::vector<std::pair<std::wstring, std::wstring>>& FixedEnvironment()
{
	static const std::vector<std::pair<std::wstring, std::wstring>> values{
		{ L"GH_PROMPT_DISABLED", L"1" },
		{ L"GH_NO_UPDATE_NOTIFIER", L"1" },
		{ L"GH_SPINNER_DISABLED", L"1" },
	};
	return values;
}

const std::vector<std::wstring>& RemovedEnvironment()
{
	static const std::vector<std::wstring> values{
		L"GH_TOKEN", L"GITHUB_TOKEN", L"GH_ENTERPRISE_TOKEN", L"GITHUB_ENTERPRISE_TOKEN",
		L"GH_HOST", L"GH_REPO", L"GH_DEBUG", L"DEBUG", L"GH_PAGER", L"PAGER",
		L"GH_BROWSER", L"BROWSER", L"GH_FORCE_TTY", L"GH_HTTP_UNIX_SOCKET", L"GH_CONFIG_DIR",
		L"GH_EDITOR", L"GIT_EDITOR", L"VISUAL", L"EDITOR",
	};
	return values;
}

bool IsAsciiName(const std::wstring_view value, const std::size_t maximum) noexcept
{
	if (value.empty() || value.size() > maximum || value == L"." || value == L"..") return false;
	return std::ranges::all_of(value, [](const wchar_t character) {
		return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')
			|| (character >= L'0' && character <= L'9') || character == L'-'
			|| character == L'_' || character == L'.';
	});
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

bool IsRequest(const GhRepositoryReadRequest& request) noexcept
{
	if (!IsHostname(request.Hostname()) || !IsAsciiName(request.Owner(), 100)
		|| !IsAsciiName(request.Repository(), 100) || request.ResourceSegments().empty()
		|| request.ResourceSegments().size() > 16) return false;
	std::size_t endpointLength = request.Owner().size() + request.Repository().size() + 8;
	for (const auto& segment : request.ResourceSegments()) {
		if (!IsAsciiName(segment, 128)) return false;
		endpointLength += segment.size() + 1;
	}
	return endpointLength <= 2048;
}

std::wstring Endpoint(const GhRepositoryReadRequest& request)
{
	std::wstring value = L"repos/" + request.Owner() + L"/" + request.Repository();
	for (const auto& segment : request.ResourceSegments()) value += L"/" + segment;
	return value;
}

std::optional<GhToolVersion> ParseVersion(const std::vector<std::uint8_t>& bytes) noexcept
{
	constexpr std::string_view prefix = "gh version ";
	if (bytes.size() <= prefix.size() || !std::equal(prefix.begin(), prefix.end(), bytes.begin())) return std::nullopt;
	const auto* begin = reinterpret_cast<const char*>(bytes.data()) + prefix.size();
	const auto* const limit = reinterpret_cast<const char*>(bytes.data()) + bytes.size();
	auto read = [&](std::uint32_t& value, const char separator) {
		const auto parsed = std::from_chars(begin, limit, value);
		if (parsed.ec != std::errc{} || parsed.ptr == limit || *parsed.ptr != separator) return false;
		begin = parsed.ptr + 1;
		return true;
	};
	std::uint32_t major{}, minor{}, patch{};
	if (!read(major, '.') || !read(minor, '.')) return std::nullopt;
	const auto parsed = std::from_chars(begin, limit, patch);
	if (parsed.ec != std::errc{} || limit - parsed.ptr < 2
		|| parsed.ptr[0] != ' ' || parsed.ptr[1] != '(') return std::nullopt;
	return GhToolVersion(major, minor, patch);
}

GhToolAvailability ProbeTerminal(const platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::TimedOut: return GhToolAvailability::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhToolAvailability::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhToolAvailability::OutputLimitExceeded;
	default: return GhToolAvailability::ProbeFailed;
	}
}

GhRepositoryReadStatus ReadTerminal(const platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::Succeeded: return GhRepositoryReadStatus::Succeeded;
	case platform::process::EBoundedProcessStatus::Failed: return GhRepositoryReadStatus::Failed;
	case platform::process::EBoundedProcessStatus::InvalidRequest: return GhRepositoryReadStatus::InvalidRequest;
	case platform::process::EBoundedProcessStatus::LaunchFailed: return GhRepositoryReadStatus::LaunchFailed;
	case platform::process::EBoundedProcessStatus::TimedOut: return GhRepositoryReadStatus::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return GhRepositoryReadStatus::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return GhRepositoryReadStatus::OutputLimitExceeded;
	}
	return GhRepositoryReadStatus::LaunchFailed;
}

GhProcessInvocation Invocation(std::wstring executable, std::wstring workingDirectory,
	std::vector<std::wstring> arguments, const std::uint32_t timeout,
	const std::size_t outputLimit, const std::size_t errorLimit)
{
	return { std::move(executable), std::move(workingDirectory), std::move(arguments),
		FixedEnvironment(), RemovedEnvironment(), timeout, outputLimit, errorLimit };
}

} // namespace

GhToolVersion::GhToolVersion(const std::uint32_t major, const std::uint32_t minor,
	const std::uint32_t patch) noexcept : m_major(major), m_minor(minor), m_patch(patch) {}
bool GhToolVersion::Supported() const noexcept
{
	return m_major == kSupportedMajor && m_minor == kSupportedMinor && m_patch == kSupportedPatch;
}

GhToolProbe::GhToolProbe(const GhToolAvailability status, std::wstring executablePath,
	std::optional<GhToolVersion> version) noexcept : m_status(status),
	m_executablePath(std::move(executablePath)), m_version(std::move(version)) {}

GhRepositoryReadRequest::GhRepositoryReadRequest(std::wstring hostname, std::wstring owner,
	std::wstring repository, std::vector<std::wstring> resourceSegments) :
	m_hostname(std::move(hostname)), m_owner(std::move(owner)), m_repository(std::move(repository)),
	m_resourceSegments(std::move(resourceSegments)) {}

GhProcessInvocation::GhProcessInvocation(std::wstring executablePath, std::wstring workingDirectory,
	std::vector<std::wstring> arguments, std::vector<std::pair<std::wstring, std::wstring>> environmentOverrides,
	std::vector<std::wstring> environmentRemovals, const std::uint32_t timeoutMilliseconds,
	const std::size_t maximumOutputBytes, const std::size_t maximumErrorBytes) :
	m_executablePath(std::move(executablePath)), m_workingDirectory(std::move(workingDirectory)),
	m_arguments(std::move(arguments)), m_environmentOverrides(std::move(environmentOverrides)),
	m_environmentRemovals(std::move(environmentRemovals)), m_timeoutMilliseconds(timeoutMilliseconds),
	m_maximumOutputBytes(maximumOutputBytes), m_maximumErrorBytes(maximumErrorBytes) {}

GhProcessOutcome::GhProcessOutcome(const platform::process::EBoundedProcessStatus status, const int exitCode,
	std::vector<std::uint8_t> standardOutput, std::vector<std::uint8_t> standardError) :
	m_status(status), m_exitCode(exitCode), m_standardOutput(std::move(standardOutput)),
	m_standardError(std::move(standardError)) {}

std::optional<std::wstring> CWindowsGhToolPlatform::ResolveExecutable() const
{
	return platform::ResolveWindowsExecutable(L"gh.exe");
}

GhProcessOutcome CWindowsGhToolPlatform::Run(const GhProcessInvocation& invocation, HANDLE stop) const
{
	platform::process::BoundedProcessRequest request(
		invocation.ExecutablePath(), invocation.WorkingDirectory(), invocation.Arguments());
	request.SetEnvironmentOverrides(invocation.EnvironmentOverrides());
	request.SetEnvironmentRemovals(invocation.EnvironmentRemovals());
	request.SetTimeoutMilliseconds(invocation.TimeoutMilliseconds());
	request.SetMaximumStandardOutputBytes(invocation.MaximumOutputBytes());
	request.SetMaximumStandardErrorBytes(invocation.MaximumErrorBytes());
	const auto result = platform::process::RunBoundedProcess(request, stop);
	return { result.Status(), result.ExitCode(), result.StandardOutput(), result.StandardError() };
}

GhRepositoryReadResult::GhRepositoryReadResult(const GhRepositoryReadStatus status, const int exitCode,
	std::vector<std::uint8_t> standardOutput, std::vector<std::uint8_t> standardError) :
	m_status(status), m_exitCode(exitCode), m_standardOutput(std::move(standardOutput)),
	m_standardError(std::move(standardError)) {}

CGhToolPolicy::CGhToolPolicy(std::shared_ptr<const IGhToolPlatform> platform, std::wstring workingDirectory) :
	m_platform(std::move(platform)), m_workingDirectory(std::move(workingDirectory)) {}

GhToolProbe CGhToolPolicy::Probe(HANDLE stop) const
try {
	if (!m_platform || !platform::IsAbsoluteWindowsPath(m_workingDirectory)) {
		return { GhToolAvailability::InvalidConfiguration, {}, std::nullopt };
	}
	const auto executable = m_platform->ResolveExecutable();
	if (!executable) return { GhToolAvailability::NotInstalled, {}, std::nullopt };
	if (!platform::IsAbsoluteWindowsPath(*executable)) {
		return { GhToolAvailability::InvalidConfiguration, {}, std::nullopt };
	}
	const auto outcome = m_platform->Run(Invocation(*executable, m_workingDirectory,
		{ L"--version" }, kProbeTimeoutMilliseconds, kProbeOutputBytes, kErrorBytes), stop);
	if (outcome.Status() != platform::process::EBoundedProcessStatus::Succeeded) {
		return { ProbeTerminal(outcome.Status()), *executable, std::nullopt };
	}
	const auto version = ParseVersion(outcome.StandardOutput());
	if (!version) return { GhToolAvailability::ProbeFailed, *executable, std::nullopt };
	return { version->Supported() ? GhToolAvailability::Available : GhToolAvailability::UnsupportedVersion,
		*executable, version };
} catch (...) {
	return { GhToolAvailability::ProbeFailed, {}, std::nullopt };
}

GhRepositoryReadResult CGhToolPolicy::ReadRepository(const GhToolProbe& probe,
	const GhRepositoryReadRequest& request, HANDLE stop) const
try {
	if (!m_platform || !platform::IsAbsoluteWindowsPath(m_workingDirectory) || !IsRequest(request)) {
		return { GhRepositoryReadStatus::InvalidRequest, -1, {}, {} };
	}
	if (probe.Status() == GhToolAvailability::UnsupportedVersion) {
		return { GhRepositoryReadStatus::UnsupportedVersion, -1, {}, {} };
	}
	if (probe.Status() != GhToolAvailability::Available || !probe.Version()
		|| !probe.Version()->Supported() || !platform::IsAbsoluteWindowsPath(probe.ExecutablePath())) {
		return { GhRepositoryReadStatus::ToolUnavailable, -1, {}, {} };
	}
	const std::vector<std::wstring> arguments{
		L"api", L"--hostname", request.Hostname(), L"--method", L"GET", L"--include",
		L"--header", L"X-GitHub-Api-Version: 2022-11-28", Endpoint(request),
	};
	const auto outcome = m_platform->Run(Invocation(probe.ExecutablePath(), m_workingDirectory,
		arguments, kReadTimeoutMilliseconds, kReadOutputBytes, kErrorBytes), stop);
	return { ReadTerminal(outcome.Status()), outcome.ExitCode(),
		outcome.StandardOutput(), outcome.StandardError() };
} catch (...) {
	return { GhRepositoryReadStatus::LaunchFailed, -1, {}, {} };
}

} // namespace senp::github
