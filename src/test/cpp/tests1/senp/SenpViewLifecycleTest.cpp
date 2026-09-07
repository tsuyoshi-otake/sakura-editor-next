/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchIds.h"
#include "senp/SenpContributionOwners.h"
#include <array>
#include <deque>
#include <stdexcept>

namespace {
using namespace workbench::layout;
using Status = EWorkbenchContributionChangeStatus;

WorkbenchViewContainerDescriptor Container(std::string id = "github.issues")
{
	return { .id = std::move(id), .title = "GitHub", .location = EViewContainerLocation::Sidebar,
		.icon = "github", .supportedLocations = { EViewContainerLocation::Sidebar, EViewContainerLocation::AuxiliaryBar } };
}
WorkbenchViewDescriptor View(std::string id = "github.issues.open", std::string container = "github.issues")
{
	return { .id = std::move(id), .containerId = std::move(container), .title = "Open Issues", .provider = "senp.tree" };
}
PrepareWorkbenchContributionsResult Prepare(WorkbenchContributionRegistry& registry,
	std::uint64_t generation, std::uint64_t previous = 0)
{
	return registry.PrepareOwnerReplacement({ "test.github", generation }, previous,
		std::array{ Container() }, std::array{ View() });
}
void ExpectUnchanged(const WorkbenchContributionSnapshot& expected, const WorkbenchContributionSnapshot& actual)
{
	EXPECT_EQ(expected.revision, actual.revision);
	EXPECT_EQ(expected.owners, actual.owners);
	ASSERT_EQ(expected.viewContainers.size(), actual.viewContainers.size());
	ASSERT_EQ(expected.views.size(), actual.views.size());
	for (std::size_t i = 0; i < expected.viewContainers.size(); ++i) {
		EXPECT_EQ(expected.viewContainers[i].descriptor, actual.viewContainers[i].descriptor);
		EXPECT_EQ(expected.viewContainers[i].owner, actual.viewContainers[i].owner);
	}
	for (std::size_t i = 0; i < expected.views.size(); ++i) {
		EXPECT_EQ(expected.views[i].descriptor, actual.views[i].descriptor);
		EXPECT_EQ(expected.views[i].owner, actual.views[i].owner);
	}
}

TEST(SenpViewLifecycle, PreparedCatalogIsInvisibleAndAbandonedUpdatePreservesOwner)
{
	WorkbenchContributionRegistry registry;
	const auto before = registry.Snapshot();
	auto prepared = Prepare(registry, 1);
	ASSERT_EQ(Status::Prepared, prepared.status);
	ExpectUnchanged(before, registry.Snapshot());
	EXPECT_EQ(before.views.size() + 1, prepared.change->Snapshot().views.size());
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(prepared.change)));
	const auto active = registry.Snapshot();
	{ auto abandoned = Prepare(registry, 2, 1); ASSERT_EQ(Status::Prepared, abandoned.status); }
	ExpectUnchanged(active, registry.Snapshot());
	EXPECT_TRUE(registry.IsOwnerCurrent({ "test.github", 1 }));
	EXPECT_FALSE(registry.IsOwnerCurrent({ "test.github", 2 }));
}

TEST(SenpViewLifecycle, InvalidOrCollidingBatchNeverPublishesAPartialOwner)
{
	WorkbenchContributionRegistry registry;
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(Prepare(registry, 1).change)));
	const auto before = registry.Snapshot();
	const auto check = [&](auto containers, auto views) {
		auto result = registry.PrepareOwnerReplacement({ "test.github", 2 }, 1, containers, views);
		EXPECT_EQ(Status::Invalid, result.status);
		EXPECT_FALSE(result.change);
		ExpectUnchanged(before, registry.Snapshot());
	};
	check(std::array{ Container(), Container() }, std::array{ View() });
	check(std::array{ Container() }, std::array{ View(), View() });
	check(std::array{ Container() }, std::array{ View("missing", "missing.container") });
	check(std::array{ Container(std::string(ids::viewContainer::SourceControl)) }, std::array{ View() });
	check(std::array{ Container() }, std::array{ View(std::string(ids::view::SourceControl)) });
}

