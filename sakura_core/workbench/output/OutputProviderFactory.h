/*! @file
 * @brief One-shot Output provider selection and construction boundary.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/output/IOutputService.h"
#include "workbench/output/OutputProviderTypes.h"

#include <functional>
#include <memory>
#include <string>

namespace workbench::output {

//! A provider creator is an explicit composition seam.  The creator must
//! return exactly one fully initialized provider or null.  It must not return
//! a C++ provider for a Rust request.
using OutputProviderCreator = std::function<std::unique_ptr<IOutputService>(
	const OutputServiceLimits&)>;

struct OutputProviderFactoryDependencies final {
	//! Test-only C++ creator override. Production composition must leave this
	//! empty so health attribution cannot mislabel another implementation.
	OutputProviderCreator testCppCreator;
	//! Test-only Rust creator override. Without one, a
	//! Rust-enabled build constructs OutputServiceRustProvider directly; a
	//! non-Rust build reports typed unavailability with no C++ fallback.
	OutputProviderCreator testRustCreator;
};

struct OutputProviderFactoryRequest final {
	EOutputProviderKind kind{ EOutputProviderKind::Cpp };
	OutputServiceLimits limits{};
};

struct OutputProviderFactoryResult final {
	EOutputProviderFactoryStatus status{ EOutputProviderFactoryStatus::NotAttempted };
	EOutputProviderKind kind{ EOutputProviderKind::Cpp };
	std::unique_ptr<IOutputService> provider;
	std::string diagnostic;
	OutputProviderHealthSnapshot health{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EOutputProviderFactoryStatus::Created && provider != nullptr;
	}
};

//! Returns the compile-selected default.  Only the output backend selector
//! controls this value; UTF-16/SIMD selectors are intentionally ignored.
[[nodiscard]] EOutputProviderKind DefaultOutputProviderKind() noexcept;

//! Builds exactly the requested provider.  This function never probes one
//! provider and then substitutes another provider on failure.
[[nodiscard]] OutputProviderFactoryResult CreateOutputProvider(
	const OutputProviderFactoryRequest& request,
	const OutputProviderFactoryDependencies& dependencies = {});

} // namespace workbench::output
