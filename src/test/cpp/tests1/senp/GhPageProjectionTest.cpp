/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhPageProjection.h"

#include "senp/github/GhRepositorySelection.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include <sakura/serialization/JsoncDocument.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <tuple>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace senp::github;

//! Every shape a request can be built for. The list is the tool boundary's own
//! closed vocabulary, written out so a shape added there without a stated field
//! set fails here instead of at a live page.
constexpr std::array<std::wstring_view, 14> kShapes{
	L"issues", L"issue", L"issueComments", L"issueComment", L"pulls", L"pull",
	L"workflows", L"runs", L"workflowRuns", L"run", L"runAttempt", L"runJobs",
	L"runAttemptJobs", L"job",
};

//! A shape name as a failure message can print it. The names are ASCII by the
//! boundary's own vocabulary, so the narrowing is stated rather than assumed.
std::string Narrowed(const std::wstring_view shape)
{
	std::string narrowed;
	narrowed.reserve(shape.size());
	for (const auto character : shape) narrowed.push_back(static_cast<char>(character));
	return narrowed;
}

bool ShapeNeedsId(const std::wstring_view shape) noexcept
{
	return shape == L"issue" || shape == L"pull" || shape == L"issueComments"
		|| shape == L"issueComment" || shape == L"run" || shape == L"runJobs"
		|| shape == L"workflowRuns" || shape == L"runAttempt"
		|| shape == L"runAttemptJobs" || shape == L"job";
}

#if defined(_DEBUG)
/*!
	@brief The peak live CRT bytes one thread allocates while it is installed.

	The debug heap's high-water counter is process-wide since startup, so it
	cannot isolate one call; this hook nets allocations against frees on the
	measuring thread only, and the peak of that running total is the transient
	the call added above what the thread already held.
*/
class ThreadAllocPeak final {
public:
	ThreadAllocPeak() noexcept : m_thread(::GetCurrentThreadId())
	{
		s_active = this;
		m_previous = _CrtSetAllocHook(&Hook);
	}
	~ThreadAllocPeak()
	{
		(void)_CrtSetAllocHook(m_previous);
		s_active = nullptr;
	}
	ThreadAllocPeak(const ThreadAllocPeak&) = delete;
	ThreadAllocPeak& operator=(const ThreadAllocPeak&) = delete;

	[[nodiscard]] std::size_t Peak() const noexcept { return m_peak; }

private:
	static int __cdecl Hook(int allocType, void* data, size_t size, int blockUse,
		long, const unsigned char*, int)
	{
		auto* self = s_active;
		if (!self || blockUse == _CRT_BLOCK || ::GetCurrentThreadId() != self->m_thread) return TRUE;
		if (allocType == _HOOK_ALLOC) {
			self->m_live += size;
		} else if (allocType == _HOOK_REALLOC) {
			self->m_live += size - _msize_dbg(data, blockUse);
		} else if (allocType == _HOOK_FREE) {
			self->m_live -= _msize_dbg(data, blockUse);
		}
		if (self->m_live > self->m_peak) self->m_peak = self->m_live;
		return TRUE;
	}

	inline static ThreadAllocPeak* s_active = nullptr;
	_CRT_ALLOC_HOOK m_previous{};
	DWORD m_thread{};
	std::size_t m_live{};
	std::size_t m_peak{};
};
#endif

} // namespace

//! The projection transient per input byte, measured 2026-10-18 in Debug (see
//! OnePageProjectionPeaksUnderAMeasuredHeapCeiling). A list page peaks inside
//! the parse at 7.65x its body; the ceiling adds a quarter. A detail page whose
//! body string fills it peaks while serializing at 6.20x, and at 8.65x when the
//! source DOM is still alive there, so its ceiling sits between the two: the
//! debug heap's sizes are deterministic for one build, and the ceiling has to
//! fail a projection that keeps both trees.
constexpr std::size_t kListProjectionPeakRatioCeiling = 10;
constexpr std::size_t kDetailProjectionPeakRatioCeiling = 7;

TEST(GhPageProjection, KeepsExactlyTheMembersTheIssueShapeNames)
{
	// One issue as GitHub really answers it: the members the extension reads are
	// a small part of what arrives, and the body alone can outweigh all of them.
	const std::string page = R"([{"id":11,"number":7,"title":"Visible issue",)"
		R"("state":"open","user":{"login":"octocat","id":583231,"site_admin":false},)"
		R"("labels":[{"id":1,"name":"bug","color":"d73a4a","description":"broken"}],)"
		R"("html_url":"https://github.com/o/r/issues/7","comments":2,)"
		R"("body":"a very long report","reactions":{"total_count":3,"+1":3},)"
		R"("closed_at":null,"author_association":"OWNER"}])";
	const auto projected = ProjectRepositoryPage(L"issues", page);
	ASSERT_TRUE(projected.has_value());
	EXPECT_EQ(R"([{"comments":2,"html_url":"https://github.com/o/r/issues/7","id":11,)"
		R"("labels":[{"name":"bug"}],"number":7,"state":"open","title":"Visible issue",)"
		R"("user":{"login":"octocat"}}])", *projected);
}

