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

//! Lifecycle port between contribution ownership and isolated process I/O.
//! Implementations own every resource acquired by Start, including failures.
class ISenpEffectRuntime {
public:
	virtual ~ISenpEffectRuntime() = default;
	[[nodiscard]] virtual InvocationAdmission Start() = 0;
	[[nodiscard]] virtual InvocationAdmission Submit(effect::OperationContext context, effect::Event event,
		CSenpRuntimeSession::Time deadline) = 0;
	virtual bool Cancel(std::wstring_view operationId) = 0;
	virtual void Stop(effect::StopReason reason) = 0;
	virtual void Join() = 0;
	[[nodiscard]] virtual std::optional<InvocationResult> TakeCompleted() = 0;
	[[nodiscard]] virtual EffectRuntimeSnapshot Snapshot() const = 0;
};

//! One worker and one job per owner. Paint/input callers only enqueue or drain;
//! Join belongs to explicit owner teardown. A stopped instance never restarts.
class CSenpEffectRuntime final : public ISenpEffectRuntime {
public:
	explicit CSenpEffectRuntime(EffectRuntimeLaunch launch);
	~CSenpEffectRuntime() override;
	CSenpEffectRuntime(const CSenpEffectRuntime&) = delete;
	CSenpEffectRuntime& operator=(const CSenpEffectRuntime&) = delete;
	[[nodiscard]] InvocationAdmission Start() override;
	[[nodiscard]] InvocationAdmission Submit(effect::OperationContext context, effect::Event event,
		CSenpRuntimeSession::Time deadline) override;
	bool Cancel(std::wstring_view operationId) override;
	void Stop(effect::StopReason reason) override;
	void Join() override;
	[[nodiscard]] std::optional<InvocationResult> TakeCompleted() override;
	[[nodiscard]] EffectRuntimeSnapshot Snapshot() const override;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace senp