TEST(SenpViewLifecycle, CommitFencesGenerationRevisionAndRegistryIdentity)
{
	WorkbenchContributionRegistry registry;
	WorkbenchContributionRegistry unrelated;
	auto foreign = Prepare(registry, 1);
	EXPECT_EQ(Status::Invalid, unrelated.Commit(std::move(foreign.change)));
	auto first = Prepare(registry, 1);
	auto stale = Prepare(registry, 2);
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(first.change)));
	EXPECT_EQ(Status::Conflict, registry.Commit(std::move(stale.change)));
	EXPECT_EQ(Status::Conflict, Prepare(registry, 2).status);
	EXPECT_EQ(Status::Conflict, Prepare(registry, 1, 1).status);
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(Prepare(registry, 2, 1).change)));
	EXPECT_FALSE(registry.IsOwnerCurrent({ "test.github", 1 }));
	EXPECT_TRUE(registry.IsOwnerCurrent({ "test.github", 2 }));
	EXPECT_EQ(Status::Conflict, registry.PrepareOwnerDisposal({ "test.github", 1 }).status);
}

TEST(SenpViewLifecycle, DisposalKeepsBuiltinsOtherOwnersAndRejectsResurrection)
{
	WorkbenchContributionRegistry registry;
	const auto builtins = registry.Snapshot();
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(Prepare(registry, 1).change)));
	auto second = registry.PrepareOwnerReplacement({ "test.actions", 2 }, 0,
		std::array{ Container("github.actions") }, std::array{ View("github.runs", "github.actions") });
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(second.change)));
	auto disposal = registry.PrepareOwnerDisposal({ "test.github", 1 });
	ASSERT_EQ(Status::Prepared, disposal.status);
	EXPECT_TRUE(registry.IsOwnerCurrent({ "test.github", 1 }));
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(disposal.change)));
	EXPECT_FALSE(registry.IsOwnerCurrent({ "test.github", 1 }));
	EXPECT_TRUE(registry.IsOwnerCurrent({ "test.actions", 2 }));
	EXPECT_EQ(builtins.parts.size(), registry.Snapshot().parts.size());
	EXPECT_EQ(builtins.views.size() + 1, registry.Snapshot().views.size());
	EXPECT_EQ(Status::Conflict, Prepare(registry, 1).status);
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(Prepare(registry, 3).change)));
	EXPECT_TRUE(registry.IsOwnerCurrent({ "test.github", 3 }));
}

TEST(SenpViewLifecycle, ExtensionsMayUseBuiltinContainersButNeverBorrowAnotherOwner)
{
	WorkbenchContributionRegistry registry;
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(Prepare(registry, 1).change)));
	auto borrowed = registry.PrepareOwnerReplacement({ "test.actions", 2 }, 0, {}, std::array{ View("github.runs") });
	EXPECT_EQ(Status::Unsupported, borrowed.status);
	auto supported = registry.PrepareOwnerReplacement({ "test.actions", 2 }, 0, {},
		std::array{ View("github.runs", std::string(ids::viewContainer::Explorer)) });
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(supported.change)));
	auto snapshot = registry.Snapshot();
	snapshot.views.back().owner = { "missing.owner", 99 };
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(snapshot));
}

TEST(SenpViewLifecycle, BoundsFailBeforePublicationAndDisposalReleasesActiveCapacity)
{
	WorkbenchContributionRegistry registry;
	std::vector<WorkbenchViewDescriptor> excessive(65, View());
	EXPECT_EQ(Status::Invalid, registry.PrepareOwnerReplacement({ "large", 1 }, 0, {}, excessive).status);
	EXPECT_EQ(Status::Invalid, Prepare(registry, 0).status);
	EXPECT_EQ(Status::Invalid, Prepare(registry, UINT64_MAX).status);
	for (std::uint64_t i = 1; i <= 64; ++i) {
		auto ready = registry.PrepareOwnerReplacement({ "owner." + std::to_string(i), i }, 0, {}, {});
		ASSERT_EQ(Status::Prepared, ready.status);
		ASSERT_EQ(Status::Committed, registry.Commit(std::move(ready.change)));
	}
	EXPECT_EQ(Status::Exhausted, registry.PrepareOwnerReplacement({ "extra", 65 }, 0, {}, {}).status);
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(registry.PrepareOwnerDisposal({ "owner.1", 1 }).change)));
	ASSERT_EQ(Status::Committed, registry.Commit(std::move(registry.PrepareOwnerReplacement({ "extra", 65 }, 0, {}, {}).change)));
}

