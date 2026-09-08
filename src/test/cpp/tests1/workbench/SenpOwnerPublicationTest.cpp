/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/SenpOwnerPublication.h"

#include <deque>

namespace workbench {
namespace {
using Clock = std::chrono::steady_clock;

struct TargetState final { int revoked{}; };
class Target final : public ISenpOwnerProjectionTarget {
public:
	explicit Target(std::shared_ptr<TargetState> state) : m_state(std::move(state)) {}
	bool BeginDocument(std::wstring_view, const senp::effect::OperationContext&) noexcept override { return true; }
	bool PublishDocument(const senp::effect::OperationContext&, senp::effect::PublishDocument) noexcept override { return true; }
	bool FailDocument(const senp::effect::OperationContext&, senp::InvocationStatus) noexcept override { return true; }
	bool CompleteCommand(const senp::effect::OperationContext&, senp::effect::CompleteCommand) noexcept override { return true; }
	bool ReleaseResource(std::wstring_view) noexcept override { return true; }
	void Revoke() noexcept override { ++m_state->revoked; }
private:
	std::shared_ptr<TargetState> m_state;
};

class Body final : public viewcontainer::ISenpViewBody {
public:
	explicit Body(viewcontainer::SenpViewBodyHost host) : m_host(std::move(host))
	{
		m_window = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1,
			m_host.parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	}
	~Body() override { Close(); }
	HWND Window() const noexcept override { return m_window; }
	void Layout(const RECT& bounds, unsigned int) noexcept override
	{
		if (m_window) (void)::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
	}
	void SetVisible(bool visible) noexcept override { if (m_window) ::ShowWindow(m_window, visible ? SW_SHOWNA : SW_HIDE); }
	void SetPalette(const theme::ThemePalette&, layout::EViewContainerLocation) noexcept override {}
	bool Focus() noexcept override
	{
		if (!m_window) return false;
		::SetFocus(m_window);
		return ::GetFocus() == m_window;
	}
	bool PreTranslate(MSG&) noexcept override { return false; }
	void Close() noexcept override
	{
		m_host = {};
		if (m_window) { ::DestroyWindow(m_window); m_window = nullptr; }
	}
private:
	viewcontainer::SenpViewBodyHost m_host;
	HWND m_window{};
};

class Runtime final : public senp::ISenpEffectRuntime {
public:
	Runtime(senp::EffectRuntimeLaunch launch, std::vector<senp::effect::Effect> activation)
		: m_launch(std::move(launch)), m_activation(std::move(activation)) {}
	senp::InvocationAdmission Start() override
	{
		m_state.phase = senp::RuntimePhase::Active;
		auto context = m_launch.context;
		context.operationId = L"activation";
		m_results.push_back({ context, true, senp::InvocationStatus::EffectsReady, {}, std::move(m_activation) });
		return { senp::AdmissionStatus::Accepted, context.operationId };
	}
	senp::InvocationAdmission Submit(senp::effect::OperationContext, senp::effect::Event,
		senp::CSenpRuntimeSession::Time) override { return { senp::AdmissionStatus::Busy }; }
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
private:
	senp::EffectRuntimeLaunch m_launch;
	std::vector<senp::effect::Effect> m_activation;
	std::deque<senp::InvocationResult> m_results;
	senp::EffectRuntimeSnapshot m_state;
};

struct DeclaredState final {
	std::optional<senp::ContributionOwnerIdentity> owner;
	std::vector<SenpOwnerBoundTree> bindings;
	bool permitCommit{ true };
	int pumped{};
	int cleared{};
};

class DeclaredPublication final : public ISenpDeclaredTreePublication {
public:
	DeclaredPublication(std::shared_ptr<DeclaredState> state,
		senp::CSenpContributionOwners& owners, senp::ContributionOwnerIdentity owner,
		std::vector<SenpOwnerBoundTree> bindings)
		: m_state(std::move(state)), m_owners(owners), m_owner(std::move(owner)),
		m_previous(m_state->owner), m_bindings(std::move(bindings)) {}
	bool CanCommit() const noexcept override
	{
		return !m_closed && !m_committed && m_state->permitCommit && m_state->owner == m_previous;
	}
	bool Commit() noexcept override
	{
		if (!CanCommit()) return false;
		m_state->owner = std::move(m_owner);
		m_state->bindings = std::move(m_bindings);
		m_generation = m_state->owner->generation;
		m_committed = true;
		return true;
	}
	bool Pump() noexcept override
	{
		if (!m_committed || m_closed || !m_state->owner
			|| m_state->owner->generation != m_generation) return false;
		EXPECT_TRUE(m_owners.IsCurrent(*m_state->owner));
		++m_state->pumped;
		return true;
	}
	void Close() noexcept override
	{
		if (m_closed) return;
		m_closed = true;
		if (!m_committed || !m_state->owner || m_state->owner->generation != m_generation) return;
		m_state->owner.reset();
		m_state->bindings.clear();
		++m_state->cleared;
	}
private:
	std::shared_ptr<DeclaredState> m_state;
	senp::CSenpContributionOwners& m_owners;
	senp::ContributionOwnerIdentity m_owner;
	std::optional<senp::ContributionOwnerIdentity> m_previous;
	std::vector<SenpOwnerBoundTree> m_bindings;
	std::int64_t m_generation{};
	bool m_committed{};
	bool m_closed{};
};

class SenpOwnerPublicationTest : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }

