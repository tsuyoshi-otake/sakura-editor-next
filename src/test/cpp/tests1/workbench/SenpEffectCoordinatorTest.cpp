/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/SenpEffectCoordinator.h"

#include <deque>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class ImmediateRuntime final : public senp::ISenpEffectRuntime {
public:
	explicit ImmediateRuntime(senp::EffectRuntimeLaunch launch) : m_launch(std::move(launch)) {}
	senp::InvocationAdmission Start() override
	{
		m_state.phase = senp::RuntimePhase::Active;
		auto context = m_launch.context;
		context.operationId = L"activation";
		m_results.push_back({ context, true, senp::InvocationStatus::EffectsReady });
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	senp::InvocationAdmission Submit(senp::effect::OperationContext context,
		senp::effect::Event event, senp::CSenpRuntimeSession::Time) override
	{
		context.operationId = L"request." + std::to_wstring(++m_sequence);
		m_events.push_back(std::move(event));
		m_results.push_back({ context, false, m_nextStatus, {}, std::move(m_nextEffects) });
		m_nextStatus = senp::InvocationStatus::EffectsReady;
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	bool Cancel(std::wstring_view) override { return true; }
	void Stop(senp::effect::StopReason) override
	{
		m_state.phase = senp::RuntimePhase::Stopped;
		m_state.workerExited = true;
		m_state.processExitConfirmed = true;
	}
	void Join() override {}
	std::optional<senp::InvocationResult> TakeCompleted() override
	{
		if (m_results.empty()) return {};
		auto result = std::move(m_results.front());
		m_results.pop_front();
		return result;
	}
	senp::EffectRuntimeSnapshot Snapshot() const override { return m_state; }
	void CompleteWith(std::vector<senp::effect::Effect> effects)
	{
		m_nextEffects = std::move(effects);
	}
	void CompleteWith(senp::InvocationStatus status) noexcept { m_nextStatus = status; }
	[[nodiscard]] const std::vector<senp::effect::Event>& Events() const noexcept { return m_events; }
private:
	senp::EffectRuntimeLaunch m_launch;
	senp::EffectRuntimeSnapshot m_state;
	std::deque<senp::InvocationResult> m_results;
	std::vector<senp::effect::Event> m_events;
	std::vector<senp::effect::Effect> m_nextEffects;
	senp::InvocationStatus m_nextStatus{ senp::InvocationStatus::EffectsReady };
	std::int64_t m_sequence{};
};

class PublicationPort final {
public:
	void Bind(CSenpEffectCoordinator& coordinator)
	{
		m_publish = [&coordinator](senp::InvocationResult result) {
			return coordinator.Publish(std::move(result));
		};
	}
	bool Apply(senp::InvocationResult result) noexcept
	{
		return result.activation || (m_publish && m_publish(std::move(result)));
	}
	void Clear() noexcept { m_publish = {}; }
private:
	std::function<bool(senp::InvocationResult)> m_publish;
};

class Publication final : public senp::ISenpOwnerPublication {
public:
	explicit Publication(std::shared_ptr<PublicationPort> port) : m_port(std::move(port)) {}
	bool Validate(const senp::InvocationResult&) const noexcept override { return true; }
	bool Commit() noexcept override { return true; }
	bool Apply(senp::InvocationResult result) noexcept override { return m_port->Apply(std::move(result)); }
	void Revoke(senp::effect::StopReason) noexcept override { m_port->Clear(); }
private:
	std::shared_ptr<PublicationPort> m_port;
};

class Target final : public ISenpEffectTarget {
public:
	ESenpEffectTargetStatus Apply(const senp::effect::OperationContext& context,
		senp::effect::Effect effect, senp::CSenpRuntimeSession::Time) noexcept override
	{
		m_lastContext = context;
		m_effects.push_back(std::move(effect));
		if (m_onApply) m_onApply();
		return m_next;
	}
	bool Failed(const senp::effect::OperationContext& context, senp::InvocationStatus status,
		senp::CSenpRuntimeSession::Time) noexcept override
	{
		m_lastContext = context;
		m_failures.push_back(status);
		return true;
	}
	void SetNext(ESenpEffectTargetStatus value) noexcept { m_next = value; }
	void SetOnApply(std::function<void()> value) { m_onApply = std::move(value); }
	[[nodiscard]] const std::vector<senp::effect::Effect>& Effects() const noexcept { return m_effects; }
	[[nodiscard]] const std::vector<senp::InvocationStatus>& Failures() const noexcept { return m_failures; }
	[[nodiscard]] const senp::effect::OperationContext& LastContext() const noexcept { return m_lastContext; }
private:
	ESenpEffectTargetStatus m_next{ ESenpEffectTargetStatus::Applied };
	std::function<void()> m_onApply;
	senp::effect::OperationContext m_lastContext;
	std::vector<senp::effect::Effect> m_effects;
	std::vector<senp::InvocationStatus> m_failures;
};

class Fixture final {
public:
	Fixture() : m_owners([this](senp::EffectRuntimeLaunch launch) {
		auto runtime = std::make_unique<ImmediateRuntime>(std::move(launch));
		m_process = runtime.get();
		return runtime;
	}) {}
	senp::ContributionOwnerIdentity Activate()
	{
		auto result = m_owners.Prepare({ .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.extension",
			.context = { .workspaceRevision = 7, .accountGeneration = 9 } },
			std::wstring(64, L'b'), [this](const auto&, const auto*) {
				return std::make_unique<Publication>(m_publication);
			}, Clock::now());
		EXPECT_EQ(senp::OwnerChangeStatus::Accepted, result.status);
		m_owners.Poll(Clock::now());
		auto terminal = m_owners.TakeTransition();
		EXPECT_TRUE(terminal);
		if (terminal) EXPECT_EQ(senp::OwnerChangeStatus::Activated, terminal->status);
		return result.owner;
	}
	[[nodiscard]] ImmediateRuntime& Process() noexcept { return *m_process; }
	[[nodiscard]] PublicationPort& PublicationChannel() noexcept { return *m_publication; }
	[[nodiscard]] senp::CSenpContributionOwners& Owners() noexcept { return m_owners; }
private:
	ImmediateRuntime* m_process{};
	std::shared_ptr<PublicationPort> m_publication = std::make_shared<PublicationPort>();
	senp::CSenpContributionOwners m_owners;
};

TEST(SenpEffectCoordinator, AdaptsTreeAndDocumentRequestsAfterOwnerPoll)
{
	Fixture fixture;
	const auto owner = fixture.Activate();
	Target target;
	CSenpEffectCoordinator coordinator(fixture.Owners(), owner, target);
	fixture.PublicationChannel().Bind(coordinator);
	fixture.Process().CompleteWith({ senp::effect::PublishTreePage{
		L"sample.projects", L"", {}, L"", 1, senp::effect::PageStatus::Empty, L"" } });
	const auto tree = coordinator.Submit({ L"sample.projects", L"", L"" }, Clock::now() + 1s);
	ASSERT_EQ(senp::AdmissionStatus::Accepted, tree.status);
	EXPECT_EQ(ESenpEffectDrainStatus::Idle, coordinator.Drain(Clock::now()).Status());
	fixture.Owners().Poll(Clock::now());
	const auto drained = coordinator.Drain(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Applied, drained.Status());
	EXPECT_EQ(1U, drained.Terminals());
	ASSERT_EQ(1U, target.Effects().size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::PublishTreePage>(target.Effects()[0]));
	EXPECT_EQ(tree.context, target.LastContext());
	EXPECT_EQ(0U, coordinator.Snapshot().Requests());

	fixture.Process().CompleteWith({ senp::effect::PublishDocument{
		L"sample:details", L"Details", 1, {} } });
	const auto document = coordinator.SubmitDocument(L"sample:details", Clock::now() + 1s);
	ASSERT_EQ(senp::AdmissionStatus::Accepted, document.Status());
	EXPECT_GT(document.Context().requestGeneration, tree.context.requestGeneration);
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Applied, coordinator.Drain(Clock::now()).Status());
	ASSERT_EQ(2U, target.Effects().size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::PublishDocument>(target.Effects()[1]));
}

