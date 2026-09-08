/*! @file
 * @brief Closed GitHub CLI discovery and repository-read process policy.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "platform/process/BoundedProcessRunner.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace senp::github {

enum class GhToolAvailability : std::uint8_t {
	Available,
	NotInstalled,
	UnsupportedVersion,
	InvalidConfiguration,
	ProbeFailed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
};

class GhToolVersion final {
public:
	GhToolVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch) noexcept;
	[[nodiscard]] std::uint32_t Major() const noexcept { return m_major; }
	[[nodiscard]] std::uint32_t Minor() const noexcept { return m_minor; }
	[[nodiscard]] std::uint32_t Patch() const noexcept { return m_patch; }
	[[nodiscard]] bool Supported() const noexcept;
private:
	std::uint32_t m_major{}, m_minor{}, m_patch{};
};

class GhToolProbe final {
public:
	[[nodiscard]] GhToolAvailability Status() const noexcept { return m_status; }
	[[nodiscard]] const std::wstring& ExecutablePath() const noexcept { return m_executablePath; }
	[[nodiscard]] const std::optional<GhToolVersion>& Version() const noexcept { return m_version; }
private:
	GhToolProbe(GhToolAvailability status, std::wstring executablePath,
		std::optional<GhToolVersion> version) noexcept;
	GhToolAvailability m_status{ GhToolAvailability::InvalidConfiguration };
	std::wstring m_executablePath;
	std::optional<GhToolVersion> m_version;
	friend class CGhToolPolicy;
};

enum class GhRepositoryReadStatus : std::uint8_t;

class GhRepositoryReadRequest final {
public:
	GhRepositoryReadRequest(std::wstring hostname, std::wstring owner,
		std::wstring repository, std::vector<std::wstring> resourceSegments);
	GhRepositoryReadRequest(std::wstring hostname, std::wstring owner,
		std::wstring repository, std::vector<std::wstring> resourceSegments,
		std::vector<std::pair<std::wstring, std::wstring>> query,
		std::optional<std::wstring> ifNoneMatch);
	[[nodiscard]] const std::wstring& Hostname() const noexcept { return m_hostname; }
	[[nodiscard]] const std::wstring& Owner() const noexcept { return m_owner; }
	[[nodiscard]] const std::wstring& Repository() const noexcept { return m_repository; }
	[[nodiscard]] const std::vector<std::wstring>& ResourceSegments() const noexcept { return m_resourceSegments; }
	[[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& Query() const noexcept { return m_query; }
	[[nodiscard]] const std::optional<std::wstring>& IfNoneMatch() const noexcept { return m_ifNoneMatch; }
private:
	std::wstring m_hostname, m_owner, m_repository;
	std::vector<std::wstring> m_resourceSegments;
	std::vector<std::pair<std::wstring, std::wstring>> m_query;
	std::optional<std::wstring> m_ifNoneMatch;
};

class GhPreparedRepositoryRead final {
public:
	GhPreparedRepositoryRead(GhRepositoryReadStatus status, std::vector<std::wstring> arguments);
	[[nodiscard]] GhRepositoryReadStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
private:
	GhRepositoryReadStatus m_status;
	std::vector<std::wstring> m_arguments;
};

class GhProcessInvocation final {
public:
	GhProcessInvocation(std::wstring executablePath, std::wstring workingDirectory,
		std::vector<std::wstring> arguments, std::vector<std::pair<std::wstring, std::wstring>> environmentOverrides,
		std::vector<std::wstring> environmentRemovals, std::uint32_t timeoutMilliseconds,
		std::size_t maximumOutputBytes, std::size_t maximumErrorBytes);
	GhProcessInvocation(std::wstring executablePath, std::wstring workingDirectory,
		std::vector<std::wstring> arguments, std::vector<std::pair<std::wstring, std::wstring>> environmentOverrides,
		std::vector<std::wstring> environmentRemovals, std::uint32_t timeoutMilliseconds,
		std::size_t maximumOutputBytes, std::size_t maximumErrorBytes,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> outputObserver);
	~GhProcessInvocation()
	{
		for (auto& entry : m_environmentOverrides) {
			if (!entry.second.empty()) ::SecureZeroMemory(entry.second.data(), entry.second.size() * sizeof(wchar_t));
		}
	}
	GhProcessInvocation(const GhProcessInvocation&) = default;
	GhProcessInvocation& operator=(const GhProcessInvocation&) = default;
	GhProcessInvocation(GhProcessInvocation&&) noexcept = default;
	GhProcessInvocation& operator=(GhProcessInvocation&&) noexcept = default;
	[[nodiscard]] const std::wstring& ExecutablePath() const noexcept { return m_executablePath; }
	[[nodiscard]] const std::wstring& WorkingDirectory() const noexcept { return m_workingDirectory; }
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
	[[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& EnvironmentOverrides() const noexcept { return m_environmentOverrides; }
	[[nodiscard]] const std::vector<std::wstring>& EnvironmentRemovals() const noexcept { return m_environmentRemovals; }
	[[nodiscard]] std::uint32_t TimeoutMilliseconds() const noexcept { return m_timeoutMilliseconds; }
	[[nodiscard]] std::size_t MaximumOutputBytes() const noexcept { return m_maximumOutputBytes; }
	[[nodiscard]] std::size_t MaximumErrorBytes() const noexcept { return m_maximumErrorBytes; }
	[[nodiscard]] const std::shared_ptr<platform::process::IBoundedProcessOutputObserver>& OutputObserver() const noexcept
	{
		return m_outputObserver;
	}
private:
	std::wstring m_executablePath, m_workingDirectory;
	std::vector<std::wstring> m_arguments;
	std::vector<std::pair<std::wstring, std::wstring>> m_environmentOverrides;
	std::vector<std::wstring> m_environmentRemovals;
	std::shared_ptr<platform::process::IBoundedProcessOutputObserver> m_outputObserver;
	std::uint32_t m_timeoutMilliseconds{};
	std::size_t m_maximumOutputBytes{}, m_maximumErrorBytes{};
};

class GhProcessOutcome final {
public:
	GhProcessOutcome(platform::process::EBoundedProcessStatus status, int exitCode,
		std::vector<std::uint8_t> standardOutput, std::vector<std::uint8_t> standardError);
	[[nodiscard]] platform::process::EBoundedProcessStatus Status() const noexcept { return m_status; }
	[[nodiscard]] int ExitCode() const noexcept { return m_exitCode; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardOutput() const noexcept { return m_standardOutput; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardError() const noexcept { return m_standardError; }
	[[nodiscard]] std::vector<std::uint8_t> TakeStandardOutput() noexcept { return std::move(m_standardOutput); }
private:
	platform::process::EBoundedProcessStatus m_status{ platform::process::EBoundedProcessStatus::InvalidRequest };
	int m_exitCode{ -1 };
	std::vector<std::uint8_t> m_standardOutput, m_standardError;
};

class IGhToolPlatform {
public:
	virtual ~IGhToolPlatform() = default;
	[[nodiscard]] virtual std::optional<std::wstring> ResolveExecutable() const = 0;
	[[nodiscard]] virtual GhProcessOutcome Run(const GhProcessInvocation& invocation, HANDLE stop) const = 0;
};

class CWindowsGhToolPlatform final : public IGhToolPlatform {
public:
	[[nodiscard]] std::optional<std::wstring> ResolveExecutable() const override;
	[[nodiscard]] GhProcessOutcome Run(const GhProcessInvocation& invocation, HANDLE stop) const override;
};

enum class GhRepositoryReadStatus : std::uint8_t {
	Succeeded,
	Failed,
	InvalidRequest,
	ToolUnavailable,
	UnsupportedVersion,
	LaunchFailed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
};

class GhRepositoryReadResult final {
public:
	GhRepositoryReadResult(GhRepositoryReadStatus status, int exitCode,
		std::vector<std::uint8_t> standardOutput, std::vector<std::uint8_t> standardError);
	[[nodiscard]] GhRepositoryReadStatus Status() const noexcept { return m_status; }
	[[nodiscard]] int ExitCode() const noexcept { return m_exitCode; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardOutput() const noexcept { return m_standardOutput; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardError() const noexcept { return m_standardError; }
private:
	GhRepositoryReadStatus m_status{ GhRepositoryReadStatus::InvalidRequest };
	int m_exitCode{ -1 };
	std::vector<std::uint8_t> m_standardOutput, m_standardError;
};

//! This policy exposes two fixed operations: version discovery and one REST GET.
//! It never accepts an executable, shell command, flag, cwd, stdin, or environment
//! value from an extension.
class CGhToolPolicy final {
public:
	CGhToolPolicy(std::shared_ptr<const IGhToolPlatform> platform, std::wstring workingDirectory);
	[[nodiscard]] GhToolProbe Probe(HANDLE stop) const;
	[[nodiscard]] GhRepositoryReadResult ReadRepository(const GhToolProbe& probe,
		const GhRepositoryReadRequest& request, HANDLE stop) const;
	[[nodiscard]] GhPreparedRepositoryRead PrepareRepositoryRead(const GhToolProbe& probe,
		const GhRepositoryReadRequest& request) const;
private:
	std::shared_ptr<const IGhToolPlatform> m_platform;
	std::wstring m_workingDirectory;
};

} // namespace senp::github