	void SetUp() override
	{
		m_owner = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 480,
			nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, m_owner);
	}
	void TearDown() override { if (m_owner) ::DestroyWindow(m_owner); }

	SenpOwnerPublicationOptions Options(std::shared_ptr<TargetState> target) const
	{
		layout::WorkbenchViewContainerDescriptor container{
			"sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
			{ layout::EViewContainerLocation::Sidebar },
		};
		layout::WorkbenchViewDescriptor view{
			"sample.projects", container.id, "Projects", 10, true, true, "senp.tree",
		};
		return {
			m_owner, { std::move(container) }, { { std::move(view), { "sample.open" } } },
			std::make_unique<Target>(std::move(target)), [](std::string_view) { return true; },
			[](viewcontainer::SenpViewBodyHost host, std::shared_ptr<tree::SenpTreeProvider>, std::wstring) {
				auto body = std::make_unique<Body>(std::move(host));
				return body->Window() ? std::unique_ptr<viewcontainer::ISenpViewBody>(std::move(body)) : nullptr;
			},
		};
	}

	static senp::EffectRuntimeLaunch Launch()
	{
		return { .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = L"sample.extension",
			.context = { .workspaceRevision = 7, .accountGeneration = 9 } };
	}

	HWND m_owner{};
};

TEST_F(SenpOwnerPublicationTest, AtomicallyPublishesCatalogPagesAndQueuedActivation)
{
	std::vector<senp::effect::Effect> activation{ senp::effect::InvalidateTree{ L"sample.projects" } };
	senp::CSenpContributionOwners owners([&](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::move(activation));
	});
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	EXPECT_FALSE(pages.Contains("sample.senp"));

	owners.Poll(Clock::now());
	ASSERT_TRUE(hub.Pump(Clock::now()));
	ASSERT_TRUE(pages.Contains("sample.senp"));
	const std::vector<std::string> invalidRemoval{ "sample.senp", "workbench.view.explorer" };
	EXPECT_FALSE(pages.RemoveContributedPages(invalidRemoval));
	EXPECT_TRUE(pages.Contains("sample.senp"));
	const auto snapshot = catalog.Snapshot();
	ASSERT_EQ(1U, snapshot.owners.size());
	EXPECT_EQ("sample.extension", snapshot.owners[0].ownerId);
	EXPECT_EQ(static_cast<std::uint64_t>(change.owner.generation), snapshot.owners[0].generation);
	const auto view = std::ranges::find_if(snapshot.views,
		[](const auto& item) { return item.descriptor.id == "sample.projects"; });
	ASSERT_NE(snapshot.views.end(), view);
	EXPECT_EQ("senp.tree", view->descriptor.provider);
	auto terminal = owners.TakeTransition();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, terminal->status);

	ASSERT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Updated));
	owners.Poll(Clock::now());
	EXPECT_EQ(1, target->revoked);
	EXPECT_TRUE(catalog.Snapshot().owners.empty());
	EXPECT_FALSE(pages.Contains("sample.senp"));
	auto reenabled = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, reenabled.status);
	owners.Poll(Clock::now());
	auto activatedAgain = owners.TakeTransition();
	ASSERT_TRUE(activatedAgain);
	EXPECT_EQ(senp::OwnerChangeStatus::Activated, activatedAgain->status);
	EXPECT_TRUE(pages.Contains("sample.senp"));
	EXPECT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Shutdown));
	EXPECT_FALSE(pages.Contains("sample.senp"));
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, RevisionConflictPublishesNeitherCandidate)
{
	std::vector<senp::effect::Effect> activation;
	senp::CSenpContributionOwners owners([&](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::move(activation));
	});
	layout::WorkbenchContributionRegistry catalog;
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(m_owner));
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto target = std::make_shared<TargetState>();
	auto change = owners.Prepare(Launch(), std::wstring(64, L'b'),
		[&](const auto& candidate, const auto* previous) {
			return hub.Prepare(candidate, previous, Options(target));
		}, Clock::now());
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, change.status);
	auto other = catalog.PrepareOwnerReplacement({ "other.extension", 1 }, 0, {}, {});
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Prepared, other.status);
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(other.change)));

	owners.Poll(Clock::now());
	EXPECT_FALSE(pages.Contains("sample.senp"));
	EXPECT_TRUE(std::ranges::none_of(catalog.Snapshot().owners,
		[](const auto& owner) { return owner.ownerId == "sample.extension"; }));
	auto terminal = owners.TakeTransition();
	ASSERT_TRUE(terminal);
	EXPECT_EQ(senp::OwnerChangeStatus::Failed, terminal->status);
	EXPECT_EQ(1, target->revoked);
	pages.Close();
}

