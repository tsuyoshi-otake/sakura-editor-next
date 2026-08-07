/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/ITrustedFoldersStore.h"
#include "config/TrustedFoldersCodec.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using config::CTrustedFoldersCodec;
using config::ETrustedFoldersCodecStatus;
using config::kMaximumTrustedFolderEntries;
using config::kTrustedFoldersFormatVersion;
using config::TrustedFoldersSnapshot;
using config::WorkspaceTrustEntry;
using platform::uri::Uri;

Uri ParseUri(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceTrustEntry Entry(const wchar_t* text, bool includesDescendants)
{
	return { ParseUri(text), includesDescendants };
}

//! Asserts a decode rejection's exact status and that no partial document ever leaks out.
void ExpectRejected(std::string_view payload, ETrustedFoldersCodecStatus expectedStatus)
{
	const auto decoded = CTrustedFoldersCodec::Decode(payload);
	EXPECT_EQ(expectedStatus, decoded.status);
	EXPECT_FALSE(decoded.snapshot.has_value());
	EXPECT_FALSE(decoded.Succeeded());
}

} // namespace

// ---------------------------------------------------------------------------
// Encode / round trip
// ---------------------------------------------------------------------------

//! A snapshot with zero trusted folders is a valid document, not an error.
TEST(TrustedFoldersCodec, EmptySnapshotRoundTripsToZeroEntries)
{
	const TrustedFoldersSnapshot snapshot;

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const auto decoded = CTrustedFoldersCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_TRUE(decoded.snapshot->entries.empty());
}

TEST(TrustedFoldersCodec, SingleEntryRoundTripsUriAndIncludesDescendantsTrue)
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", true));

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const auto decoded = CTrustedFoldersCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(1U, decoded.snapshot->entries.size());
	EXPECT_EQ(snapshot.entries[0].uri.ToString(), decoded.snapshot->entries[0].uri.ToString());
	EXPECT_TRUE(decoded.snapshot->entries[0].includesDescendants);
}

TEST(TrustedFoldersCodec, SingleEntryRoundTripsUriAndIncludesDescendantsFalse)
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", false));

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const auto decoded = CTrustedFoldersCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(1U, decoded.snapshot->entries.size());
	EXPECT_EQ(snapshot.entries[0].uri.ToString(), decoded.snapshot->entries[0].uri.ToString());
	EXPECT_FALSE(decoded.snapshot->entries[0].includesDescendants);
}

TEST(TrustedFoldersCodec, MultipleEntriesRoundTripPreservingOrder)
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", true));
	snapshot.entries.push_back(Entry(L"file:///d:/vendor/lib", false));
	snapshot.entries.push_back(Entry(L"file://server/share", true));

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const auto decoded = CTrustedFoldersCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(3U, decoded.snapshot->entries.size());
	for (std::size_t index = 0; index < snapshot.entries.size(); ++index) {
		EXPECT_EQ(snapshot.entries[index].uri.ToString(), decoded.snapshot->entries[index].uri.ToString())
			<< "index " << index;
		EXPECT_EQ(snapshot.entries[index].includesDescendants, decoded.snapshot->entries[index].includesDescendants)
			<< "index " << index;
	}
}

//! Duplicate identical entries are a documented, deliberate acceptance — not a bug to "fix".
TEST(TrustedFoldersCodec, DuplicateIdenticalEntriesAreAcceptedAndRoundTripAsTwoEntries)
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", true));
	snapshot.entries.push_back(Entry(L"file:///c:/codes/app", true));

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const auto decoded = CTrustedFoldersCodec::Decode(encoded.payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(2U, decoded.snapshot->entries.size());
	EXPECT_EQ(decoded.snapshot->entries[0].uri.ToString(), decoded.snapshot->entries[1].uri.ToString());
	EXPECT_TRUE(decoded.snapshot->entries[0].includesDescendants);
	EXPECT_TRUE(decoded.snapshot->entries[1].includesDescendants);
}

//! Same decision, pinned directly against the wire format rather than through Encode.
TEST(TrustedFoldersCodec, DecodeAcceptsDuplicateIdenticalEntriesAsTwoEntries)
{
	const std::string payload = R"json({"formatVersion":1,"entries":[
		{"uri":"file:///c:/codes/app","includesDescendants":true},
		{"uri":"file:///c:/codes/app","includesDescendants":true}
	]})json";

	const auto decoded = CTrustedFoldersCodec::Decode(payload);
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	EXPECT_EQ(2U, decoded.snapshot->entries.size());
}

TEST(TrustedFoldersCodec, EntryCountOverMaximumIsRejectedAsInvalidSnapshot)
{
	TrustedFoldersSnapshot snapshot;
	snapshot.entries.assign(kMaximumTrustedFolderEntries + 1, Entry(L"file:///c:/codes/app", false));

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	EXPECT_EQ(ETrustedFoldersCodecStatus::InvalidSnapshot, encoded.status);
	EXPECT_TRUE(encoded.payload.empty());
}

