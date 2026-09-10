/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhPageProjection.h"

#include "senp/github/GhRepositorySelection.h"
#include "senp/github/SenpGitHubToolExecutor.h"

#include <array>
#include <string>

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

} // namespace

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
