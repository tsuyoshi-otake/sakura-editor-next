/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/output/OutputServiceTypes.h"

namespace workbench::output {

/*!
	@brief Provider-neutral authority boundary for Output and structured Log channels.

	The accepted-commit feed is intentionally not part of this boundary. It is a
	concrete `OutputService` migration feed used by observational candidates;
	authority consumers use only the mutation, snapshot, and advisory notification
	contract below. Implementations are thread-safe, return snapshots by value,
	and deliver advisory listeners outside their model lock while containing
	listener exceptions. `Unsubscribe` is non-draining; an external `Stop` is the
	callback lifetime fence, while callback-originated `Stop` reports deferred
	drain. A listener may reenter this interface but must not destroy the provider
	from inside its callback.
*/
class IOutputService {
public:
	virtual ~IOutputService() = default;

	[[nodiscard]] virtual OutputOperationResult CreateChannel(const OutputCreateChannelRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult AppendOutput(const OutputTextMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult ReplaceOutput(const OutputTextMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult AppendLog(const OutputLogMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult Clear(const OutputChannelMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult Show(const OutputShowChannelRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult Hide(const OutputChannelMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult Dispose(const OutputChannelMutationRequest& request) = 0;
	[[nodiscard]] virtual OutputOperationResult DisposeOwner(const OutputDisposeOwnerRequest& request) = 0;

	//! External callers wait for active advisory listener callbacks; a reentrant Stop returns deferred.
	[[nodiscard]] virtual OutputOperationResult Stop() noexcept = 0;

	[[nodiscard]] virtual OutputServiceSnapshot Snapshot() const = 0;
	[[nodiscard]] virtual std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener listener) = 0;
	virtual void Unsubscribe(OutputServiceSubscriptionId subscriptionId) noexcept = 0;
};

} // namespace workbench::output
