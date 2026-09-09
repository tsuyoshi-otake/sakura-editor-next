/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/editor/SenpOwnerTextResources.h"

#include <string>

namespace workbench::editor {
namespace {

constexpr wchar_t kProfile[] = L"0123456789abcdef0123456789abcdef";

std::wstring Digest(const wchar_t fill = L'a')
{
	return std::wstring(64, fill);
}

senp::ContributionOwnerIdentity Owner(const std::int64_t generation = 7)
{
	senp::ContributionOwnerIdentity owner;
	owner.extensionId = L"sakura.github";
	owner.packageDigest = Digest();
	owner.generation = generation;
	owner.workspaceRevision = 3;
	owner.accountGeneration = 11;
	return owner;
}

SenpReadonlyScope Document(const std::int64_t generation = 7)
{
	SenpReadonlyScope scope;
	scope.extensionId = "sakura.github";
	scope.ownerGeneration = generation;
	scope.workspaceRevision = 3;
	scope.accountGeneration = 11;
	return scope;
}

senp::effect::TextResourceSection Section(std::wstring handle = L"log-1")
{
	senp::effect::TextResourceSection section;
	section.handle = std::move(handle);
	// Deliberately wrong: the extension's own account of a resource the control
	// side owns. A resolution that read it would be attributing this claim.
	section.length = 999999;
	return section;
}

TEST(SenpOwnerTextResources, ProjectsALiveOwnerOntoTheCohortItsResourcesAreCreatedUnder)
{
	CSenpOwnerTextResources resources(kProfile);
	ASSERT_TRUE(resources.Usable());
	ASSERT_TRUE(resources.Admit(Owner()));

	const auto scope = resources.Resolve(Document(), Section());
	ASSERT_TRUE(scope.has_value());
	EXPECT_EQ("0123456789abcdef0123456789abcdef", scope->profileId);
	EXPECT_EQ("sakura.github", scope->extensionId);
	EXPECT_EQ(std::string(64, 'a'), scope->packageDigest);
	EXPECT_EQ(7, scope->ownerGeneration);
	EXPECT_EQ(3, scope->workspaceRevision);
	EXPECT_EQ(11, scope->accountGeneration);
	// The control side creates its resources under the owner generation, and a
	// chunk carrying any other revision is refused by the view rather than shown.
	EXPECT_EQ(7, scope->revision);
	// No grant is minted on this side, so the scope names none.
	EXPECT_TRUE(scope->grantId.empty());
	EXPECT_TRUE(resources.IsCurrent(*scope, L"log-1"));
}

TEST(SenpOwnerTextResources, RefusesToResolveForAnOwnerItWasNeverToldAbout)
{
	CSenpOwnerTextResources resources(kProfile);
	EXPECT_FALSE(resources.Resolve(Document(), Section()).has_value());
	EXPECT_EQ(0u, resources.OwnerCount());

	ASSERT_TRUE(resources.Admit(Owner()));
	// A different activation of the same extension is a different owner, and the
	// document that named it is not the document this one publishes.
	EXPECT_FALSE(resources.Resolve(Document(8), Section()).has_value());
}

TEST(SenpOwnerTextResources, StopsAgreeingWithAScopeOnceItsOwnerIsRetired)
{
	CSenpOwnerTextResources resources(kProfile);
	ASSERT_TRUE(resources.Admit(Owner()));
	const auto scope = resources.Resolve(Document(), Section());
	ASSERT_TRUE(scope.has_value());

	resources.Retire(Owner());
	EXPECT_EQ(0u, resources.OwnerCount());
	// The scope keeps its values - it describes a cohort, not a permission - but
	// nothing this window shows may still be attributed to it.
	EXPECT_FALSE(resources.IsCurrent(*scope, L"log-1"));
	EXPECT_FALSE(resources.Resolve(Document(), Section()).has_value());
}

TEST(SenpOwnerTextResources, AnswersNothingWhenTheProfileIsNotAnOpaqueProfileIdentity)
{
	CSenpOwnerTextResources resources(L"profile/../other");
	EXPECT_FALSE(resources.Usable());
	EXPECT_FALSE(resources.Admit(Owner()));
	EXPECT_EQ(0u, resources.OwnerCount());
	EXPECT_FALSE(resources.Resolve(Document(), Section()).has_value());

	senp::TextResourceScope scope;
	scope.extensionId = "sakura.github";
	scope.ownerGeneration = 7;
	EXPECT_FALSE(resources.IsCurrent(scope, L"log-1"));
}

TEST(SenpOwnerTextResources, RefusesAnOwnerWhoseIdentityCannotBeCarriedByAResourceScope)
{
	CSenpOwnerTextResources resources(kProfile);

	auto uppercase = Owner();
	uppercase.extensionId = L"Sakura.GitHub";
	EXPECT_FALSE(resources.Admit(uppercase));

	auto empty = Owner();
	empty.extensionId.clear();
	EXPECT_FALSE(resources.Admit(empty));

	auto shortDigest = Owner();
	shortDigest.packageDigest = std::wstring(63, L'a');
	EXPECT_FALSE(resources.Admit(shortDigest));

	auto uppercaseDigest = Owner();
	uppercaseDigest.packageDigest = std::wstring(64, L'A');
	EXPECT_FALSE(resources.Admit(uppercaseDigest));

	// The store refuses a scope whose owner generation is not positive, so an
	// owner carrying one could never hold a resource to attribute.
	auto ungenerated = Owner(0);
	EXPECT_FALSE(resources.Admit(ungenerated));

	auto negative = Owner();
	negative.workspaceRevision = -1;
	EXPECT_FALSE(resources.Admit(negative));

	auto unaccounted = Owner();
	unaccounted.accountGeneration = -1;
	EXPECT_FALSE(resources.Admit(unaccounted));

	EXPECT_EQ(0u, resources.OwnerCount());
}

TEST(SenpOwnerTextResources, RefusesASecondPackageClaimingOneOwnerIdentityAndKeepsTheFirst)
{
	CSenpOwnerTextResources resources(kProfile);
	ASSERT_TRUE(resources.Admit(Owner()));

	// Admitting the same owner again states nothing new.
	EXPECT_TRUE(resources.Admit(Owner()));
	EXPECT_EQ(1u, resources.OwnerCount());

	auto other = Owner();
	other.packageDigest = Digest(L'b');
	EXPECT_FALSE(resources.Admit(other));
	EXPECT_EQ(1u, resources.OwnerCount());

	const auto scope = resources.Resolve(Document(), Section());
	ASSERT_TRUE(scope.has_value());
	EXPECT_EQ(std::string(64, 'a'), scope->packageDigest);
}

TEST(SenpOwnerTextResources, SeparatesOwnersThatDifferOnlyInGenerationWorkspaceOrAccount)
{
	CSenpOwnerTextResources resources(kProfile);
	auto workspace = Owner();
	workspace.workspaceRevision = 4;
	auto account = Owner();
	account.accountGeneration = 12;
	ASSERT_TRUE(resources.Admit(Owner()));
	ASSERT_TRUE(resources.Admit(Owner(8)));
	ASSERT_TRUE(resources.Admit(workspace));
	ASSERT_TRUE(resources.Admit(account));
	EXPECT_EQ(4u, resources.OwnerCount());

	const auto first = resources.Resolve(Document(), Section());
	const auto second = resources.Resolve(Document(8), Section());
	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_NE(*first, *second);
	EXPECT_TRUE(resources.IsCurrent(*first, L"log-1"));
	EXPECT_TRUE(resources.IsCurrent(*second, L"log-1"));

	// Retiring one activation leaves the others answering for themselves.
	resources.Retire(Owner(8));
	EXPECT_TRUE(resources.IsCurrent(*first, L"log-1"));
	EXPECT_FALSE(resources.IsCurrent(*second, L"log-1"));
}

TEST(SenpOwnerTextResources, AgreesWithAHandleItHasNeverSeenButNotWithOneNamingNothing)
{
	CSenpOwnerTextResources resources(kProfile);
	ASSERT_TRUE(resources.Admit(Owner()));
	const auto scope = resources.Resolve(Document(), Section());
	ASSERT_TRUE(scope.has_value());

	// Whether a handle names a resource is the control-side store's answer, and
	// reading it is how that answer is obtained. Refusing here would be this
	// side stating an absence it cannot observe.
	EXPECT_TRUE(resources.IsCurrent(*scope, L"never-read-anything"));
	EXPECT_FALSE(resources.IsCurrent(*scope, L""));
	EXPECT_FALSE(resources.IsCurrent(*scope,
		std::wstring(CSenpOwnerTextResources::kMaximumHandleCharacters + 1, L'x')));

	EXPECT_FALSE(resources.Resolve(Document(), Section(L"")).has_value());
	EXPECT_FALSE(resources.Resolve(Document(),
		Section(std::wstring(CSenpOwnerTextResources::kMaximumHandleCharacters + 1, L'x'))).has_value());
}

TEST(SenpOwnerTextResources, RefusesToRecordMoreOwnersThanItHoldsRoomFor)
{
	CSenpOwnerTextResources resources(kProfile);
	for (std::size_t index = 0; index < CSenpOwnerTextResources::kMaximumOwners; ++index) {
		ASSERT_TRUE(resources.Admit(Owner(static_cast<std::int64_t>(index) + 1)));
	}
	EXPECT_EQ(CSenpOwnerTextResources::kMaximumOwners, resources.OwnerCount());

	EXPECT_FALSE(resources.Admit(Owner(static_cast<std::int64_t>(CSenpOwnerTextResources::kMaximumOwners) + 1)));
	EXPECT_EQ(CSenpOwnerTextResources::kMaximumOwners, resources.OwnerCount());
	// One already recorded still answers, because it takes no new room.
	EXPECT_TRUE(resources.Admit(Owner(1)));
}

} // namespace
} // namespace workbench::editor
