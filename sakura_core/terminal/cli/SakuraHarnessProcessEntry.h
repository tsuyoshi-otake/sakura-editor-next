/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/cli/SakuraHarnessCli.h"

#include <optional>

namespace terminal::cli {

//! Reads the exact child environment contract with bounded scans. Endpoint
//! broker identity is intentionally not inferred from the public descriptor.
[[nodiscard]] std::optional<SakuraHarnessEnvironment> ReadSakuraHarnessEnvironment() noexcept;

//! Shared process entry for sakura-harness.exe. No shell/re-exec/PATH lookup.
int SakuraHarnessCliMain(int argc, wchar_t* const* argv) noexcept;

} // namespace terminal::cli