TEST(GhPageProjection, KeepsAPullRequestMarkerAsPresenceAndNeverInventsOne)
{
	// The marker is read as presence alone, so an issue must come back without
	// the member and a pull request with it. A null in its place would say every
	// issue is a pull request, which is how a filtered list would silently empty.
	const std::string page = R"([{"id":11,"number":7,"title":"Issue","state":"open",)"
		R"("user":{"login":"octocat"},"labels":[],"html_url":"https://github.com/o/r/issues/7",)"
		R"("comments":0},)"
		R"({"id":12,"number":8,"title":"Pull","state":"open","user":{"login":"hubot"},)"
		R"("labels":[],"html_url":"https://github.com/o/r/pull/8","comments":0,)"
		R"("pull_request":{"url":"https://api.github.com/repos/o/r/pulls/8","merged_at":null}}])";
	const auto projected = ProjectRepositoryPage(L"issues", page);
	ASSERT_TRUE(projected.has_value());
	EXPECT_EQ(std::string::npos, projected->find(R"("pull_request":null)"));
	EXPECT_NE(std::string::npos, projected->find(R"("pull_request":{},)"));
	EXPECT_EQ(std::string::npos, projected->find("api.github.com"));
}

TEST(GhPageProjection, KeepsPlainStringArraysAndNestedRefsTheirShapesName)
{
	const std::string jobs = R"({"total_count":1,"jobs":[{"id":5,"run_id":6,"run_attempt":1,)"
		R"("name":"build","status":"completed","conclusion":"success","started_at":"t0",)"
		R"("completed_at":"t1","html_url":"https://github.com/o/r/actions/runs/6/job/5",)"
		R"("head_sha":"abc","runner_id":9,"runner_name":"runner","runner_group_id":1,)"
		R"("runner_group_name":"default","labels":["ubuntu-latest"],)"
		R"("steps":[{"number":1,"name":"checkout","status":"completed","conclusion":"success",)"
		R"("started_at":"t0","completed_at":"t1","extra":"dropped"}],"check_run_url":"dropped"}]})";
	const auto projectedJobs = ProjectRepositoryPage(L"runJobs", jobs);
	ASSERT_TRUE(projectedJobs.has_value());
	// A job's labels are strings, so the shape keeps them whole; an issue's are
	// objects, so that shape names the one member it keeps. Both are the same rule.
	EXPECT_NE(std::string::npos, projectedJobs->find(R"("labels":["ubuntu-latest"])"));
	EXPECT_NE(std::string::npos, projectedJobs->find(R"("total_count":1)"));
	EXPECT_EQ(std::string::npos, projectedJobs->find("dropped"));

	const std::string pulls = R"([{"id":51,"number":8,"title":"Change","state":"open",)"
		R"("user":{"login":"contributor"},"labels":[],"html_url":"https://github.com/b/p/pull/8",)"
		R"("comments":2,"draft":false,"merged_at":null,)"
		R"("base":{"ref":"main","sha":"bbbb","label":"dropped","repo":{"full_name":"base/project","id":7}},)"
		R"("head":{"ref":"feature","sha":"hhhh","repo":{"full_name":"fork/project"}}}])";
	const auto projectedPulls = ProjectRepositoryPage(L"pulls", pulls);
	ASSERT_TRUE(projectedPulls.has_value());
	EXPECT_NE(std::string::npos,
		projectedPulls->find(R"("base":{"ref":"main","repo":{"full_name":"base/project"},"sha":"bbbb"})"));
	EXPECT_EQ(std::string::npos, projectedPulls->find("dropped"));
}

TEST(GhPageProjection, EscapesWhatJsonRequiresAndRefusesWhatIsNotTheShapesAnswer)
{
	const std::string page = R"([{"id":1,"title":"line\nbreak \"quoted\" \\ tab\tend"}])";
	const auto projected = ProjectRepositoryPage(L"issues", page);
	ASSERT_TRUE(projected.has_value());
	// This compiler reads a macro argument before it reads a raw string, so an
	// expectation carrying backslashes is named first and only then compared.
	const std::string expected = page;
	EXPECT_EQ(expected, *projected);

	// A body that is not JSON, not an object or list, or names a shape nobody
	// stated fields for yields nothing. None of those can be answered short.
	EXPECT_FALSE(ProjectRepositoryPage(L"issues", "{").has_value());
	EXPECT_FALSE(ProjectRepositoryPage(L"issues", "42").has_value());
	EXPECT_FALSE(ProjectRepositoryPage(L"secrets", "[]").has_value());
	EXPECT_FALSE(ProjectRepositoryPage(L"", "[]").has_value());
}

