/*! @file
 * @brief UI-independent bounded Windows child-process execution.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform::process {

enum class EBoundedProcessStatus : std::uint8_t {
	Succeeded,
	Failed,
	InvalidRequest,
	LaunchFailed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
};

class BoundedProcessRequest final {
public:
	BoundedProcessRequest(std::wstring executablePath, std::wstring workingDirectory,
		std::vector<std::wstring> arguments);

	void SetStandardInput(std::string value) { m_standardInput = std::move(value); }
	void SetEnvironmentOverrides(std::vector<std::pair<std::wstring, std::wstring>> value)
	{
		m_environmentOverrides = std::move(value);
	}
	void SetTimeoutMilliseconds(std::uint32_t value) noexcept { m_timeoutMilliseconds = value; }
	void SetMaximumStandardOutputBytes(std::size_t value) noexcept { m_maximumStandardOutputBytes = value; }
	void SetMaximumStandardErrorBytes(std::size_t value) noexcept { m_maximumStandardErrorBytes = value; }

	[[nodiscard]] const std::wstring& ExecutablePath() const noexcept { return m_executablePath; }
	[[nodiscard]] const std::wstring& WorkingDirectory() const noexcept { return m_workingDirectory; }
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
	[[nodiscard]] const std::string& StandardInput() const noexcept { return m_standardInput; }
	[[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& EnvironmentOverrides() const noexcept
	{
		return m_environmentOverrides;
	}
	[[nodiscard]] std::uint32_t TimeoutMilliseconds() const noexcept { return m_timeoutMilliseconds; }
	[[nodiscard]] std::size_t MaximumStandardOutputBytes() const noexcept { return m_maximumStandardOutputBytes; }
	[[nodiscard]] std::size_t MaximumStandardErrorBytes() const noexcept { return m_maximumStandardErrorBytes; }

private:
	std::wstring m_executablePath;
	std::wstring m_workingDirectory;
	std::vector<std::wstring> m_arguments;
	std::string m_standardInput;
	std::vector<std::pair<std::wstring, std::wstring>> m_environmentOverrides;
	std::uint32_t m_timeoutMilliseconds{ 15000 };
	std::size_t m_maximumStandardOutputBytes{ 4u * 1024u * 1024u };
	std::size_t m_maximumStandardErrorBytes{ 64u * 1024u };
};

class BoundedProcessResult final {
public:
	explicit BoundedProcessResult(EBoundedProcessStatus status) noexcept : m_status(status) {}
	[[nodiscard]] EBoundedProcessStatus Status() const noexcept { return m_status; }
	[[nodiscard]] int ExitCode() const noexcept { return m_exitCode; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardOutput() const noexcept { return m_standardOutput; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardError() const noexcept { return m_standardError; }
	[[nodiscard]] bool Succeeded() const noexcept { return m_status == EBoundedProcessStatus::Succeeded; }

private:
	EBoundedProcessStatus m_status{ EBoundedProcessStatus::InvalidRequest };
	int m_exitCode{ -1 };
	std::vector<std::uint8_t> m_standardOutput;
	std::vector<std::uint8_t> m_standardError;

	friend BoundedProcessResult RunBoundedProcess(const BoundedProcessRequest&, HANDLE);
};

inline constexpr std::size_t kMaximumBoundedProcessArguments = 128;
inline constexpr std::size_t kMaximumBoundedProcessArgumentLength = 32768;
inline constexpr std::size_t kMaximumBoundedProcessCommandLineLength = 32000;
inline constexpr std::size_t kMaximumBoundedProcessStandardInputBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumBoundedProcessEnvironmentOverrides = 32;
inline constexpr std::size_t kMaximumBoundedProcessOutputBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaximumBoundedProcessErrorBytes = 4u * 1024u * 1024u;

[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view value);
[[nodiscard]] std::wstring BuildWindowsCommandLine(
	std::wstring_view executable, const std::vector<std::wstring>& arguments);
[[nodiscard]] bool IsExecutableBoundedProcessRequest(const BoundedProcessRequest& request) noexcept;

//! Run one exact executable with bounded streams, deadline, cancellation, and
//! kill-on-close ownership for its complete process tree.
[[nodiscard]] BoundedProcessResult RunBoundedProcess(const BoundedProcessRequest& request, HANDLE stop);

} // namespace platform::process