TEST(SenpEffectCoordinator, RetainedToolReadLineageCompletesThroughDerivedEvent)
{
	Fixture fixture;
	const auto owner = fixture.Activate();
	Target target;
	CSenpEffectCoordinator coordinator(fixture.Owners(), owner, target);
	fixture.PublicationChannel().Bind(coordinator);
	target.SetNext(ESenpEffectTargetStatus::Retained);
	fixture.Process().CompleteWith({ senp::effect::StartToolRead{ L"read.1", L"sample", L"read", {} } });
	const auto document = coordinator.SubmitDocument(L"sample:details", Clock::now() + 1s);
	ASSERT_EQ(senp::AdmissionStatus::Accepted, document.Status());
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Applied, coordinator.Drain(Clock::now()).Status());
	EXPECT_EQ(1U, coordinator.Snapshot().Requests());
	EXPECT_EQ(0U, coordinator.Snapshot().Invocations());

	target.SetNext(ESenpEffectTargetStatus::Applied);
	fixture.Process().CompleteWith({});
	const auto derived = coordinator.SubmitDerived(document.Context(),
		{ L"read.1", senp::effect::CompletionStatus::Succeeded, L"{}", L"" }, Clock::now() + 1s);
	ASSERT_EQ(senp::AdmissionStatus::Accepted, derived.Status());
	EXPECT_EQ(document.Context().requestGeneration, derived.Context().requestGeneration);
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Applied, coordinator.Drain(Clock::now()).Status());
	EXPECT_EQ(0U, coordinator.Snapshot().Requests());
	ASSERT_EQ(2U, fixture.Process().Events().size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::ToolCompleted>(fixture.Process().Events()[1]));
}

