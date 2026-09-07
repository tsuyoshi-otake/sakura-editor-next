/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/SenpTextResource.h"
#include <limits>

namespace {
using namespace senp;
TextResourceScope Scope()
{
	return { "profile-1", "sakura.github-actions", std::string(64, 'a'), "grant:9", 1, 2, 3, 4 };
}
TEST(SenpTextResource, ReadsArbitraryBoundedByteRangesAcrossFixedPages)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto created = store.Create(scope);
	ASSERT_EQ(created.result, TextResourceResult::Accepted);
	std::string expected(SenpTextResourceStore::kChunkBytes - 7, 'a');
	ASSERT_EQ(store.Append(scope, created.handle, 0, expected), TextResourceResult::Accepted);
	ASSERT_EQ(store.Append(scope, created.handle, expected.size(), "0123456789ABCDEFGHIJ"), TextResourceResult::Accepted);
	expected += "0123456789ABCDEFGHIJ";
	const auto chunk = store.Read(scope, created.handle, SenpTextResourceStore::kChunkBytes - 9, 18);
	EXPECT_EQ(chunk.result, TextResourceResult::Accepted); EXPECT_EQ(chunk.state, TextResourceState::Loading);
	EXPECT_EQ(chunk.length, expected.size()); EXPECT_EQ(chunk.bytes, expected.substr(chunk.offset, 18));
	EXPECT_EQ(store.Usage().allocatedBytes, 2 * SenpTextResourceStore::kChunkBytes);
	EXPECT_EQ(store.Finish(scope, created.handle, TextResourceEnd::Complete), TextResourceResult::Accepted);
	const auto end = store.Read(scope, created.handle, expected.size(), 1);
	EXPECT_EQ(end.state, TextResourceState::Complete); EXPECT_TRUE(end.bytes.empty());
	EXPECT_EQ(store.Append(scope, created.handle, expected.size(), "late"), TextResourceResult::Terminal);
}
TEST(SenpTextResource, EveryAuthorizationDimensionAndResourceRevisionFenceAllOperations)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto created = store.Create(scope);
	ASSERT_EQ(store.Append(scope, created.handle, 0, "private"), TextResourceResult::Accepted);
	for (int dimension = 0; dimension < 8; ++dimension) {
		auto foreign = scope;
		switch (dimension) {
		case 0: foreign.profileId += "-other"; break; case 1: foreign.extensionId += "-other"; break;
		case 2: foreign.packageDigest[0] = 'b'; break; case 3: foreign.grantId += "-other"; break;
		case 4: ++foreign.ownerGeneration; break; case 5: ++foreign.workspaceRevision; break;
		case 6: ++foreign.accountGeneration; break; case 7: ++foreign.revision; break;
		}
		const auto read = store.Read(foreign, created.handle, 0, 100);
		EXPECT_EQ(read.result, TextResourceResult::NotFound); EXPECT_TRUE(read.bytes.empty()); EXPECT_TRUE(read.handle.empty());
		EXPECT_EQ(store.Append(foreign, created.handle, 7, "!"), TextResourceResult::NotFound);
		EXPECT_EQ(store.Finish(foreign, created.handle, TextResourceEnd::Complete), TextResourceResult::NotFound);
		EXPECT_EQ(store.Expire(foreign, created.handle), TextResourceResult::NotFound);
		EXPECT_EQ(store.Release(foreign, created.handle), TextResourceResult::NotFound);
	}
	EXPECT_EQ(store.Read(scope, created.handle, 0, 100).bytes, "private");
}
TEST(SenpTextResource, HandlesNeverReplayAfterReleaseOrStoreRecreation)
{
	const auto scope = Scope(); std::wstring previous;
	{ SenpTextResourceStore store; previous = store.Create(scope).handle;
		EXPECT_EQ(store.Release(scope, previous), TextResourceResult::Accepted);
		EXPECT_NE(store.Create(scope).handle, previous);
		EXPECT_EQ(store.Read(scope, previous, 0, 1).result, TextResourceResult::NotFound); }
	SenpTextResourceStore next; EXPECT_NE(next.Create(scope).handle, previous);
	EXPECT_EQ(next.Read(scope, previous, 0, 1).result, TextResourceResult::NotFound);
}
TEST(SenpTextResource, MalformedScopesAndRangesChangeNothing)
{
	SenpTextResourceStore store; const auto scope = Scope();
	for (int dimension = 0; dimension < 8; ++dimension) {
		auto invalid = scope;
		switch (dimension) {
		case 0: invalid.profileId = "../profile"; break; case 1: invalid.extensionId = "UPPER"; break;
		case 2: invalid.packageDigest[0] = 'X'; break; case 3: invalid.grantId.assign(129, 'a'); break;
		case 4: invalid.ownerGeneration = 0; break; case 5: invalid.workspaceRevision = -1; break;
		case 6: invalid.accountGeneration = -1; break; case 7: invalid.revision = -1; break;
		}
		EXPECT_EQ(store.Create(invalid).result, TextResourceResult::Invalid);
	}
	EXPECT_EQ(store.Usage().resources, 0u); const auto id = store.Create(scope).handle;
	EXPECT_EQ(store.Append(scope, id, 0, {}), TextResourceResult::Invalid);
	EXPECT_EQ(store.Append(scope, id, 1, "x"), TextResourceResult::Stale);
	EXPECT_EQ(store.Append(scope, id, 0, std::string(SenpTextResourceStore::kChunkBytes + 1, 'x')), TextResourceResult::Invalid);
	EXPECT_EQ(store.Read(scope, id, 0, 0).result, TextResourceResult::Invalid);
	EXPECT_EQ(store.Read(scope, id, 1, 1).result, TextResourceResult::Invalid);
	EXPECT_EQ(store.Read(scope, id, 0, (std::numeric_limits<std::size_t>::max)()).result, TextResourceResult::Invalid);
	EXPECT_EQ(store.Usage().allocatedBytes, 0u);
}
TEST(SenpTextResource, EmptyCompleteFailureAndPartialFailuresRemainDistinctTerminals)
{
	SenpTextResourceStore store; const auto scope = Scope();
	for (auto reason : { TextResourceEnd::Complete, TextResourceEnd::Failed, TextResourceEnd::Cancelled, TextResourceEnd::LimitExceeded }) {
		for (bool prefix : { false, true }) {
			const auto id = store.Create(scope).handle;
			if (prefix) ASSERT_EQ(store.Append(scope, id, 0, "prefix"), TextResourceResult::Accepted);
			ASSERT_EQ(store.Finish(scope, id, reason), TextResourceResult::Accepted);
			const auto read = store.Read(scope, id, 0, 99);
			EXPECT_EQ(read.end, reason); EXPECT_EQ(read.bytes, prefix ? "prefix" : "");
			EXPECT_EQ(read.state, reason == TextResourceEnd::Complete ? TextResourceState::Complete
				: prefix ? TextResourceState::Partial : TextResourceState::Failed);
			EXPECT_EQ(store.Finish(scope, id, TextResourceEnd::Complete), TextResourceResult::Terminal);
		}
	}
}
TEST(SenpTextResource, ResourceLimitPreservesOnlyTheAcceptedPrefixAndRequiresProducerFinalization)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto id = store.Create(scope).handle;
	const std::string chunk(SenpTextResourceStore::kChunkBytes, 'x');
	for (std::size_t offset = 0; offset < SenpTextResourceStore::kResourceBytes; offset += chunk.size())
		ASSERT_EQ(store.Append(scope, id, offset, chunk), TextResourceResult::Accepted);
	EXPECT_EQ(store.Append(scope, id, SenpTextResourceStore::kResourceBytes, "!"), TextResourceResult::LimitReached);
	const auto read = store.Read(scope, id, 0, 1);
	EXPECT_EQ(read.length, SenpTextResourceStore::kResourceBytes); EXPECT_EQ(read.state, TextResourceState::Partial);
	EXPECT_EQ(read.end, TextResourceEnd::LimitExceeded); EXPECT_EQ(read.bytes, "x");
	EXPECT_EQ(store.Append(scope, id, read.length, "!"), TextResourceResult::Terminal);
	EXPECT_EQ(store.Release(scope, id), TextResourceResult::Accepted); EXPECT_EQ(store.Usage().allocatedBytes, 0u);
}
TEST(SenpTextResource, TotalBudgetCountsAllocatedPagesAndNeverEvictsAnotherReader)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto first = store.Create(scope).handle;
	const auto second = store.Create(scope).handle; const auto third = store.Create(scope).handle;
	const std::string chunk(SenpTextResourceStore::kChunkBytes, 'x');
	for (const auto& id : { first, second })
		for (std::size_t offset = 0; offset < SenpTextResourceStore::kResourceBytes; offset += chunk.size())
			ASSERT_EQ(store.Append(scope, id, offset, chunk), TextResourceResult::Accepted);
	EXPECT_EQ(store.Usage().allocatedBytes, SenpTextResourceStore::kTotalBytes);
	EXPECT_EQ(store.Append(scope, third, 0, "x"), TextResourceResult::LimitReached);
	EXPECT_EQ(store.Read(scope, third, 0, 1).state, TextResourceState::Failed);
	EXPECT_EQ(store.Read(scope, first, 0, 1).bytes, "x");
	EXPECT_EQ(store.Expire(scope, first), TextResourceResult::Expired);
	EXPECT_EQ(store.Usage().allocatedBytes, SenpTextResourceStore::kResourceBytes);
	const auto fourth = store.Create(scope).handle;
	ASSERT_EQ(store.Append(scope, fourth, 0, "x"), TextResourceResult::Accepted);
	EXPECT_EQ(store.Usage().allocatedBytes, SenpTextResourceStore::kResourceBytes + SenpTextResourceStore::kChunkBytes);
}
TEST(SenpTextResource, ExpiryErasesPrivateBytesAndCloseReclaimsAllSlots)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto id = store.Create(scope).handle;
	ASSERT_EQ(store.Append(scope, id, 0, "private"), TextResourceResult::Accepted);
	EXPECT_EQ(store.Expire(scope, id), TextResourceResult::Expired);
	const auto expired = store.Read(scope, id, 7, 1);
	EXPECT_EQ(expired.result, TextResourceResult::Expired); EXPECT_EQ(expired.state, TextResourceState::Expired);
	EXPECT_TRUE(expired.bytes.empty()); EXPECT_EQ(expired.length, 0u); EXPECT_EQ(store.Usage().bytes, 0u);
	for (std::size_t i = 1; i < SenpTextResourceStore::kResources; ++i) ASSERT_EQ(store.Create(scope).result, TextResourceResult::Accepted);
	EXPECT_EQ(store.Create(scope).result, TextResourceResult::LimitReached);
	store.Close(); store.Close(); EXPECT_TRUE(store.Usage().closed); EXPECT_EQ(store.Usage().resources, 0u);
	EXPECT_EQ(store.Create(scope).result, TextResourceResult::Closed);
	EXPECT_EQ(store.Read(scope, id, 0, 1).result, TextResourceResult::Closed);
	EXPECT_EQ(store.Release(scope, id), TextResourceResult::Closed);
}
TEST(SenpTextResource, ByteSizedAppendsShareFixedPagesAndNeverCreatePerChunkMetadata)
{
	SenpTextResourceStore store; const auto scope = Scope(); const auto id = store.Create(scope).handle;
	for (std::size_t offset = 0; offset < 10000; ++offset) ASSERT_EQ(store.Append(scope, id, offset, "x"), TextResourceResult::Accepted);
	EXPECT_EQ(store.Usage().bytes, 10000u); EXPECT_EQ(store.Usage().allocatedBytes, SenpTextResourceStore::kChunkBytes);
	EXPECT_EQ(store.Read(scope, id, 9997, 3).bytes, "xxx");
}
TEST(SenpTextResource, Utf8ScalarsSurviveEverySplitAndByteAtATimeDelivery)
{
	const std::string input = "A\xc2\xa2\xe6\x97\xa5\xf0\x9f\x98\x80Z";
	const std::wstring expected = L"A\u00a2\u65e5\xd83d\xde00Z";
	for (std::size_t split = 0; split <= input.size(); ++split) {
		SenpTextResourceDecoder decoder;
		const auto first = decoder.Decode(0, std::string_view(input).substr(0, split), false);
		ASSERT_EQ(first.result, TextDecodeResult::Accepted); EXPECT_LE(decoder.PendingBytes(), 3u);
		const auto second = decoder.Decode(split, std::string_view(input).substr(split), true);
		ASSERT_EQ(second.result, TextDecodeResult::Accepted); EXPECT_EQ(first.text + second.text, expected); EXPECT_TRUE(decoder.IsClosed());
	}
	SenpTextResourceDecoder decoder; std::wstring output;
	for (std::size_t i = 0; i < input.size(); ++i) { const auto part = decoder.Decode(i, std::string_view(input).substr(i, 1), i + 1 == input.size());
		ASSERT_EQ(part.result, TextDecodeResult::Accepted); output += part.text; EXPECT_LE(decoder.PendingBytes(), 3u); }
	EXPECT_EQ(output, expected); EXPECT_EQ(decoder.Offset(), input.size());
}
TEST(SenpTextResource, InvalidUtf8RejectsTheCurrentChunkAndClosesWithoutReplacingAnEarlierPrefix)
{
	for (const std::string input : { "\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe6\x20", "\xf0\x80", "\xff" }) {
		SenpTextResourceDecoder decoder; ASSERT_EQ(decoder.Decode(0, "ok", false).text, L"ok");
		const auto bad = decoder.Decode(2, input, false);
		EXPECT_EQ(bad.result, TextDecodeResult::InvalidUtf8); EXPECT_TRUE(bad.text.empty()); EXPECT_TRUE(decoder.IsClosed());
		EXPECT_EQ(decoder.Decode(2, "late", true).result, TextDecodeResult::Closed);
	}
}
TEST(SenpTextResource, TruncatedScalarsAreExplicitFailureOnlyAtFinalization)
{
	for (const std::string input : { "\xc2", "\xe6\x97", "\xf0\x9f\x98" }) {
		SenpTextResourceDecoder decoder; const auto part = decoder.Decode(0, input, false);
		EXPECT_EQ(part.result, TextDecodeResult::Accepted); EXPECT_TRUE(part.text.empty()); EXPECT_EQ(decoder.PendingBytes(), input.size());
		EXPECT_EQ(decoder.Decode(input.size(), {}, true).result, TextDecodeResult::InvalidUtf8);
		EXPECT_EQ(decoder.PendingBytes(), 0u);
	}
}
TEST(SenpTextResource, ControlsAnsiOscAndBidiCannotPerformActionsAndNewlinesNormalizeAcrossChunks)
{
	SenpTextResourceDecoder decoder;
	// Use size from the literal, not a NUL-terminated string: NUL is visible too.
	const char bytes[] = "a\r\nb\rc\n\t\0\x1b]52;c;c2VjcmV0\x07\x1b[31m\x7f\xc2\x9b\xe2\x80\xae";
	std::wstring output;
	for (std::size_t i = 0; i < sizeof(bytes) - 1; ++i) {
		const auto part = decoder.Decode(i, std::string_view(bytes + i, 1), i + 2 == sizeof(bytes));
		ASSERT_EQ(part.result, TextDecodeResult::Accepted); output += part.text;
	}
	EXPECT_EQ(output, L"a\nb\nc\n\t\u2400\u241b]52;c;c2VjcmV0\u2407\u241b[31m\u2421\ufffd\ufffd");
}
TEST(SenpTextResource, DecoderRejectsStaleAndOversizedInputWithoutConsumingAndEnforcesTheTotalBound)
{
	SenpTextResourceDecoder decoder; const std::string page(SenpTextResourceStore::kChunkBytes, 'x');
	EXPECT_EQ(decoder.Decode(1, "x", false).result, TextDecodeResult::Stale);
	EXPECT_EQ(decoder.Decode(0, page + "x", false).result, TextDecodeResult::Invalid);
	EXPECT_EQ(decoder.Offset(), 0u);
	for (std::size_t offset = 0; offset < SenpTextResourceStore::kResourceBytes; offset += page.size()) {
		const auto part = decoder.Decode(offset, page, false); ASSERT_EQ(part.result, TextDecodeResult::Accepted);
		ASSERT_EQ(part.text.size(), page.size());
	}
	EXPECT_EQ(decoder.Decode(decoder.Offset(), "x", false).result, TextDecodeResult::LimitReached);
	EXPECT_TRUE(decoder.IsClosed());
}
} // namespace
