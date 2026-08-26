/*! @file
 * @brief One-shot Output provider selection and construction boundary.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/output/OutputProviderFactory.h"

#include "workbench/output/OutputService.h"
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
#include "workbench/output/OutputServiceRustProvider.h"
#endif

#include <exception>
#include <utility>

namespace workbench::output {
namespace {

OutputProviderFactoryResult Failed(
	const EOutputProviderKind kind,
	const EOutputProviderFactoryStatus status,
	std::string diagnostic)
{
	return {
		.status = status,
		.kind = kind,
		.provider = nullptr,
		.diagnostic = std::move(diagnostic),
	};
}

} // namespace

EOutputProviderKind DefaultOutputProviderKind() noexcept
{
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	return EOutputProviderKind::Rust;
#else
	return EOutputProviderKind::Cpp;
#endif
}

OutputProviderFactoryResult CreateOutputProvider(
	const OutputProviderFactoryRequest& request,
	const OutputProviderFactoryDependencies& dependencies)
{
	try {
		OutputProviderCreator creator;
		switch (request.kind) {
		case EOutputProviderKind::Cpp:
			creator = dependencies.cppCreator;
			if (!creator) {
				return {
					.status = EOutputProviderFactoryStatus::Created,
					.kind = request.kind,
					.provider = std::make_unique<OutputService>(request.limits),
					.diagnostic = {},
				};
			}
			break;
		case EOutputProviderKind::Rust:
			if (!dependencies.rustCreator) {
		#if defined(SAKURA_OUTPUT_BACKEND_RUST)
			auto provider = std::make_unique<OutputServiceRustProvider>(request.limits);
			if (!provider->IsAvailable()) {
				return Failed(request.kind, EOutputProviderFactoryStatus::Unavailable,
					"Rust Output provider is unavailable");
			}
			return {
				.status = EOutputProviderFactoryStatus::Created,
				.kind = request.kind,
				.provider = std::move(provider),
				.diagnostic = {},
			};
		#else
				return Failed(request.kind, EOutputProviderFactoryStatus::Unavailable,
					"Rust Output provider is unavailable");
		#endif
			}
			creator = dependencies.rustCreator;
			break;
		default:
			return Failed(request.kind, EOutputProviderFactoryStatus::InvalidSelection,
				"Output provider selection is invalid");
		}

		auto provider = creator(request.limits);
		if (!provider) {
			return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
				request.kind == EOutputProviderKind::Rust
					? "Rust Output provider initialization failed"
					: "C++ Output provider initialization failed");
		}
		return {
			.status = EOutputProviderFactoryStatus::Created,
			.kind = request.kind,
			.provider = std::move(provider),
			.diagnostic = {},
		};
	}
	catch (const std::exception& exception) {
		return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
			request.kind == EOutputProviderKind::Rust
				? std::string("Rust Output provider initialization threw: ") + exception.what()
				: std::string("C++ Output provider initialization threw: ") + exception.what());
	}
	catch (...) {
		return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
			request.kind == EOutputProviderKind::Rust
				? "Rust Output provider initialization threw"
				: "C++ Output provider initialization threw");
	}
}

} // namespace workbench::output
