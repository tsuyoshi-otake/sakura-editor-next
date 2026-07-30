/*! @file
	@brief Node.js 拡張ホストプロセスの安全な起動と所有
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONHOSTPROCESS_3131E7DB_7315_49ED_B3AB_450A54A544DB_H_
#define SAKURA_CEXTENSIONHOSTPROCESS_3131E7DB_7315_49ED_B3AB_450A54A544DB_H_
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SExtensionHostLaunchOptions {
	std::filesystem::path nodeExecutable;
	std::filesystem::path hostBundle;
	std::filesystem::path securityShim;
	std::filesystem::path workingDirectory;
	std::wstring profileHash;
	std::wstring bootId;
	std::wstring pipeName;
	std::uint64_t generation = 0;
	std::uint32_t brokerProcessId = 0;
	bool developerInspect = false;
};

struct SExtensionHostProcessStartResult {
	bool success = false;
	std::uint32_t processId = 0;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;
};

class IExtensionHostProcess {
public:
	virtual ~IExtensionHostProcess() = default;
	virtual SExtensionHostProcessStartResult Start(const SExtensionHostLaunchOptions& options) = 0;
	virtual std::optional<std::uint32_t> PollExitCode() const noexcept = 0;
	virtual bool WaitForExit(std::chrono::milliseconds timeout) noexcept = 0;
	virtual void Terminate(std::uint32_t exitCode) noexcept = 0;
	virtual std::uint32_t GetProcessId() const noexcept = 0;
};

class CExtensionHostProcess final : public IExtensionHostProcess {
public:
	using EnvironmentEntry = std::pair<std::wstring, std::wstring>;

	CExtensionHostProcess();
	~CExtensionHostProcess() override;
	CExtensionHostProcess(const CExtensionHostProcess&) = delete;
	CExtensionHostProcess& operator=(const CExtensionHostProcess&) = delete;

	SExtensionHostProcessStartResult Start(const SExtensionHostLaunchOptions& options) override;
	std::optional<std::uint32_t> PollExitCode() const noexcept override;
	bool WaitForExit(std::chrono::milliseconds timeout) noexcept override;
	void Terminate(std::uint32_t exitCode) noexcept override;
	std::uint32_t GetProcessId() const noexcept override;

	static std::wstring QuoteWindowsArgument(std::wstring_view argument);
	static std::vector<wchar_t> BuildSanitizedEnvironmentBlock(
		const std::vector<EnvironmentEntry>& inherited,
		const std::vector<EnvironmentEntry>& trustedOverrides);

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

#endif /* SAKURA_CEXTENSIONHOSTPROCESS_3131E7DB_7315_49ED_B3AB_450A54A544DB_H_ */