using namespace senp;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct RuntimeProbe {
	EffectRuntimeLaunch launch;
	EffectRuntimeSnapshot state{};
	std::deque<InvocationResult> results;
	int starts{}, stops{}, joins{};
	bool startThrows{}, joinThrows{}, autoActivate{ true }, exitOnStop{ true }, physicalExit{ true };
	std::function<void()> onStop, onJoin;
};

class ManualRuntime final : public ISenpEffectRuntime {
public:
	explicit ManualRuntime(std::shared_ptr<RuntimeProbe> value) : p(std::move(value)) {}
	InvocationAdmission Start() override
	{
		++p->starts;
		if (p->startThrows) throw std::runtime_error("launch failure");
		p->state.phase = p->autoActivate ? RuntimePhase::Active : RuntimePhase::Activating;
		auto context = p->launch.context;
		context.operationId = L"activation";
		if (p->autoActivate) p->results.push_back({ context, true, InvocationStatus::EffectsReady });
		return { AdmissionStatus::Accepted, context.operationId };
	}
	InvocationAdmission Submit(effect::OperationContext context, effect::Event, CSenpRuntimeSession::Time) override
	{
		context.operationId = L"request";
		p->results.push_back({ context, false, InvocationStatus::EffectsReady });
		return { AdmissionStatus::Accepted, context.operationId };
	}
	bool Cancel(std::wstring_view) override { return true; }
	void Stop(effect::StopReason) override
	{
		++p->stops;
		if (p->onStop) p->onStop();
		p->state.phase = p->exitOnStop ? RuntimePhase::Stopped : RuntimePhase::Stopping;
		p->state.workerExited = p->exitOnStop;
		p->state.processExitConfirmed = p->exitOnStop && p->physicalExit;
	}
	void Join() override
	{
		++p->joins;
		if (p->joinThrows) throw std::runtime_error("join failure");
		if (p->onJoin) p->onJoin();
		p->state.phase = RuntimePhase::Stopped;
		p->state.workerExited = true;
		p->state.processExitConfirmed = p->physicalExit;
	}
	std::optional<InvocationResult> TakeCompleted() override
	{
		if (p->results.empty()) return {};
		auto result = std::move(p->results.front());
		p->results.pop_front();
		return result;
	}
	EffectRuntimeSnapshot Snapshot() const override { return p->state; }
private:
	std::shared_ptr<RuntimeProbe> p;
};

struct PublicationProbe {
	ContributionOwnerIdentity identity;
	bool valid{ true }, commitAllowed{ true }, applyAllowed{ true }, visible{};
	int commits{}, applies{}, revokes{}, destroyed{};
};

class CatalogPublication final : public ISenpOwnerPublication {
public:
	CatalogPublication(WorkbenchContributionRegistry& registry, std::shared_ptr<PublicationProbe> probe,
		std::unique_ptr<PreparedWorkbenchContributions> change)
		: r(registry), p(std::move(probe)), prepared(std::move(change)) {}
	~CatalogPublication() override { ++p->destroyed; }
	bool Validate(const InvocationResult&) const noexcept override { return p->valid; }
	bool Commit() noexcept override
	{
		++p->commits;
		return p->commitAllowed && r.Commit(std::move(prepared)) == Status::Committed;
	}
	bool Apply(InvocationResult) noexcept override { ++p->applies; p->visible = true; return p->applyAllowed; }
	void Revoke(effect::StopReason) noexcept override
	{
		++p->revokes;
		p->visible = false;
		const auto owner = Owner();
		if (r.IsOwnerCurrent(owner)) EXPECT_EQ(Status::Committed, r.DisposeOwner(owner));
	}
	WorkbenchContributionOwner Owner() const
	{
		std::string id;
		for (auto c : p->identity.extensionId) id.push_back(static_cast<char>(c));
		return { id, static_cast<std::uint64_t>(p->identity.generation) };
	}
private:
	WorkbenchContributionRegistry& r;
	std::shared_ptr<PublicationProbe> p;
	std::unique_ptr<PreparedWorkbenchContributions> prepared;
};

