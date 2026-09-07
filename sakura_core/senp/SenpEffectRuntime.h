/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once
#include "senp/SenpRuntimeSession.h"

namespace senp {

//! Native composition supplies management-verified paths and a fresh generation.
//! This descriptor is never deserialized from an extension event or effect.
struct EffectRuntimeLaunch final {
	std::wstring hostExecutable{};
	std::wstring modulePath{};
	std::wstring moduleSha256{};
	std::wstring extensionId{};
	effect::OperationContext context{};
	std::int64_t generation{};
};

struct EffectRuntimeSnapshot final {
	RuntimePhase phase{};
	std::size_t pending{};
	std::size_t completed{};
	std::uint32_t processId{};
	bool workerExited{};
	bool processExitConfirmed{};
};

//! One worker and one job per owner. Paint/input callers only enqueue or drain;
//! Join belongs to explicit owner teardown. A stopped instance never restarts.
class CSenpEffectRuntime final {
public:
	explicit CSenpEffectRuntime(EffectRuntimeLaunch launch);
	~CSenpEffectRuntime();
	CSenpEffectRuntime(const CSenpEffectRuntime&) = delete;
	CSenpEffectRuntime& operator=(const CSenpEffectRuntime&) = delete;
	[[nodiscard]] InvocationAdmission Start();
	[[nodiscard]] InvocationAdmission Submit(effect::OperationContext context, effect::Event event,
		CSenpRuntimeSession::Time deadline);
	bool Cancel(std::wstring_view operationId);
	void Stop(effect::StopReason reason);
	void Join();
	[[nodiscard]] std::optional<InvocationResult> TakeCompleted();
	[[nodiscard]] EffectRuntimeSnapshot Snapshot() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace senp
