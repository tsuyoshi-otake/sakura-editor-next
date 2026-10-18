/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpRuntimeService.h"
#include <string>
#include <vector>

namespace {

// The exact bytes the v1 host sends; the producer side is pinned byte for byte
// by `hello_reply_is_the_exact_v1_handshake_bytes` in `sakura_senp_host/src/main.rs`.
constexpr std::string_view kHello =
	R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0"})";
constexpr std::string_view kDecorations =
	R"({"type":"decorations","slots":[{"line":0,"visualStart":0,"visualLength":1,"depth":0}]})";

TEST(SenpRuntimeService, HelloReplyRequiresExactlyTheV1Members)
{
	using senp::detail::AcceptExtensionHello;
	EXPECT_TRUE(AcceptExtensionHello(kHello));
	// Strict JSON only: comments and trailing commas are not the protocol.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello",/*c*/"protocol":1,"abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0",})"));
	// The marker text inside another member's value is not a hello reply.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"error","detail":{"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0"}})"));
	// Exactly the three members: wrong values and extra/missing members fail.
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":2,"abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1,"abi":"sakura:senp/extension@1.0.0","extra":1})"));
	EXPECT_FALSE(AcceptExtensionHello(
		R"({"type":"hello","protocol":1})"));
	EXPECT_FALSE(AcceptExtensionHello(R"({"type":"hello","protocol":"1","abi":"sakura:senp/extension@1.0.0"})"));
	EXPECT_FALSE(AcceptExtensionHello(R"(["type","hello","protocol",1])"));
	EXPECT_FALSE(AcceptExtensionHello(""));
}

TEST(SenpRuntimeService, DecorationsReplyUsesStrictJson)
{
	using senp::detail::ParseIndentDecorations;
	std::vector<senp::IndentDecoration> target;
	ASSERT_TRUE(ParseIndentDecorations(kDecorations, target));
	ASSERT_EQ(target.size(), 1U);
	EXPECT_EQ(target[0].visualStart, 0U);
	EXPECT_EQ(target[0].visualLength, 1U);
	EXPECT_EQ(target[0].depth, 0U);
	// Strict JSON only: a trailing comma or a comment is not the protocol.
	EXPECT_FALSE(ParseIndentDecorations(
		R"({"type":"decorations","slots":[{"line":0,"visualStart":0,"visualLength":1,"depth":0},]})", target));
	EXPECT_FALSE(ParseIndentDecorations(
		R"({"type":"decorations",/*c*/"slots":[]})", target));
	// Shape checks that already held keep holding.
	EXPECT_FALSE(ParseIndentDecorations(R"({"type":"other","slots":[]})", target));
	EXPECT_FALSE(ParseIndentDecorations(
		R"({"type":"decorations","slots":[{"line":0,"visualStart":-1,"visualLength":1,"depth":0}]})", target));
	EXPECT_FALSE(ParseIndentDecorations("", target));
}

} // namespace