struct OwnerHarness {
	WorkbenchContributionRegistry registry;
	std::vector<std::shared_ptr<RuntimeProbe>> runtimes;
	std::vector<std::shared_ptr<PublicationProbe>> publications;
	bool activate{ true }, throwStart{}, exitOnStop{ true }, valid{ true }, commit{ true };
	CSenpContributionOwners owners{ [this](EffectRuntimeLaunch launch) {
		auto probe = std::make_shared<RuntimeProbe>();
		probe->launch = std::move(launch);
		probe->autoActivate = activate;
		probe->startThrows = throwStart;
		probe->exitOnStop = exitOnStop;
		runtimes.push_back(probe);
		return std::make_unique<ManualRuntime>(probe);
	} };
	OwnerChangeResult Begin(std::wstring id = L"test.github")
	{
		return owners.Prepare({ .hostExecutable = L"host.exe", .modulePath = L"module.wasm",
			.moduleSha256 = std::wstring(64, L'a'), .extensionId = std::move(id),
			.context = { .workspaceRevision = 3, .accountGeneration = 4 } }, std::wstring(64, L'b'),
			[this](const ContributionOwnerIdentity& identity, const ContributionOwnerIdentity* previous) -> std::unique_ptr<ISenpOwnerPublication> {
				auto probe = std::make_shared<PublicationProbe>();
				probe->identity = identity;
				probe->valid = valid;
				probe->commitAllowed = commit;
				std::string id;
				for (auto c : identity.extensionId) id.push_back(static_cast<char>(c));
				auto prepared = registry.PrepareOwnerReplacement({ id, static_cast<std::uint64_t>(identity.generation) },
					previous ? previous->generation : 0, std::array{ Container(id + ".container") },
					std::array{ View(id + ".view", id + ".container") });
				if (prepared.status != Status::Prepared) return {};
				publications.push_back(probe);
				return std::make_unique<CatalogPublication>(registry, probe, std::move(prepared.change));
			}, Clock::now());
	}
	ContributionOwnerIdentity Activate(std::wstring id = L"test.github")
	{
		auto result = Begin(std::move(id));
		EXPECT_EQ(OwnerChangeStatus::Accepted, result.status);
		owners.Poll(Clock::now());
		auto terminal = owners.TakeTransition();
		EXPECT_TRUE(terminal);
		if (terminal) EXPECT_EQ(OwnerChangeStatus::Activated, terminal->status);
		return result.owner;
	}
};

TEST(SenpViewLifecycle, ActivationAndFailedUpdateKeepTheExistingPublicationAtomic)
{
	OwnerHarness h;
	const auto old = h.Activate();
	const auto before = h.registry.Snapshot();
	h.commit = false;
	const auto update = h.Begin();
	ASSERT_EQ(OwnerChangeStatus::Accepted, update.status);
	EXPECT_TRUE(h.owners.IsCurrent(old));
	EXPECT_FALSE(h.owners.IsCurrent(update.owner));
	h.owners.Poll(Clock::now());
	ExpectUnchanged(before, h.registry.Snapshot());
	EXPECT_TRUE(h.owners.IsCurrent(old));
	ASSERT_TRUE(h.owners.TakeTransition());
	EXPECT_FALSE(h.owners.TakeTransition());
	EXPECT_EQ(0, h.runtimes[0]->stops);
	EXPECT_EQ(1, h.runtimes[1]->stops);
	EXPECT_EQ(1, h.runtimes[1]->joins);
}

