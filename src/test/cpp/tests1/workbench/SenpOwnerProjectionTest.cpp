/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/SenpOwnerProjection.h"

#include <deque>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

class Runtime final : public senp::ISenpEffectRuntime {
public:
	explicit Runtime(senp::EffectRuntimeLaunch launch) : m_launch(std::move(launch)) {}
	senp::InvocationAdmission Start() override
	{
		m_state.phase = senp::RuntimePhase::Active;
		auto context = m_launch.context; context.operationId = L"activation";
		m_results.push_back({ context, true, senp::InvocationStatus::EffectsReady });
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	senp::InvocationAdmission Submit(senp::effect::OperationContext context,
		senp::effect::Event event, senp::CSenpRuntimeSession::Time) override
	{
		context.operationId = L"request." + std::to_wstring(++m_sequence);
		m_events.push_back(std::move(event));
		m_results.push_back({ context, false, m_status, {}, std::move(m_effects) });
		m_status = senp::InvocationStatus::EffectsReady;
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	bool Cancel(std::wstring_view) override { return true; }
	void Stop(senp::effect::StopReason) override
	{
		m_state.phase = senp::RuntimePhase::Stopped;
		m_state.workerExited = true; m_state.processExitConfirmed = true;
	}
	void Join() override {}
	std::optional<senp::InvocationResult> TakeCompleted() override
	{
		if (m_results.empty()) return {};
		auto result = std::move(m_results.front()); m_results.pop_front(); return result;
	}
	senp::EffectRuntimeSnapshot Snapshot() const override { return m_state; }
	void Next(std::vector<senp::effect::Effect> effects) { m_effects = std::move(effects); }
	void Next(senp::InvocationStatus status) noexcept { m_status = status; }
	[[nodiscard]] const std::vector<senp::effect::Event>& Events() const noexcept { return m_events; }
private:
	senp::EffectRuntimeLaunch m_launch;
	senp::EffectRuntimeSnapshot m_state;
	std::deque<senp::InvocationResult> m_results;
	std::vector<senp::effect::Event> m_events;
	std::vector<senp::effect::Effect> m_effects;
	senp::InvocationStatus m_status{ senp::InvocationStatus::EffectsReady };
	std::int64_t m_sequence{};
};

class PublicationPort final {
public:
	void Bind(CSenpOwnerProjection& projection)
	{
		m_publish = [&projection](senp::InvocationResult result) { return projection.Publish(std::move(result)); };
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

class Target final : public ISenpOwnerProjectionTarget {
public:
	bool BeginDocument(std::wstring_view resource,
		const senp::effect::OperationContext& context) noexcept override
	{
		m_resource = resource; m_context = context; ++m_begins;
		if (m_onBegin) m_onBegin();
		return m_accept;
	}
	bool PublishDocument(const senp::effect::OperationContext& context,
		senp::effect::PublishDocument document) noexcept override
	{
		m_context = context; m_document = std::move(document); ++m_publishes; return m_accept;
	}
	bool FailDocument(const senp::effect::OperationContext& context,
		senp::InvocationStatus) noexcept override
	{
		m_context = context; ++m_failures; return m_accept;
	}
	bool CompleteCommand(const senp::effect::OperationContext&,
		senp::effect::CompleteCommand) noexcept override { return m_accept; }
	bool ReleaseResource(std::wstring_view) noexcept override { return m_accept; }
	bool StartToolRead(const senp::effect::OperationContext& context,
		senp::effect::StartToolRead read) noexcept override
	{
		m_toolContext = context; m_readId = std::move(read.readId); ++m_toolReads; return m_accept;
	}
	void CancelToolReads(const senp::effect::OperationContext& context) noexcept override
	{
		if (context.ownerGeneration == m_toolContext.ownerGeneration
			&& context.requestGeneration == m_toolContext.requestGeneration) ++m_toolCancels;
	}
	void Revoke() noexcept override { ++m_revokes; }
	void Reject() noexcept { m_accept = false; }
	void OnBegin(std::function<void()> callback) { m_onBegin = std::move(callback); }
	[[nodiscard]] int Begins() const noexcept { return m_begins; }
	[[nodiscard]] int Publishes() const noexcept { return m_publishes; }
	[[nodiscard]] int Failures() const noexcept { return m_failures; }
	[[nodiscard]] int Revokes() const noexcept { return m_revokes; }
	[[nodiscard]] int ToolReads() const noexcept { return m_toolReads; }
	[[nodiscard]] int ToolCancels() const noexcept { return m_toolCancels; }
	[[nodiscard]] const senp::effect::PublishDocument& Document() const noexcept { return m_document; }
private:
	bool m_accept{ true };
	std::function<void()> m_onBegin;
	int m_begins{}, m_publishes{}, m_failures{}, m_revokes{};
	std::wstring m_resource;
	senp::effect::OperationContext m_context;
	senp::effect::OperationContext m_toolContext;
	senp::effect::PublishDocument m_document;
	std::wstring m_readId;
	int m_toolReads{}, m_toolCancels{};
};

class Fixture final {
public:
	Fixture() : m_owners([this](senp::EffectRuntimeLaunch launch) {
		auto runtime = std::make_unique<Runtime>(std::move(launch));
		m_runtime = runtime.get(); return runtime;
	}) {}
	senp::ContributionOwnerIdentity Activate()
	{
		auto result = m_owners.Prepare({ .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.extension",
			.context = { .workspaceRevision = 7, .accountGeneration = 9 } }, std::wstring(64, L'b'),
			[this](const auto&, const auto*) { return std::make_unique<Publication>(m_port); }, Clock::now());
		EXPECT_EQ(senp::OwnerChangeStatus::Accepted, result.status);
		m_owners.Poll(Clock::now()); (void)m_owners.TakeTransition(); return result.owner;
	}
	[[nodiscard]] senp::CSenpContributionOwners& Owners() noexcept { return m_owners; }
	[[nodiscard]] Runtime& Process() noexcept { return *m_runtime; }
	[[nodiscard]] PublicationPort& Port() noexcept { return *m_port; }
private:
	Runtime* m_runtime{};
	std::shared_ptr<PublicationPort> m_port = std::make_shared<PublicationPort>();
	senp::CSenpContributionOwners m_owners;
};

TEST(SenpOwnerProjection, RoutesTreeCommandOpenAndDocumentAsOneOwnerCohort)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	ASSERT_TRUE(projection.RegisterTree(L"sample.projects", { L"sample.open" }));
	auto tree = projection.Tree(L"sample.projects"); ASSERT_TRUE(tree);
	fixture.Process().Next({ senp::effect::PublishTreePage{ L"sample.projects", L"", {
		{ L"item", L"Item", L"", L"", L"", senp::effect::CollapsibleState::Leaf,
			L"sample.open", { L"sample:details" } } }, L"", 1, senp::effect::PageStatus::Complete, L"" } });
	tree->SetVisible(true, Clock::now()); fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	ASSERT_TRUE(tree->Select(L"item"));
	fixture.Process().Next({ senp::effect::OpenDocument{ L"sample:details" } });
	ASSERT_TRUE(tree->Execute(L"item"));
	fixture.Process().Next({ senp::effect::PublishDocument{ L"sample:details", L"Details", 2,
		{ senp::effect::MarkdownSection{ L"Body" } } } });
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.Begins());
	ASSERT_EQ(3U, fixture.Process().Events().size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::DocumentRequest>(fixture.Process().Events()[2]));
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.Publishes());
	EXPECT_EQ(L"Details", target.Document().title);
	projection.Close(); EXPECT_EQ(1, target.Revokes());
}

TEST(SenpOwnerProjection, WorkspaceSnapshotsReplaceUnsentOnesAndRefuseInvalidPayloads)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	// A root id the wire's charset refuses never reaches the queue. Catching it
	// where it is published keeps a host-side mistake from arriving later as an
	// admission failure, which is indistinguishable from the owner going away.
	EXPECT_FALSE(projection.PublishWorkspace({ { { L"root 0", L"main", {} } } }));
	EXPECT_TRUE(projection.PublishWorkspace({ { { L"root:0", L"main", {} } } }));
	// The event carries the whole workspace, so an unsent snapshot is replaced
	// rather than queued behind: delivering the older list after the newer one
	// would be wrong rather than merely late.
	EXPECT_TRUE(projection.PublishWorkspace({ { { L"root:0", L"feature", {} } } }));
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	ASSERT_EQ(1U, fixture.Process().Events().size());
	const auto* published = std::get_if<senp::effect::WorkspaceChanged>(&fixture.Process().Events()[0]);
	ASSERT_NE(nullptr, published);
	ASSERT_EQ(1U, published->repositories.size());
	EXPECT_EQ(L"feature", published->repositories[0].branch);
	// Nothing is retained once it is submitted, so a turn with no new snapshot
	// republishes nothing.
	EXPECT_EQ(ESenpOwnerProjectionStatus::Idle, projection.Pump(Clock::now()));
	EXPECT_EQ(1U, fixture.Process().Events().size());
	projection.Close(); EXPECT_EQ(1, target.Revokes());
}

