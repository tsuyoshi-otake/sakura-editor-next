/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/output/IOutputService.h"
#include "workbench/output/OutputProviderTypes.h"
#include "workbench/output/OutputServiceRustProviderAbi.h"

#include <cstdint>
#include <memory>

namespace workbench::output {

enum class EOutputServiceRustProviderAvailability : std::uint8_t {
	Unavailable,
	Available,
};

enum class EOutputServiceRustProviderState : std::uint8_t {
	Unavailable,
	Ready,
	Faulted,
	Stopped,
};

enum class EOutputServiceRustProviderFault : std::uint8_t {
	None,
	Unavailable,
	AbiFailure,
	FfiFailure,
	SnapshotFailure,
	CallbackFailure,
	DestroyFailure,
};

//! A copied diagnostic view of the Rust provider lifecycle and last boundary result.
struct OutputServiceRustProviderDiagnostics final {
	EOutputServiceRustProviderAvailability availability{
		EOutputServiceRustProviderAvailability::Unavailable };
	EOutputServiceRustProviderState state{ EOutputServiceRustProviderState::Unavailable };
	EOutputProviderInitializationStage initializationStage{
		EOutputProviderInitializationStage::NotStarted };
	EOutputProviderBoundary lastBoundary{ EOutputProviderBoundary::None };
	EOutputProviderBoundary failureBoundary{ EOutputProviderBoundary::None };
	SakuraOutputProviderStatus lastFfiStatus{ SakuraOutputProviderStatus::InternalError };
	bool hasLastOperation{};
	SakuraOutputProviderOperationStatus lastOperationStatus{
		SakuraOutputProviderOperationStatus::Stopped };
	SakuraOutputProviderReason lastOperationReason{ SakuraOutputProviderReason::None };
	std::uint64_t lastOperationRevision{};
	EOutputServiceRustProviderFault fault{ EOutputServiceRustProviderFault::Unavailable };
	OutputProviderHealthCounters counters{};
};

/*!
 * @brief Sole-authority Rust implementation of IOutputService.
 *
 * Rust owns channels, owner generations, operation replay, revisions, and
 * snapshots.  This adapter owns only the ABI token, copied diagnostics, and
 * the advisory C++ listener lifetime; it never mirrors channel state or
 * performs a fallback mutation.
 */
class OutputServiceRustProvider final : public IOutputService {
public:
	explicit OutputServiceRustProvider(OutputServiceLimits limits = {}) noexcept;
	~OutputServiceRustProvider() override;

	OutputServiceRustProvider(const OutputServiceRustProvider&) = delete;
	OutputServiceRustProvider& operator=(const OutputServiceRustProvider&) = delete;
	OutputServiceRustProvider(OutputServiceRustProvider&&) = delete;
	OutputServiceRustProvider& operator=(OutputServiceRustProvider&&) = delete;

	//! The selector macro is the sole compile/link adoption boundary.
	[[nodiscard]] static constexpr bool IsCompiledIn() noexcept
	{
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		return true;
#else
		return false;
#endif
	}
	[[nodiscard]] bool IsAvailable() const noexcept;
	[[nodiscard]] OutputServiceRustProviderDiagnostics Diagnostics() const noexcept;
	[[nodiscard]] OutputProviderHealthSnapshot Health() const noexcept override;

	[[nodiscard]] OutputOperationResult CreateChannel(const OutputCreateChannelRequest& request) override;
	[[nodiscard]] OutputOperationResult AppendOutput(const OutputTextMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult ReplaceOutput(const OutputTextMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult AppendLog(const OutputLogMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Clear(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Show(const OutputShowChannelRequest& request) override;
	[[nodiscard]] OutputOperationResult Hide(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Dispose(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult DisposeOwner(const OutputDisposeOwnerRequest& request) override;

	//! Stop is terminal for the provider object but retains its stopped snapshot until destruction.
	[[nodiscard]] OutputOperationResult Stop() noexcept override;

	[[nodiscard]] OutputServiceSnapshot Snapshot() const override;
	[[nodiscard]] std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener listener) override;
	void Unsubscribe(OutputServiceSubscriptionId subscriptionId) noexcept override;

private:
	struct Control;
	std::unique_ptr<Control> m_control;
};

} // namespace workbench::output