TEST(SenpViewLifecycle, ReplacementFencesTakenResultsAndKeepsRetirementOwnership)
{
	OwnerHarness h;
	const auto old = h.Activate();
	h.runtimes[0]->exitOnStop = false;
	const auto replacement = h.Begin();
	h.runtimes[0]->onStop = [&] {
		EXPECT_FALSE(h.owners.IsCurrent(old));
		EXPECT_FALSE(h.publications[0]->visible);
	};
	h.owners.Poll(Clock::now());
	EXPECT_FALSE(h.owners.IsCurrent(old));
	EXPECT_TRUE(h.owners.IsCurrent(replacement.owner));
	EXPECT_EQ(1U, h.owners.Snapshot().retiring);
	EXPECT_EQ(0, h.runtimes[0]->joins);
	EXPECT_EQ(AdmissionStatus::Unavailable, h.owners.Submit(old, effect::TreeRequest{ L"old" }, 1, Clock::now() + 1s).status);
	h.runtimes[0]->results.push_back({ { L"late", old.generation, 3, 4, 1 }, false, InvocationStatus::EffectsReady });
	h.runtimes[0]->state = { .phase = RuntimePhase::Stopped, .workerExited = true, .processExitConfirmed = true };
	h.owners.Poll(Clock::now());
	EXPECT_EQ(1, h.publications[0]->applies);
	EXPECT_EQ(1, h.runtimes[0]->joins);
	EXPECT_EQ(0U, h.owners.Snapshot().retiring);
	EXPECT_TRUE(h.registry.IsOwnerCurrent({ "test.github", static_cast<std::uint64_t>(replacement.owner.generation) }));
}

TEST(SenpViewLifecycle, DisableCancelsPendingUpdateAndRejectsUndeliveredEffects)
{
	OwnerHarness h;
	const auto old = h.Activate();
	ASSERT_EQ(AdmissionStatus::Accepted, h.owners.Submit(old, effect::TreeRequest{ L"tree" }, 1, Clock::now() + 1s).status);
	h.activate = false;
	const auto pending = h.Begin();
	ASSERT_EQ(OwnerChangeStatus::Accepted, pending.status);
	ASSERT_TRUE(h.owners.Revoke(L"test.github", effect::StopReason::Disabled));
	EXPECT_FALSE(h.owners.IsCurrent(old));
	EXPECT_FALSE(h.owners.IsCurrent(pending.owner));
	EXPECT_FALSE(h.publications[0]->visible);
	h.owners.Poll(Clock::now());
	EXPECT_EQ(1, h.publications[0]->applies);
	EXPECT_EQ(0, h.publications[1]->applies);
	ASSERT_TRUE(h.owners.TakeTransition());
	EXPECT_FALSE(h.owners.TakeTransition());
	EXPECT_TRUE(h.owners.Close());
	for (const auto& runtime : h.runtimes) { EXPECT_EQ(1, runtime->stops); EXPECT_EQ(1, runtime->joins); }
}

TEST(SenpViewLifecycle, FailedStartAndPreparationDeadlineHaveOneTerminalReceipt)
{
	OwnerHarness h;
	h.throwStart = true;
	ASSERT_EQ(OwnerChangeStatus::Accepted, h.Begin().status);
	h.owners.Poll(Clock::now());
	auto failed = h.owners.TakeTransition();
	ASSERT_TRUE(failed);
	EXPECT_EQ(OwnerChangeStatus::Failed, failed->status);
	EXPECT_FALSE(h.owners.TakeTransition());
	h.throwStart = false;
	h.activate = false;
	ASSERT_EQ(OwnerChangeStatus::Accepted, h.Begin().status);
	h.owners.Poll(Clock::now() + 11s);
	auto expired = h.owners.TakeTransition();
	ASSERT_TRUE(expired);
	EXPECT_EQ(OwnerChangeStatus::TimedOut, expired->status);
	EXPECT_EQ(0U, h.owners.Snapshot().active);
	EXPECT_EQ(0U, h.owners.Snapshot().retiring);
}

TEST(SenpViewLifecycle, CrashAndWrongContextRevokeInsteadOfApplyingOrRestarting)
{
	for (int fault = 0; fault < 4; ++fault) {
		OwnerHarness h;
		const auto owner = h.Activate();
		ASSERT_EQ(AdmissionStatus::Accepted, h.owners.Submit(owner, effect::TreeRequest{ L"tree" }, 1, Clock::now() + 1s).status);
		auto& result = h.runtimes[0]->results.back();
		if (fault == 0) h.runtimes[0]->state.phase = RuntimePhase::Stopped;
		if (fault == 1) ++result.context.ownerGeneration;
		if (fault == 2) ++result.context.workspaceRevision;
		if (fault == 3) ++result.context.accountGeneration;
		h.owners.Poll(Clock::now());
		EXPECT_FALSE(h.owners.IsCurrent(owner));
		EXPECT_FALSE(h.publications[0]->visible);
		EXPECT_EQ(1, h.publications[0]->applies);
		h.owners.Poll(Clock::now() + 1h);
		EXPECT_EQ(1U, h.runtimes.size());
	}
}