TEST(SenpOwnerProjection, DocumentFailureTerminatesAndForeignEffectRevokesCohort)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	ASSERT_TRUE(projection.RegisterTree(L"sample.projects", {}));
	fixture.Process().Next({ senp::effect::OpenDocument{ L"sample:error" } });
	ASSERT_TRUE(projection.Tree(L"sample.projects")->IsVisible() == false);
	// A visibility request supplies a valid owner request context for the open effect.
	projection.Tree(L"sample.projects")->SetVisible(true, Clock::now());
	fixture.Process().Next(senp::InvocationStatus::TimedOut);
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.Begins());
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.Failures());

	fixture.Process().Next({ senp::effect::PublishTreePage{ L"foreign.view", L"", {}, L"", 1,
		senp::effect::PageStatus::Empty, L"" } });
	projection.Tree(L"sample.projects")->Refresh(Clock::now()); fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Rejected, projection.Pump(Clock::now()));
	EXPECT_FALSE(projection.IsCurrent()); EXPECT_EQ(1, target.Revokes());
}

TEST(SenpOwnerProjection, ReentrantCloseFromDocumentBeginDefersRevocation)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	ASSERT_TRUE(projection.RegisterTree(L"sample.projects", {}));
	fixture.Process().Next({ senp::effect::OpenDocument{ L"sample:close" } });
	auto tree = projection.Tree(L"sample.projects"); tree->SetVisible(true, Clock::now());
	target.OnBegin([&] { projection.Close(); EXPECT_EQ(0, target.Revokes()); });
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Closed, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.Begins()); EXPECT_EQ(1, target.Revokes());
	EXPECT_FALSE(projection.IsCurrent());
}

