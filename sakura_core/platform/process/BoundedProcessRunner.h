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
#include <memory>
#include <span>
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
	ObserverRejected,
};

enum class EBoundedProcessStream : std::uint8_t { StandardOutput, StandardError };

class IBoundedProcessOutputObserver {
public:
	virtual ~IBoundedProcessOutputObserver() = default;
	[[nodiscard]] virtual bool OnOutput(EBoundedProcessStream stream,
		std::span<const std::uint8_t> bytes) = 0;
};

class BoundedProcessRequest final {
public:
	BoundedProcessRequest(std::wstring executablePath, std::wstring workingDirectory,
		std::vector<std::wstring> arguments);
	~BoundedProcessRequest();
	BoundedProcessRequest(BoundedProcessRequest&&) noexcept = default;
	BoundedProcessRequest& operator=(BoundedProcessRequest&&) noexcept = default;
	BoundedProcessRequest(const BoundedProcessRequest&) = delete;
	BoundedProcessRequest& operator=(const BoundedProcessRequest&) = delete;

	void SetStandardInput(std::string value) { m_standardInput = std::move(value); }
	void SetEnvironmentOverrides(std::vector<std::pair<std::wstring, std::wstring>> value)
	{
		m_environmentOverrides = std::move(value);
	}
	void SetEnvironmentRemovals(std::vector<std::wstring> value)
	{
		m_environmentRemovals = std::move(value);
	}
	void SetTimeoutMilliseconds(std::uint32_t value) noexcept { m_timeoutMilliseconds = value; }
	void SetMaximumStandardOutputBytes(std::size_t value) noexcept { m_maximumStandardOutputBytes = value; }
	void SetMaximumStandardErrorBytes(std::size_t value) noexcept { m_maximumStandardErrorBytes = value; }
	void SetOutputObserver(std::shared_ptr<IBoundedProcessOutputObserver> value) noexcept
	{
		m_outputObserver = std::move(value);
	}
	void SetSensitiveStandardOutput(const bool value) noexcept { m_sensitiveStandardOutput = value; }

	[[nodiscard]] const std::wstring& ExecutablePath() const noexcept { return m_executablePath; }
	[[nodiscard]] const std::wstring& WorkingDirectory() const noexcept { return m_workingDirectory; }
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
	[[nodiscard]] const std::string& StandardInput() const noexcept { return m_standardInput; }
	[[nodiscard]] const std::vector<std::pair<std::wstring, std::wstring>>& EnvironmentOverrides() const noexcept
	{
		return m_environmentOverrides;
	}
	[[nodiscard]] const std::vector<std::wstring>& EnvironmentRemovals() const noexcept
	{
		return m_environmentRemovals;
	}
	[[nodiscard]] std::uint32_t TimeoutMilliseconds() const noexcept { return m_timeoutMilliseconds; }
	[[nodiscard]] std::size_t MaximumStandardOutputBytes() const noexcept { return m_maximumStandardOutputBytes; }
	[[nodiscard]] std::size_t MaximumStandardErrorBytes() const noexcept { return m_maximumStandardErrorBytes; }
	[[nodiscard]] const std::shared_ptr<IBoundedProcessOutputObserver>& OutputObserver() const noexcept
	{
		return m_outputObserver;
	}
	[[nodiscard]] bool SensitiveStandardOutput() const noexcept { return m_sensitiveStandardOutput; }

private:
	std::wstring m_executablePath;
	std::wstring m_workingDirectory;
	std::vector<std::wstring> m_arguments;
	std::string m_standardInput;
	std::vector<std::pair<std::wstring, std::wstring>> m_environmentOverrides;
	std::vector<std::wstring> m_environmentRemovals;
	std::shared_ptr<IBoundedProcessOutputObserver> m_outputObserver;
	bool m_sensitiveStandardOutput{ false };
	std::uint32_t m_timeoutMilliseconds{ 15000 };
	std::size_t m_maximumStandardOutputBytes{ 4u * 1024u * 1024u };
	std::size_t m_maximumStandardErrorBytes{ 64u * 1024u };
};