TEST(SenpEffectCoordinator, RoutesFailureAndMakesRejectedTargetTerminal)
{
	Fixture fixture;
	const auto owner = fixture.Activate();
	Target target;
	CSenpEffectCoordinator coordinator(fixture.Owners(), owner, target);
	fixture.PublicationChannel().Bind(coordinator);
	fixture.Process().CompleteWith(senp::InvocationStatus::TimedOut);
	EXPECT_TRUE(coordinator.Execute({ L"sample.open", {} }));
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Applied, coordinator.Drain(Clock::now()).Status());
	ASSERT_EQ(1U, target.Failures().size());
	EXPECT_EQ(senp::InvocationStatus::TimedOut, target.Failures()[0]);

	target.SetNext(ESenpEffectTargetStatus::Rejected);
	fixture.Process().CompleteWith({ senp::effect::OpenDocument{ L"sample:invalid" } });
	ASSERT_EQ(senp::AdmissionStatus::Accepted,
		coordinator.SubmitVisibility(L"sample.projects", true, Clock::now() + 1s).Status());
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Rejected, coordinator.Drain(Clock::now()).Status());
	EXPECT_FALSE(coordinator.IsCurrent());
	EXPECT_EQ(senp::AdmissionStatus::Unavailable,
		coordinator.SubmitDocument(L"sample:later", Clock::now() + 1s).Status());
	EXPECT_TRUE(fixture.Owners().Revoke(owner.extensionId, senp::effect::StopReason::ProtocolError));
}

TEST(SenpEffectCoordinator, ReentrantCloseDefersCleanupToTheDrainOwner)
{
	Fixture fixture;
	const auto owner = fixture.Activate();
	Target target;
	CSenpEffectCoordinator coordinator(fixture.Owners(), owner, target);
	fixture.PublicationChannel().Bind(coordinator);
	target.SetOnApply([&coordinator] { coordinator.Close(); });
	fixture.Process().CompleteWith({ senp::effect::OpenDocument{ L"sample:details" } });
	ASSERT_EQ(senp::AdmissionStatus::Accepted,
		coordinator.SubmitDocument(L"sample:details", Clock::now() + 1s).Status());
	fixture.Owners().Poll(Clock::now());
	const auto drained = coordinator.Drain(Clock::now());
	EXPECT_EQ(ESenpEffectDrainStatus::Closed, drained.Status());
	EXPECT_EQ(1U, drained.Terminals());
	EXPECT_TRUE(coordinator.Snapshot().Closed());
	EXPECT_FALSE(coordinator.IsCurrent());
}

} // namespace
} // namespace workbench
