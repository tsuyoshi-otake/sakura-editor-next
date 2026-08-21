/*! @file @brief Issue #239 benchmark-only UTF-16 evidence entry point. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

namespace Utf16Phase8Benchmark
{
// Returns false when SAKURA_UTF16_BENCHMARK_MODE is unset. Benchmark modes are
// reachable only through an explicitly selected disabled GoogleTest.
bool TryRunFromEnvironment();
}