class BoundedProcessResult final {
public:
	explicit BoundedProcessResult(EBoundedProcessStatus status) noexcept : m_status(status) {}
	//! A result built for a request that marked its standard output sensitive
	//! zeroes whatever output bytes it still owns before they are freed. Bytes
	//! already handed out through TakeStandardOutput are the caller's to scrub.
	~BoundedProcessResult()
	{
		if (m_sensitiveStandardOutput && !m_standardOutput.empty()) {
			::SecureZeroMemory(m_standardOutput.data(), m_standardOutput.size());
		}
	}
	BoundedProcessResult(BoundedProcessResult&&) noexcept = default;
	BoundedProcessResult& operator=(BoundedProcessResult&&) noexcept = default;
	BoundedProcessResult(const BoundedProcessResult&) = delete;
	BoundedProcessResult& operator=(const BoundedProcessResult&) = delete;
	[[nodiscard]] EBoundedProcessStatus Status() const noexcept { return m_status; }
	[[nodiscard]] int ExitCode() const noexcept { return m_exitCode; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardOutput() const noexcept { return m_standardOutput; }
	[[nodiscard]] const std::vector<std::uint8_t>& StandardError() const noexcept { return m_standardError; }
	[[nodiscard]] bool Succeeded() const noexcept { return m_status == EBoundedProcessStatus::Succeeded; }
	//! Moves the captured standard output out without copying; the result is
	//! left empty and no longer owns the bytes.
	[[nodiscard]] std::vector<std::uint8_t> TakeStandardOutput() noexcept
	{
		return std::move(m_standardOutput);
	}

private:
	EBoundedProcessStatus m_status{ EBoundedProcessStatus::InvalidRequest };
	int m_exitCode{ -1 };
	std::vector<std::uint8_t> m_standardOutput;
	std::vector<std::uint8_t> m_standardError;
	bool m_sensitiveStandardOutput{ false };

	friend BoundedProcessResult RunBoundedProcess(const BoundedProcessRequest&, HANDLE);
};

inline constexpr std::size_t kMaximumBoundedProcessArguments = 128;
inline constexpr std::size_t kMaximumBoundedProcessArgumentLength = 32768;
inline constexpr std::size_t kMaximumBoundedProcessCommandLineLength = 32000;
inline constexpr std::size_t kMaximumBoundedProcessStandardInputBytes = 4u * 1024u * 1024u;
// Overrides plus removals share this one bound. The gh ambient scrub list
// alone is 30 removals and a token-bearing call adds up to five overrides, so
// 32 could not express a real gh invocation; 64 keeps the list a fixed bound.
inline constexpr std::size_t kMaximumBoundedProcessEnvironmentOverrides = 64;
inline constexpr std::size_t kMaximumBoundedProcessOutputBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaximumBoundedProcessErrorBytes = 4u * 1024u * 1024u;

[[nodiscard]] std::wstring QuoteWindowsArgument(std::wstring_view value);
[[nodiscard]] std::wstring BuildWindowsCommandLine(
	std::wstring_view executable, const std::vector<std::wstring>& arguments);
[[nodiscard]] bool IsExecutableBoundedProcessRequest(const BoundedProcessRequest& request) noexcept;

//! Run one exact executable with bounded streams, deadline, cancellation, and
//! kill-on-close ownership for its complete process tree.
[[nodiscard]] BoundedProcessResult RunBoundedProcess(const BoundedProcessRequest& request, HANDLE stop);

namespace detail {
//! Builds the exact environment block RunBoundedProcess passes to
//! CreateProcessW. Declared here so tests can observe its single-allocation
//! guarantee; the block may carry secret overrides, so callers keep it inside
//! the same scrubbed lifetime the runner uses.
[[nodiscard]] std::vector<wchar_t> BuildEnvironmentBlock(
	const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
	const std::vector<std::wstring>& removals);
} // namespace detail

} // namespace platform::process
