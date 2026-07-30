/*! @file @brief Runtime selection of x64 SIMD implementations. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>

namespace CpuDispatch
{
enum class Isa : std::uint8_t {
	Avx = 1,
	Avx2 = 2,
	Avx512 = 3,
};

struct Capabilities {
	bool avx{};
	bool avx2{};
	bool avx512{};
};

using FindCrOrLfFunction = std::size_t (*)(const char* data, std::size_t length) noexcept;

struct Dispatch {
	Isa isa{Isa::Avx};
	Capabilities capabilities{};
	FindCrOrLfFunction findCrOrLf{};
	std::int64_t initializationTicks{};
};

// Detects CPU and OS extended-state support once and freezes the process-wide
// dispatch table. Call during wWinMain startup before worker threads begin.
const Dispatch& Initialize() noexcept;
const Dispatch& Get() noexcept;

// Pure selection helper used to lock the fallback order in unit tests.
Isa SelectBestIsa(const Capabilities& capabilities) noexcept;
const char* GetIsaName(Isa isa) noexcept;

namespace Testing
{
// Returns nullptr when the requested implementation is unsafe on this machine.
FindCrOrLfFunction GetSupportedFindCrOrLf(Isa isa) noexcept;
}
}