TEST_F(SenpOwnerPublicationTest, DeclaredBindingsRetainCatalogAcrossReplacementFailureAndRevocation)
{
	senp::CSenpContributionOwners owners([](senp::EffectRuntimeLaunch launch) {
		return std::make_unique<Runtime>(std::move(launch), std::vector<senp::effect::Effect>{});
	});
	layout::WorkbenchContributionRegistry catalog;
	const layout::WorkbenchContributionOwner declarationOwner{ "sample.extension", 1 };
	const layout::WorkbenchViewContainerDescriptor container{
		"sample.senp", "Sample", layout::EViewContainerLocation::Sidebar, 10, "", false,
		{ layout::EViewContainerLocation::Sidebar } };
	const layout::WorkbenchViewDescriptor view{
		"sample.projects", container.id, "Projects", 10, true, true, "senp.tree" };
	auto declaration = catalog.PrepareOwnerReplacement(declarationOwner, 0,
		std::span(&container, 1), std::span(&view, 1));
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Prepared, declaration.status);
	ASSERT_EQ(layout::EWorkbenchContributionChangeStatus::Committed, catalog.Commit(std::move(declaration.change)));
	CDlgFuncList dialog;
	viewcontainer::CViewContainerPages pages(dialog);
	CSenpOwnerPublicationHub hub(owners, catalog, pages);
	auto state = std::make_shared<DeclaredState>();
	auto target = std::make_shared<TargetState>();
	auto prepare = [&](wchar_t digest) {
		return owners.Prepare(Launch(), std::wstring(64, digest),
			[&](const auto& candidate, const auto* previous) {
				return hub.Prepare(candidate, previous, SenpOwnerPublicationOptions{
					{ { view, { "sample.open" } } }, std::make_unique<Target>(target),
					[&](const auto& owner, auto bindings) {
						return std::make_unique<DeclaredPublication>(state, owners, owner, std::move(bindings));
					} });
			}, Clock::now());
	};
	auto initial = prepare(L'b');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, initial.status);
	EXPECT_FALSE(state->owner);
	owners.Poll(Clock::now());
	ASSERT_TRUE(owners.IsCurrent(initial.owner));
	EXPECT_EQ(0, state->pumped);
	ASSERT_TRUE(hub.Pump(Clock::now()));
	EXPECT_EQ(1, state->pumped);
	ASSERT_EQ(1U, state->bindings.size());
	EXPECT_EQ(L"sample.projects", state->bindings.front().ViewId());
	auto originalProvider = state->bindings.front().Provider();
	ASSERT_TRUE(owners.TakeTransition());

	auto rejected = prepare(L'c');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, rejected.status);
	state->permitCommit = false;
	owners.Poll(Clock::now());
	auto failed = owners.TakeTransition();
	ASSERT_TRUE(failed);
	EXPECT_EQ(senp::OwnerChangeStatus::Failed, failed->status);
	EXPECT_TRUE(owners.IsCurrent(initial.owner));
	EXPECT_EQ(originalProvider, state->bindings.front().Provider());
	EXPECT_EQ(0, state->cleared);
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));

	state->permitCommit = true;
	auto replacement = prepare(L'd');
	ASSERT_EQ(senp::OwnerChangeStatus::Accepted, replacement.status);
	owners.Poll(Clock::now());
	ASSERT_TRUE(owners.IsCurrent(replacement.owner));
	ASSERT_TRUE(state->owner);
	EXPECT_EQ(replacement.owner, *state->owner);
	EXPECT_NE(originalProvider, state->bindings.front().Provider());
	EXPECT_EQ(0, state->cleared);
	EXPECT_EQ(2, target->revoked);
	ASSERT_TRUE(hub.Pump(Clock::now()));
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));
	ASSERT_TRUE(owners.Revoke(L"sample.extension", senp::effect::StopReason::Disabled));
	EXPECT_FALSE(state->owner);
	EXPECT_TRUE(state->bindings.empty());
	EXPECT_EQ(1, state->cleared);
	EXPECT_EQ(3, target->revoked);
	EXPECT_TRUE(catalog.IsOwnerCurrent(declarationOwner));
	EXPECT_EQ(1, std::ranges::count_if(catalog.Snapshot().views,
		[](const auto& item) { return item.descriptor.id == "sample.projects"; }));
	EXPECT_TRUE(owners.Close());
	hub.Close();
	EXPECT_EQ(1, state->cleared);
}

} // namespace
} // namespace workbench
