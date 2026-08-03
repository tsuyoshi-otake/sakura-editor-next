/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/statusbar/StatusbarViewModel.h"
#include "workbench/statusbar/StatusbarVisibilityMementoCodec.h"

namespace {

using workbench::statusbar::EStatusbarEntryAlignment;
using workbench::statusbar::StatusbarViewModel;
using workbench::statusbar::StatusbarVisibilityMementoCodec;

TEST(StatusbarViewModel, HiddenStateUsesStableIdsAndSurvivesEntryRefresh)
{
	StatusbarViewModel model;
	ASSERT_TRUE(model.SetEntries({
		{ "status.notifications", L"通知", EStatusbarEntryAlignment::Right, true },
		{ "publisher.extension.item", L"拡張項目", EStatusbarEntryAlignment::Left, true },
	}));
	EXPECT_TRUE(model.IsVisible("status.notifications"));
	ASSERT_TRUE(model.SetHidden("status.notifications", true));
	EXPECT_FALSE(model.IsVisible("status.notifications"));

	ASSERT_TRUE(model.SetEntries({
		{ "status.notifications", L"Notifications", EStatusbarEntryAlignment::Right, true },
	}));
	EXPECT_FALSE(model.IsVisible("status.notifications"));
	ASSERT_TRUE(model.Toggle("status.notifications"));
	EXPECT_TRUE(model.IsVisible("status.notifications"));
}

TEST(StatusbarViewModel, RejectsDuplicateStableIdsAndHonorsProviderVisibility)
{
	StatusbarViewModel model;
	EXPECT_FALSE(model.SetEntries({
		{ "same.id", L"A", EStatusbarEntryAlignment::Left, true },
		{ "same.id", L"B", EStatusbarEntryAlignment::Right, true },
	}));
	EXPECT_FALSE(model.IsVisible("same.id", false));
}

TEST(StatusbarVisibilityMementoCodec, RoundTripsCanonicalHiddenIdsAndRejectsInvalidPayloads)
{
	const std::vector<std::string> hidden{ "status.notifications", "publisher.extension.item" };
	const auto encoded = StatusbarVisibilityMementoCodec::Encode(hidden);
	ASSERT_TRUE(encoded.has_value());
	const auto decoded = StatusbarVisibilityMementoCodec::Decode(*encoded);
	ASSERT_TRUE(decoded.has_value());
	EXPECT_EQ(hidden, *decoded);

	EXPECT_FALSE(StatusbarVisibilityMementoCodec::Encode({ "duplicate", "duplicate" }).has_value());
	EXPECT_FALSE(StatusbarVisibilityMementoCodec::Decode("[\"duplicate\",\"duplicate\"]").has_value());
	EXPECT_FALSE(StatusbarVisibilityMementoCodec::Decode("{\"hidden\":[]}").has_value());
}

} // namespace
