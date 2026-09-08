/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpOwnerRequests.h"
#include <deque>
#include <map>

namespace {
using namespace senp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class Runtime final : public ISenpEffectRuntime {
public:
	explicit Runtime(EffectRuntimeLaunch launch) : m_launch(std::move(launch)) {}
	InvocationAdmission Start() override
	{
		m_state.phase = RuntimePhase::Active;
		auto context = m_launch.context;
		context.operationId = L"activation";
		m_results.push_back({ context, true, InvocationStatus::EffectsReady });
		return { AdmissionStatus::Accepted, context.operationId };
	}
	InvocationAdmission Submit(effect::OperationContext context, effect::Event,
		CSenpRuntimeSession::Time) override
	{
		if (m_submitStatus != AdmissionStatus::Accepted) return { m_submitStatus };
		context.operationId = L"request." + std::to_wstring(++m_nextOperation);
		m_pending.emplace(context.operationId, context);
		if (m_autoComplete) Complete(context.operationId, InvocationStatus::EffectsReady);
		return { AdmissionStatus::Accepted, context.operationId };
	}
	bool Cancel(std::wstring_view operationId) override
	{
		const auto found = m_pending.find(std::wstring(operationId));
		if (found == m_pending.end()) return false;
		Complete(found->first, InvocationStatus::Cancelled);
		return true;
	}
	void Stop(effect::StopReason) override
	{
		m_state.phase = RuntimePhase::Stopped;
		m_state.workerExited = true;
		m_state.processExitConfirmed = true;
	}
	void Join() override {}
	std::optional<InvocationResult> TakeCompleted() override
	{
		if (m_results.empty()) return {};
		auto result = std::move(m_results.front());
		m_results.pop_front();
		return result;
	}
	EffectRuntimeSnapshot Snapshot() const override { return m_state; }
	void SetSubmitStatus(AdmissionStatus value) noexcept { m_submitStatus = value; }
	void SetAutoComplete(bool value) noexcept { m_autoComplete = value; }
private:
	void Complete(const std::wstring& operationId, InvocationStatus status)
	{
		const auto found = m_pending.find(operationId);
		if (found == m_pending.end()) return;
		m_results.push_back({ found->second, false, status });
		m_pending.erase(found);
	}
	EffectRuntimeLaunch m_launch;
	EffectRuntimeSnapshot m_state;
	std::deque<InvocationResult> m_results;
	std::map<std::wstring, effect::OperationContext> m_pending;
	std::int64_t m_nextOperation{};
	AdmissionStatus m_submitStatus{ AdmissionStatus::Accepted };
	bool m_autoComplete{ true };
};

class PublicationState final {
public:
	void Bind(CSenpOwnerRequests& requests)
	{
		m_publish = [&requests](InvocationResult result) { return requests.Publish(std::move(result)); };
	}
	bool Apply(InvocationResult result) noexcept
	{
		return result.activation || (m_publish && m_publish(std::move(result)));
	}
	void Clear() noexcept { m_publish = {}; }
private:
	std::function<bool(InvocationResult)> m_publish;
};

class Publication final : public ISenpOwnerPublication {
public:
	explicit Publication(std::shared_ptr<PublicationState> state) : m_state(std::move(state)) {}
	bool Validate(const InvocationResult&) const noexcept override { return true; }
	bool Commit() noexcept override { return true; }
	bool Apply(InvocationResult result) noexcept override { return m_state->Apply(std::move(result)); }
	void Revoke(effect::StopReason) noexcept override { m_state->Clear(); }
private:
	std::shared_ptr<PublicationState> m_state;
};

class OwnerFixture final {
public:
	OwnerFixture() : m_owners([this](EffectRuntimeLaunch launch) {
		auto runtime = std::make_unique<Runtime>(std::move(launch));
		m_runtime = runtime.get();
		return runtime;
	}) {}
	ContributionOwnerIdentity Activate()
	{
		auto result = m_owners.Prepare({ .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"test.sample",
			.context = { .workspaceRevision = 3, .accountGeneration = 4 } }, std::wstring(64, L'b'),
			[this](const ContributionOwnerIdentity&, const ContributionOwnerIdentity*) {
				return std::make_unique<Publication>(m_publication);
			}, Clock::now());
		EXPECT_EQ(OwnerChangeStatus::Accepted, result.status);
		m_owners.Poll(Clock::now());
		auto terminal = m_owners.TakeTransition();
		EXPECT_TRUE(terminal);
		if (terminal) EXPECT_EQ(OwnerChangeStatus::Activated, terminal->status);
		return result.owner;
	}
	CSenpContributionOwners& Owners() noexcept { return m_owners; }
	Runtime& Host() noexcept { return *m_runtime; }
	void Bind(CSenpOwnerRequests& requests) { m_publication->Bind(requests); }
private:
	Runtime* m_runtime{};
	std::shared_ptr<PublicationState> m_publication = std::make_shared<PublicationState>();
	CSenpContributionOwners m_owners;
};

TEST(SenpOwnerRequests, MintsOnlyAcceptedGenerationsAndDefersPublication)
{
	OwnerFixture fixture;
	const auto owner = fixture.Activate();
	CSenpOwnerRequests requests(fixture.Owners(), owner);
	fixture.Bind(requests);
	fixture.Host().SetSubmitStatus(AdmissionStatus::Busy);
	EXPECT_EQ(AdmissionStatus::Busy,
		requests.Submit(effect::TreeRequest{ L"sample.projects" }, Clock::now() + 1s).Status());
	EXPECT_EQ(0, requests.Snapshot().LastRequestGeneration());
	fixture.Host().SetSubmitStatus(AdmissionStatus::Accepted);
	const auto admitted = requests.Submit(effect::TreeRequest{ L"sample.projects" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, admitted.Status());
	EXPECT_EQ(1, admitted.Context().requestGeneration);
	EXPECT_EQ(1U, requests.Snapshot().Invocations());
	EXPECT_FALSE(requests.TakeCompleted());
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(1U, requests.Snapshot().Completed());
	auto completed = requests.TakeCompleted();
	ASSERT_TRUE(completed);
	EXPECT_EQ(admitted.Context(), completed->context);
	EXPECT_TRUE(requests.Finish(completed->context));
	EXPECT_EQ(0U, requests.Snapshot().Requests());
}

TEST(SenpOwnerRequests, RetainsLineageAcrossDerivedInvocations)
{
	OwnerFixture fixture;
	const auto owner = fixture.Activate();
	CSenpOwnerRequests requests(fixture.Owners(), owner);
	fixture.Bind(requests);
	const auto root = requests.Submit(effect::TreeRequest{ L"sample.projects" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, root.Status());
	fixture.Owners().Poll(Clock::now());
	ASSERT_TRUE(requests.TakeCompleted());
	const auto derived = requests.SubmitDerived(root.Context(),
		effect::ToolCompleted{ L"read.1", effect::CompletionStatus::Succeeded, L"{}", L"" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, derived.Status());
	EXPECT_NE(root.Context().operationId, derived.Context().operationId);
	EXPECT_EQ(root.Context().requestGeneration, derived.Context().requestGeneration);
	EXPECT_FALSE(requests.Finish(root.Context()));
	fixture.Owners().Poll(Clock::now());
	auto terminal = requests.TakeCompleted();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(derived.Context(), terminal->context);
	EXPECT_TRUE(requests.Finish(root.Context()));
	EXPECT_EQ(AdmissionStatus::InvalidRequest,
		requests.SubmitDerived(root.Context(), effect::TreeRequest{ L"sample.projects" }, Clock::now() + 1s).Status());
}

TEST(SenpOwnerRequests, CancellationSuppressesEveryDerivedTerminal)
{
	OwnerFixture fixture;
	const auto owner = fixture.Activate();
	fixture.Host().SetAutoComplete(false);
	CSenpOwnerRequests requests(fixture.Owners(), owner);
	fixture.Bind(requests);
	const auto root = requests.Submit(effect::DocumentRequest{ L"sample.detail" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, root.Status());
	const auto derived = requests.SubmitDerived(root.Context(),
		effect::ToolCompleted{ L"read.1", effect::CompletionStatus::Succeeded, L"{}", L"" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, derived.Status());
	ASSERT_TRUE(requests.Cancel(root.Context()));
	EXPECT_FALSE(requests.Cancel(root.Context()));
	fixture.Owners().Poll(Clock::now());
	EXPECT_FALSE(requests.TakeCompleted());
	EXPECT_EQ(0U, requests.Snapshot().Requests());
	const auto next = requests.Submit(effect::DocumentRequest{ L"sample.detail" }, Clock::now() + 1s);
	ASSERT_EQ(AdmissionStatus::Accepted, next.Status());
	EXPECT_EQ(2, next.Context().requestGeneration);
	requests.Close();
	EXPECT_FALSE(requests.IsCurrent());
	EXPECT_TRUE(requests.Snapshot().Closed());
}
} // namespace
