/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/output/IOutputService.h"

namespace workbench::output {

/*!
	@brief Thread-safe pure output-channel model, intentionally independent of HWNDs, files and extension RPC.

	Each accepted mutation has a caller-supplied operation ID and optional expected service revision. Exact replay
	is accepted once remembered; reuse of an operation ID with a different request is a conflict. Notifications are
	queued under the model lock and delivered after unlocking, so listeners can safely mutate the service.
*/
class OutputService final : public IOutputService {
public:
	explicit OutputService(OutputServiceLimits limits = {});
	~OutputService() override;

	OutputService(const OutputService&) = delete;
	OutputService& operator=(const OutputService&) = delete;

	[[nodiscard]] OutputOperationResult CreateChannel(const OutputCreateChannelRequest& request) override;
	[[nodiscard]] OutputOperationResult AppendOutput(const OutputTextMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult ReplaceOutput(const OutputTextMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult AppendLog(const OutputLogMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Clear(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Show(const OutputShowChannelRequest& request) override;
	[[nodiscard]] OutputOperationResult Hide(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult Dispose(const OutputChannelMutationRequest& request) override;
	[[nodiscard]] OutputOperationResult DisposeOwner(const OutputDisposeOwnerRequest& request) override;
	//! External callers wait for active listener callbacks; a reentrant listener Stop returns deferred.
	[[nodiscard]] OutputOperationResult Stop() noexcept override;

	[[nodiscard]] OutputProviderHealthSnapshot Health() const noexcept override;
	[[nodiscard]] OutputServiceSnapshot Snapshot() const override;
	[[nodiscard]] std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener listener) override;
	void Unsubscribe(OutputServiceSubscriptionId subscriptionId) noexcept override;

	//! Atomically captures a service snapshot, current feed cursor, and a future-only feed subscription.
	//! A returned Live subscription starts after the returned cursor. A Gap event is terminal and requires
	//! resnapshot/rebootstrap; it is never silently converted into a partial stream.
	[[nodiscard]] std::optional<OutputAcceptedCommitBootstrap> SubscribeAcceptedCommits(OutputAcceptedCommitListener listener);
	//! Removes a feed subscription without waiting. Stop() is the lifetime fence for an owner that is
	//! destroying a callback target; an already-active copied callback may finish after this returns.
	void UnsubscribeAcceptedCommits(OutputAcceptedCommitSubscriptionId subscriptionId) noexcept;

private:
	struct Impl;
	Impl* m_impl;
};

} // namespace workbench::output
