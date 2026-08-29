/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <string>
#include <string_view>

namespace terminal::cli {

//! Complete process-facing result. Output is kept as UTF-8 without a BOM.
struct SakuraCliProcessResult final {
	int exitCode{ 1 };
	std::string stdoutText;
	std::string stderrText;

	[[nodiscard]] bool Succeeded() const noexcept { return exitCode == 0; }
};

//! Sink used by the thin OS entry point. Implementations must write bytes as-is.
class ISakuraCliOutput {
public:
	virtual ~ISakuraCliOutput() = default;
	[[nodiscard]] virtual bool WriteStdout(std::string_view bytes) noexcept = 0;
	[[nodiscard]] virtual bool WriteStderr(std::string_view bytes) noexcept = 0;
};

[[nodiscard]] inline bool WriteSakuraCliResult(
	const SakuraCliProcessResult& result, ISakuraCliOutput& output) noexcept
{
	if (!result.stdoutText.empty() && !output.WriteStdout(result.stdoutText)) return false;
	if (!result.stderrText.empty() && !output.WriteStderr(result.stderrText)) return false;
	return true;
}

} // namespace terminal::cli
