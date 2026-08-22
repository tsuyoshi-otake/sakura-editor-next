/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/WorkerRetirementService.h"

namespace markdown {

//! Markdown-facing name for the shared admission-bounded retirement service.
class MarkdownPreviewWorkerRetirement final {
public:
	using Reservation = workbench::WorkerRetirementService::Reservation;

	static MarkdownPreviewWorkerRetirement& Instance()
	{
		static MarkdownPreviewWorkerRetirement retirement;
		return retirement;
	}

	[[nodiscard]] std::optional<Reservation> TryReserve() noexcept
	{
		return workbench::WorkerRetirementService::Instance().TryReserve();
	}

	template <typename Lifetime>
	workbench::WorkerRetirementStatus Retire(std::jthread&& worker,
		Reservation&& reservation, std::shared_ptr<Lifetime> lifetime) noexcept
	{
		return workbench::WorkerRetirementService::Instance().Retire(
			std::move(worker), std::move(reservation), std::move(lifetime));
	}

	[[nodiscard]] std::size_t ReservedOrPendingCount() const noexcept
	{
		return workbench::WorkerRetirementService::Instance().ReservedOrPendingCount();
	}

private:
	MarkdownPreviewWorkerRetirement() = default;
};

} // namespace markdown