TEST(GhPageProjection, EveryShapeARequestCanBeBuiltForHasAStatedFieldSet)
{
	const GhSelectedRepository repository(L"repo-identity-1", L"origin", L"github.com", L"owner", L"repo");
	for (const auto shape : kShapes) {
		const auto named = Narrowed(shape);
		EXPECT_TRUE(HasRepositoryPageProjection(shape)) << named;
		std::vector<senp::effect::Field> arguments{ { L"shape", std::wstring(shape) } };
		if (ShapeNeedsId(shape)) arguments.push_back({ L"id", L"42" });
		if (shape == L"runAttempt" || shape == L"runAttemptJobs") {
			arguments.push_back({ L"attempt", L"1" });
		}
		// The two sides of this are the point: a shape a request can be built for
		// must have fields, so a new shape cannot reach a page with none.
		EXPECT_TRUE(BuildRepositoryReadRequest(repository, arguments).has_value()) << named;
	}
	EXPECT_FALSE(HasRepositoryPageProjection(L"secrets"));
}

/*!
	@brief The heap one projection adds peaks under a ratio to its input.

	Every repository read and projection in a Control process is serialized:
	reads run on the executor's single worker, and projections run under its
	state mutex when a completion is taken. The transient one call adds is
	therefore what the process needs headroom for, and it is measured rather
	than estimated: the debug heap hook nets this thread's allocations against
	its frees while the call runs. A page is large in one of two ways - many
	small members (an `issues` list) or one long string its shape keeps whole
	(an `issue` whose body fills the page) - so both are measured. The numbers
	are printed so the owning guidance can state them.
*/
TEST(GhPageProjection, OnePageProjectionPeaksUnderAMeasuredHeapCeiling)
{
#if !defined(_DEBUG)
	GTEST_SKIP() << "the allocation hook needs the debug heap";
#else
	constexpr auto limit = kMaximumRepositoryPageBodyBytes;
	// The issues fixture repeated with index-varying ids to just under the
	// page-body bound. The body text is padded so the page reaches 1 MiB on few
	// enough nodes to stay inside the parser's 65,536-node bound.
	const std::string padding(768, 'a');
	std::string list = "[";
	for (std::size_t index = 0; list.size() < limit - 4096; ++index) {
		list += R"({"id":)" + std::to_string(index)
			+ R"(,"number":7,"title":"Visible issue","state":"open",)"
			+ R"("user":{"login":"octocat","id":583231,"site_admin":false},)"
			+ R"("labels":[{"id":1,"name":"bug","color":"d73a4a","description":"broken"}],)"
			+ R"("html_url":"https://github.com/o/r/issues/7","comments":2,)"
			+ R"("body":")" + padding + R"(","reactions":{"total_count":3,"+1":3},)"
			+ R"("closed_at":null,"author_association":"OWNER"},)";
	}
	list.back() = ']';
	// One issue whose body fills the page on a single line: nothing to escape,
	// so the serializer narrows the whole string at once - its worst case.
	std::string detail = R"({"id":11,"number":7,"title":"Visible issue","state":"open",)"
		R"("user":{"login":"octocat"},"labels":[],"html_url":"https://github.com/o/r/issues/7",)"
		R"("comments":2,"created_at":"2026-10-18T00:00:00Z","updated_at":"2026-10-18T00:00:00Z",)"
		R"("body":")";
	detail.append(limit - 4096 - detail.size(), 'a');
	detail += R"("})";

	for (const auto& [shape, page, ceiling] : {
		std::tuple{ std::wstring_view(L"issues"), &list, kListProjectionPeakRatioCeiling },
		std::tuple{ std::wstring_view(L"issue"), &detail, kDetailProjectionPeakRatioCeiling },
	}) {
		ASSERT_LT(page->size(), limit);
		std::size_t parsePeak = 0;
		{
			ThreadAllocPeak peak;
			const auto parsed = platform::serialization::CJsoncDocument::ParseStrict(*page);
			parsePeak = peak.Peak();
			ASSERT_TRUE(parsed.Succeeded());
		}
		std::optional<std::string> projected;
		std::size_t projectPeak = 0;
		{
			ThreadAllocPeak peak;
			projected = ProjectRepositoryPage(shape, *page);
			projectPeak = peak.Peak();
		}
		ASSERT_TRUE(projected.has_value());
		std::fprintf(stderr, "B10D heap: shape=%ls body=%zu parse-peak=%zu projection-peak=%zu\n",
			std::wstring(shape).c_str(), page->size(), parsePeak, projectPeak);
		EXPECT_LE(projectPeak, page->size() * ceiling) << std::wstring(shape);
	}
#endif
}