TEST(TrustedFoldersCodec, EncodedPayloadContainsFormatVersion)
{
	const TrustedFoldersSnapshot snapshot;
	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	ASSERT_TRUE(encoded.Succeeded()) << encoded.diagnostic;

	const std::string expected = "\"formatVersion\":" + std::to_string(kTrustedFoldersFormatVersion);
	EXPECT_NE(std::string::npos, encoded.payload.find(expected));
}

// ---------------------------------------------------------------------------
// Decode rejections
// ---------------------------------------------------------------------------

TEST(TrustedFoldersCodec, DecodeRejectsInvalidJsonAsCorruptPayload)
{
	ExpectRejected("not json at all", ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsNonObjectTopLevelAsCorruptPayload)
{
	ExpectRejected("[]", ETrustedFoldersCodecStatus::CorruptPayload);
	ExpectRejected(R"("just a string")", ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsMissingEntriesAsCorruptPayload)
{
	ExpectRejected(R"({"formatVersion":1})", ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsNonArrayEntriesAsCorruptPayload)
{
	ExpectRejected(R"({"formatVersion":1,"entries":{}})", ETrustedFoldersCodecStatus::CorruptPayload);
	ExpectRejected(R"({"formatVersion":1,"entries":42})", ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsNonObjectEntryElementAsCorruptPayload)
{
	ExpectRejected(R"({"formatVersion":1,"entries":["not-an-object"]})", ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsEntryWithMissingOrNonStringUriAsCorruptPayload)
{
	ExpectRejected(
		R"({"formatVersion":1,"entries":[{"includesDescendants":true}]})",
		ETrustedFoldersCodecStatus::CorruptPayload);
	ExpectRejected(
		R"({"formatVersion":1,"entries":[{"uri":42,"includesDescendants":true}]})",
		ETrustedFoldersCodecStatus::CorruptPayload);
}

//! Uri::Parse rejects raw spaces and raw backslashes; the codec must not reinterpret that text.
TEST(TrustedFoldersCodec, DecodeRejectsUnparseableUriAsCorruptPayload)
{
	ExpectRejected(
		R"({"formatVersion":1,"entries":[{"uri":"C:\\bad uri","includesDescendants":false}]})",
		ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsNonBooleanIncludesDescendantsAsCorruptPayload)
{
	ExpectRejected(
		R"({"formatVersion":1,"entries":[{"uri":"file:///c:/codes/app","includesDescendants":"true"}]})",
		ETrustedFoldersCodecStatus::CorruptPayload);
	ExpectRejected(
		R"({"formatVersion":1,"entries":[{"uri":"file:///c:/codes/app","includesDescendants":1}]})",
		ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsEntryCountOverMaximumAsCorruptPayload)
{
	std::string entries = "[";
	for (std::size_t index = 0; index < kMaximumTrustedFolderEntries + 1; ++index) {
		if (index != 0) {
			entries += ",";
		}
		entries += R"({"uri":"file:///c:/codes/app","includesDescendants":false})";
	}
	entries += "]";
	const std::string payload = R"({"formatVersion":1,"entries":)" + entries + "}";

	ExpectRejected(payload, ETrustedFoldersCodecStatus::CorruptPayload);
}

TEST(TrustedFoldersCodec, DecodeRejectsMissingFormatVersionAsUnsupportedSchema)
{
	ExpectRejected(R"({"entries":[]})", ETrustedFoldersCodecStatus::UnsupportedSchema);
}

TEST(TrustedFoldersCodec, DecodeRejectsDifferentFormatVersionAsUnsupportedSchema)
{
	ExpectRejected(R"({"formatVersion":2,"entries":[]})", ETrustedFoldersCodecStatus::UnsupportedSchema);
}

TEST(TrustedFoldersCodec, DecodeRejectsNonNumericFormatVersionAsUnsupportedSchema)
{
	ExpectRejected(R"({"formatVersion":"1","entries":[]})", ETrustedFoldersCodecStatus::UnsupportedSchema);
}

//! Absence is not an error: an entry that never opted into descendant trust decodes as false.
TEST(TrustedFoldersCodec, AbsentIncludesDescendantsDecodesAsFalse)
{
	const auto decoded = CTrustedFoldersCodec::Decode(
		R"({"formatVersion":1,"entries":[{"uri":"file:///c:/codes/app"}]})");
	ASSERT_TRUE(decoded.Succeeded()) << decoded.diagnostic;
	ASSERT_EQ(1U, decoded.snapshot->entries.size());
	EXPECT_FALSE(decoded.snapshot->entries[0].includesDescendants);
}