TEST(SenpViewLifecycle, HostBudgetIncludesPreparingAndRetiringProcesses)
{
	OwnerHarness h;
	h.activate = false;
	h.exitOnStop = false;
	for (int i = 0; i < 4; ++i) ASSERT_EQ(OwnerChangeStatus::Accepted, h.Begin(L"test." + std::to_wstring(i)).status);
	EXPECT_EQ(OwnerChangeStatus::Busy, h.Begin(L"extra").status);
	ASSERT_TRUE(h.owners.Revoke(L"test.0", effect::StopReason::Disabled));
	EXPECT_EQ(OwnerChangeStatus::Busy, h.Begin(L"extra").status);
	EXPECT_EQ(4U, h.runtimes.size());
	h.runtimes[0]->state = { .phase = RuntimePhase::Stopped, .workerExited = true, .processExitConfirmed = true };
	h.owners.Poll(Clock::now());
	ASSERT_EQ(OwnerChangeStatus::Accepted, h.Begin(L"extra").status);
	EXPECT_TRUE(h.owners.Close());
	for (const auto& runtime : h.runtimes) EXPECT_EQ(1, runtime->joins);
}

TEST(SenpViewLifecycle, UnconsumedTransitionsApplyBackpressureBeforeAdmission)
{
	OwnerHarness h;
	h.commit = false;
	for (int i = 0; i < 16; ++i) {
		ASSERT_EQ(OwnerChangeStatus::Accepted, h.Begin().status);
		h.owners.Poll(Clock::now());
	}
	EXPECT_EQ(0U, h.owners.Snapshot().active);
	EXPECT_EQ(16U, h.owners.Snapshot().transitions);
	EXPECT_EQ(OwnerChangeStatus::Busy, h.Begin().status);
	EXPECT_EQ(16U, h.runtimes.size());
	ASSERT_TRUE(h.owners.TakeTransition());
	EXPECT_EQ(OwnerChangeStatus::Accepted, h.Begin().status);
}

TEST(SenpViewLifecycle, ShutdownRevokesEveryOwnerBeforeJoiningAndRetainsFailedCleanup)
{
	OwnerHarness h;
	const auto one = h.Activate(L"one");
	const auto two = h.Activate(L"two");
	h.runtimes[0]->onJoin = [&] {
		EXPECT_FALSE(h.owners.IsCurrent(one));
		EXPECT_FALSE(h.owners.IsCurrent(two));
		EXPECT_EQ(1, h.runtimes[1]->stops);
	};
	h.runtimes[0]->physicalExit = false;
	EXPECT_FALSE(h.owners.Close());
	EXPECT_EQ(1U, h.owners.Snapshot().cleanupFailed);
	EXPECT_EQ(OwnerChangeStatus::Stopped, h.Begin().status);
	h.runtimes[0]->physicalExit = true;
	h.runtimes[0]->state.processExitConfirmed = true;
	EXPECT_TRUE(h.owners.Close());
	EXPECT_EQ(1, h.runtimes[0]->joins);
	EXPECT_EQ(1, h.runtimes[1]->joins);
}

TEST(SenpViewLifecycle, FailedJoinIsRetainedWithoutAnAutomaticRetryLoop)
{
	OwnerHarness h;
	const auto owner = h.Activate();
	h.runtimes[0]->joinThrows = true;
	ASSERT_TRUE(h.owners.Revoke(owner.extensionId, effect::StopReason::Disabled));
	h.owners.Poll(Clock::now());
	h.owners.Poll(Clock::now() + 5s);
	EXPECT_EQ(1, h.runtimes[0]->joins);
	EXPECT_EQ(1U, h.owners.Snapshot().cleanupFailed);
	h.runtimes[0]->joinThrows = false;
	EXPECT_TRUE(h.owners.Close());
	EXPECT_EQ(2, h.runtimes[0]->joins);
}

} // namespace
