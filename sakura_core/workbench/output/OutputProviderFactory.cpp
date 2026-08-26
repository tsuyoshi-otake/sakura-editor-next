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
#include <limits>
#include <utility>

namespace workbench::output {
namespace {

OutputProviderFactoryResult Failed(
	const EOutputProviderKind kind,
	const EOutputProviderFactoryStatus status,
	std::string diagnostic,
	OutputProviderHealthSnapshot health)
{
	health.kind = kind;
	health.factoryStatus = status;
	health.available = false;
	if (status != EOutputProviderFactoryStatus::Created
		&& (health.lifecycle == EOutputProviderLifecycle::NotAttempted
			|| health.lifecycle == EOutputProviderLifecycle::Ready)) {
		health.lifecycle = EOutputProviderLifecycle::Unavailable;
	}
	return {
		.status = status,
		.kind = kind,
		.provider = nullptr,
		.diagnostic = std::move(diagnostic),
		.health = health,
	};
}

[[nodiscard]] OutputProviderHealthSnapshot InitialHealth(
	const EOutputProviderKind kind,
	const bool testOverrideActive = false) noexcept
{
	OutputProviderHealthSnapshot health;
	health.kind = kind;
	health.initializationStage = EOutputProviderInitializationStage::FactorySelection;
	health.compiledIn = kind == EOutputProviderKind::Cpp;
	health.testOverrideActive = testOverrideActive;
	health.counters.initializationAttempts = 1;
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	if (kind == EOutputProviderKind::Rust) health.compiledIn = true;
#endif
	return health;
}

[[nodiscard]] OutputProviderHealthSnapshot ReadyHealth(
	const EOutputProviderKind kind,
	const bool testOverrideActive) noexcept
{
	auto health = InitialHealth(kind, testOverrideActive);
	health.factoryStatus = EOutputProviderFactoryStatus::Created;
	health.lifecycle = EOutputProviderLifecycle::Ready;
	health.initializationStage = EOutputProviderInitializationStage::Ready;
	health.available = true;
	return health;
}

[[nodiscard]] OutputProviderHealthSnapshot InitializationFailureHealth(
	const EOutputProviderKind kind,
	const bool testOverrideActive) noexcept
{
	auto health = InitialHealth(kind, testOverrideActive);
	health.lifecycle = EOutputProviderLifecycle::Unavailable;
	health.initializationStage = EOutputProviderInitializationStage::ProviderConstruction;
	health.fault = EOutputProviderFault::Initialization;
	health.failureBoundary = EOutputProviderBoundary::Factory;
	health.lastBoundary = EOutputProviderBoundary::Factory;
	health.counters.boundaryFailures = 1;
	return health;
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
		bool testOverrideActive{};
		switch (request.kind) {
		case EOutputProviderKind::Cpp:
			creator = dependencies.testCppCreator;
			if (!creator) {
				return {
					.status = EOutputProviderFactoryStatus::Created,
					.kind = request.kind,
					.provider = std::make_unique<OutputService>(request.limits),
					.diagnostic = {},
					.health = ReadyHealth(request.kind, false),
				};
			}
			testOverrideActive = true;
			break;
		case EOutputProviderKind::Rust:
			if (!dependencies.testRustCreator) {
		#if defined(SAKURA_OUTPUT_BACKEND_RUST)
			auto provider = std::make_unique<OutputServiceRustProvider>(request.limits);
			auto health = provider->Health();
			if (!provider->IsAvailable()) {
				return Failed(request.kind, EOutputProviderFactoryStatus::Unavailable,
					"Rust Output provider is unavailable", health);
			}
			health.factoryStatus = EOutputProviderFactoryStatus::Created;
			return {
				.status = EOutputProviderFactoryStatus::Created,
				.kind = request.kind,
				.provider = std::move(provider),
				.diagnostic = {},
				.health = health,
			};
		#else
			auto health = InitialHealth(request.kind);
			health.lifecycle = EOutputProviderLifecycle::Unavailable;
			health.fault = EOutputProviderFault::Unavailable;
			health.failureBoundary = EOutputProviderBoundary::Factory;
			health.lastBoundary = EOutputProviderBoundary::Factory;
			health.counters.boundaryFailures = 1;
				return Failed(request.kind, EOutputProviderFactoryStatus::Unavailable,
					"Rust Output provider is unavailable", health);
		#endif
			}
			creator = dependencies.testRustCreator;
			testOverrideActive = true;
			break;
		default:
		{
			auto health = InitialHealth(request.kind);
			health.fault = EOutputProviderFault::Initialization;
			health.failureBoundary = EOutputProviderBoundary::Factory;
			health.lastBoundary = EOutputProviderBoundary::Factory;
			health.counters.boundaryFailures = 1;
			return Failed(request.kind, EOutputProviderFactoryStatus::InvalidSelection,
				"Output provider selection is invalid", health);
		}
		}

		auto provider = creator(request.limits);
		if (!provider) {
			return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
				request.kind == EOutputProviderKind::Rust
					? "Rust Output provider initialization failed"
					: "C++ Output provider initialization failed",
				InitializationFailureHealth(request.kind, testOverrideActive));
		}
		const auto providerHealth = provider->Health();
		if (providerHealth.kind != request.kind) {
			auto health = InitializationFailureHealth(request.kind, testOverrideActive);
			health.fault = EOutputProviderFault::AbiContract;
			return Failed(request.kind, EOutputProviderFactoryStatus::InvalidSelection,
				"Output provider creator returned a provider with the wrong kind", health);
		}
		if (!providerHealth.available || providerHealth.lifecycle != EOutputProviderLifecycle::Ready) {
			auto health = providerHealth;
			health.kind = request.kind;
			health.factoryStatus = EOutputProviderFactoryStatus::InitializationFailed;
			health.lifecycle = EOutputProviderLifecycle::Unavailable;
			health.available = false;
			health.testOverrideActive = testOverrideActive;
			if (health.initializationStage == EOutputProviderInitializationStage::NotStarted) {
				health.initializationStage = EOutputProviderInitializationStage::ProviderConstruction;
			}
			if (health.fault == EOutputProviderFault::None) health.fault = EOutputProviderFault::Initialization;
			health.lastBoundary = EOutputProviderBoundary::Factory;
			health.failureBoundary = EOutputProviderBoundary::Factory;
			if (health.counters.boundaryFailures != std::numeric_limits<std::uint64_t>::max()) {
				++health.counters.boundaryFailures;
			}
			return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
				"Output provider creator returned an unavailable provider", health);
		}
		auto health = providerHealth;
		health.kind = request.kind;
		health.factoryStatus = EOutputProviderFactoryStatus::Created;
		health.testOverrideActive = testOverrideActive;
		health.compiledIn = InitialHealth(request.kind, testOverrideActive).compiledIn;
		if (health.initializationStage == EOutputProviderInitializationStage::NotStarted) {
			health.initializationStage = EOutputProviderInitializationStage::Ready;
		}
		if (health.counters.initializationAttempts == 0) health.counters.initializationAttempts = 1;
		return {
			.status = EOutputProviderFactoryStatus::Created,
			.kind = request.kind,
			.provider = std::move(provider),
			.diagnostic = {},
			.health = health,
		};
	}
	catch (const std::exception&) {
		// The exception payload is optional diagnostics. Do not copy exception.what()
		// here: this catch is part of the typed fail-closed boundary and constructing
		// a richer string could throw again while reporting the original failure.
		return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
			request.kind == EOutputProviderKind::Rust
				? "Rust Output provider initialization threw"
				: "C++ Output provider initialization threw",
			InitializationFailureHealth(request.kind,
				request.kind == EOutputProviderKind::Rust
					? static_cast<bool>(dependencies.testRustCreator)
					: static_cast<bool>(dependencies.testCppCreator)));
	}
	catch (...) {
		return Failed(request.kind, EOutputProviderFactoryStatus::InitializationFailed,
			request.kind == EOutputProviderKind::Rust
				? "Rust Output provider initialization threw"
				: "C++ Output provider initialization threw",
			InitializationFailureHealth(request.kind,
				request.kind == EOutputProviderKind::Rust
					? static_cast<bool>(dependencies.testRustCreator)
					: static_cast<bool>(dependencies.testCppCreator)));
	}
}

} // namespace workbench::output