TEST(SenpOwnerProjection, RefreshCancelsRetainedToolReadBeforeStartingItsSuccessor)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	ASSERT_TRUE(projection.RegisterTree(L"sample.projects", {}));
	auto tree = projection.Tree(L"sample.projects");
	fixture.Process().Next({ senp::effect::StartToolRead{ L"read.1", L"github", L"repositoryRead", {} } });
	tree->SetVisible(true, Clock::now()); fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(1, target.ToolReads());

	fixture.Process().Next({ senp::effect::StartToolRead{ L"read.2", L"github", L"repositoryRead", {} } });
	tree->Refresh(Clock::now());
	EXPECT_EQ(1, target.ToolCancels());
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(2, target.ToolReads());
	projection.Close();
}

TEST(SenpOwnerProjection, InvalidationDeliveredDuringDrainReloadsInsteadOfFailing)
{
	Fixture fixture; const auto owner = fixture.Activate(); Target target;
	CSenpOwnerProjection projection(fixture.Owners(), owner, target); fixture.Port().Bind(projection);
	ASSERT_TRUE(projection.RegisterTree(L"sample.projects", {}));
	auto tree = projection.Tree(L"sample.projects");
	const auto page = [](std::wstring label) {
		return senp::effect::PublishTreePage{ L"sample.projects", L"", {
			{ L"item", std::move(label), L"", L"", L"", senp::effect::CollapsibleState::Leaf, L"", {} } },
			L"", 1, senp::effect::PageStatus::Complete, L"" };
	};
	fixture.Process().Next({ page(L"Before") });
	tree->SetVisible(true, Clock::now()); fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	ASSERT_EQ(tree::TreeChildrenState::Complete, tree->Model().Node(L"")->state);

	// An extension answers a workspace change (and its own activation) by
	// invalidating its views. The invalidation reaches the provider inside the
	// coordinator's drain, which refuses every submission; the reload has to wait
	// for the drain to return instead of failing the root until Retry is pressed.
	fixture.Process().Next({ senp::effect::InvalidateTree{ L"sample.projects" } });
	EXPECT_TRUE(projection.PublishWorkspace({ { { L"root:0", L"main", {} } } }));
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	fixture.Process().Next({ page(L"After") });
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(tree::TreeChildrenState::Loading, tree->Model().Node(L"")->state);
	ASSERT_EQ(3U, fixture.Process().Events().size());
	EXPECT_TRUE(std::holds_alternative<senp::effect::WorkspaceChanged>(fixture.Process().Events()[1]));
	EXPECT_TRUE(std::holds_alternative<senp::effect::TreeRequest>(fixture.Process().Events()[2]));
	fixture.Owners().Poll(Clock::now());
	EXPECT_EQ(ESenpOwnerProjectionStatus::Applied, projection.Pump(Clock::now()));
	EXPECT_EQ(tree::TreeChildrenState::Complete, tree->Model().Node(L"")->state);
	projection.Close(); EXPECT_EQ(1, target.Revokes());
}

} // namespace
} // namespace workbench
