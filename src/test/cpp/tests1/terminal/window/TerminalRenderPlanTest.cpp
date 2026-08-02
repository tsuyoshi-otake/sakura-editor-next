/*! @file */
#include "pch.h"

#include "terminal/model/TerminalModel.h"
#include "terminal/window/TerminalDWriteRenderer.h"
#include "terminal/window/TerminalRenderPlan.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace terminal {
namespace {

class RecordingClassifier final : public ITerminalRenderClassifier {
public:
	TerminalRenderClassification classification{ TerminalRenderClassification::GdiSimple };
	std::size_t calls{};
	std::uint64_t generation{ 1 };

	TerminalRenderClassification Classify(std::wstring_view, bool) noexcept override
	{
		++calls;
		return classification;
	}
	std::uint64_t Generation() const noexcept override { return generation; }
};

TerminalRenderStyle ResolveStyle(void*, const TerminalAttributes&, bool selected) noexcept
{
	return { RGB(0xDC, 0xDF, 0xE4), selected ? RGB(0x40, 0x50, 0x70) : RGB(0x28, 0x2C, 0x34),
		false, false, false, selected };
}

struct CountingStyleResolverContext final {
	std::size_t calls{};
};

TerminalRenderStyle CountingStyle(const TerminalAttributes& attributes, bool selected) noexcept
{
	const auto value = attributes.foreground.value;
	return {
		RGB(static_cast<BYTE>((value >> 16) & 0xFF), static_cast<BYTE>((value >> 8) & 0xFF),
			static_cast<BYTE>(value & 0xFF)),
		selected ? RGB(0x40, 0x50, 0x70) : RGB(0x28, 0x2C, 0x34),
		attributes.bold, attributes.underline, attributes.inverse, selected, false,
	};
}

TerminalRenderStyle ResolveCountingStyle(void* context, const TerminalAttributes& attributes, bool selected) noexcept
{
	++static_cast<CountingStyleResolverContext*>(context)->calls;
	return CountingStyle(attributes, selected);
}

struct SurfaceStyleCase final {
	COLORREF background{ RGB(0x28, 0x2C, 0x34) };
	bool inverse{};
	bool usesSurfaceDefaultBackground{};
};

TerminalRenderStyle ResolveSurfaceStyle(void* context, const TerminalAttributes&, bool selected) noexcept
{
	const auto& style = *static_cast<const SurfaceStyleCase*>(context);
	return { RGB(0xDC, 0xDF, 0xE4), style.background, false, false, style.inverse, selected,
		style.usesSurfaceDefaultBackground };
}

void Print( TerminalModel& model, std::u32string_view text )
{
	for( const auto codepoint : text ) model.Print(codepoint);
}

TerminalRenderPlanBuildInput Input(TerminalModel& model, RecordingClassifier& classifier,
	std::size_t visibleRows, RECT paintRect, int cellWidth = 8, int cellHeight = 16,
	bool surfaceClearedToDefaultBackground = false)
{
	return {
		&model,
		{ visibleRows, visibleRows, 0 },
		paintRect,
		cellWidth,
		cellHeight,
		false,
		{},
		{},
		&classifier,
		&ResolveStyle,
		nullptr,
		surfaceClearedToDefaultBackground,
	};
}

TEST(TerminalRenderPlan, AsciiBypassesClassifierAndDWrite)
{
	TerminalModel model(32, 1);
	Print(model, U"$ echo sakura");
	RecordingClassifier classifier;
	TerminalRenderPlan plan;

	ASSERT_TRUE(plan.Build(Input(model, classifier, 1, { 0, 0, 256, 16 })));
	EXPECT_EQ(0u, classifier.calls);
	EXPECT_TRUE(plan.ShapedClusters().empty());
	EXPECT_EQ(1u, plan.GdiRuns().size());
	EXPECT_EQ(0u, plan.BuiltinGlyphs().size());
	// A renderer is intentionally untouched for an ASCII-only plan.  This is
	// the same gate used by CTerminalWnd before Configure/BeginFrame.
	TerminalDWriteRenderer renderer;
	EXPECT_EQ(TerminalDWriteLifecycle::Dormant, renderer.Lifecycle());
	EXPECT_EQ(0u, renderer.Counters().factoryCreationAttempts);
	EXPECT_EQ(0u, renderer.Counters().frameBegins);
	EXPECT_EQ(0u, renderer.Counters().endDrawCalls);
}

TEST(TerminalRenderPlan, ElidesBackgroundOnlyForOptedInSemanticSurfaceDefault)
{
	struct Case final {
		const char* name;
		bool surfaceCleared{};
		bool usesSurfaceDefault{};
		COLORREF background{};
		bool inverse{};
		bool selected{};
		bool emitsBackground{};
	};
	constexpr COLORREF defaultBackground = RGB(0x28, 0x2C, 0x34);
	const std::array cases{
		Case{ "both flags true", true, true, defaultBackground, false, false, false },
		Case{ "surface not cleared", false, true, defaultBackground, false, false, true },
		Case{ "semantic bit false", true, false, defaultBackground, false, false, true },
		// Equal RGB is not enough: this is a custom color semantically, so it
		// must remain an explicit background span.
		Case{ "custom equal RGB", true, false, defaultBackground, false, false, true },
		Case{ "selected", true, false, RGB(0x40, 0x50, 0x70), false, true, true },
		Case{ "inverse", true, false, defaultBackground, true, false, true },
	};

	for( const auto& testCase : cases ) {
		TerminalModel model(1, 1);
		model.Print(U'A');
		RecordingClassifier classifier;
		SurfaceStyleCase style{ testCase.background, testCase.inverse, testCase.usesSurfaceDefault };
		TerminalRenderPlan plan;
		auto input = Input(model, classifier, 1, { 0, 0, 8, 16 }, 8, 16, testCase.surfaceCleared);
		input.styleResolver = &ResolveSurfaceStyle;
		input.styleResolverContext = &style;
		input.hasSelection = testCase.selected;
		input.selectionAnchor = { 0, 0 };
		input.selectionActive = { 0, 1 };
		ASSERT_TRUE(plan.Build(input)) << testCase.name;
		EXPECT_EQ(testCase.emitsBackground ? 1u : 0u, plan.BackgroundSpans().size()) << testCase.name;
	}
}

TEST(TerminalRenderPlan, ShapesPrimaryMissAndPlayGlyphWithoutClassifierCoverageProbe)
{
	TerminalModel model(16, 1);
	model.Print(U'\u65E5');
	model.Print(U'\u23F5');
	RecordingClassifier classifier;
	classifier.classification = TerminalRenderClassification::ShapedFallback;
	TerminalRenderPlan plan;

	ASSERT_TRUE(plan.Build(Input(model, classifier, 1, { 0, 0, 256, 16 })));
	ASSERT_EQ(2u, plan.ShapedClusters().size());
	EXPECT_EQ(1u, classifier.calls); // U+23F5 has a deterministic shaped route.
	EXPECT_EQ(L"\u65E5", plan.Text(plan.ShapedClusters()[0].textOffset, plan.ShapedClusters()[0].textLength));
	EXPECT_EQ(L"\u23F5", plan.Text(plan.ShapedClusters()[1].textOffset, plan.ShapedClusters()[1].textLength));
	EXPECT_EQ(0u, plan.GdiRuns().size());
}

TEST(TerminalRenderPlan, PreservesModelWidthsForUnicodeClusters)
{
	TerminalModel model(32, 1);
	Print(model, U"\u65E5e\u0301\u2764\uFE0F\U0001F469\u200D\U0001F4BB\U0001F1EF\U0001F1F5\U0001F44D\U0001F3FD");
	ASSERT_GE(model.Rows()[0].cells.size(), 12u);
	EXPECT_EQ(2u, model.Rows()[0].cells[0].width);
	EXPECT_EQ(L"e\u0301", model.Rows()[0].cells[2].Text());
	EXPECT_EQ(2u, model.Rows()[0].cells[3].width);
	EXPECT_EQ(5u, model.Rows()[0].cells[5].length);
	EXPECT_EQ(4u, model.Rows()[0].cells[7].length);
	EXPECT_EQ(4u, model.Rows()[0].cells[9].length);

	RecordingClassifier classifier;
	classifier.classification = TerminalRenderClassification::ShapedFallback;
	TerminalRenderPlan plan;
	ASSERT_TRUE(plan.Build(Input(model, classifier, 1, { 0, 0, 256, 16 })));
	ASSERT_EQ(6u, plan.ShapedClusters().size());
	for( const auto& cluster : plan.ShapedClusters() ) {
		EXPECT_GT(cluster.rect.right - cluster.rect.left, 0);
		EXPECT_EQ(0, (cluster.rect.right - cluster.rect.left) % 8);
	}
	EXPECT_EQ(16, plan.ShapedClusters()[0].rect.right - plan.ShapedClusters()[0].rect.left);
	EXPECT_EQ(8, plan.ShapedClusters()[1].rect.right - plan.ShapedClusters()[1].rect.left);
	EXPECT_EQ(16, plan.ShapedClusters()[2].rect.right - plan.ShapedClusters()[2].rect.left);
	EXPECT_EQ(16, plan.ShapedClusters()[3].rect.right - plan.ShapedClusters()[3].rect.left);
	EXPECT_EQ(16, plan.ShapedClusters()[4].rect.right - plan.ShapedClusters()[4].rect.left);
	EXPECT_EQ(16, plan.ShapedClusters()[5].rect.right - plan.ShapedClusters()[5].rect.left);
}

TEST(TerminalRenderPlan, WarmCapacityStopsGrowing)
{
	TerminalModel model(64, 4);
	for( std::size_t row = 0; row < 4; ++row ) Print(model, U"terminal startup frame");
	RecordingClassifier classifier;
	TerminalRenderPlan plan;
	ASSERT_TRUE(plan.Build(Input(model, classifier, 4, { 0, 0, 512, 64 })));
	const auto textCapacity = plan.TextCapacity();
	const auto advanceCapacity = plan.AdvanceCapacity();
	const auto commandCapacity = plan.CommandCapacity();
	ASSERT_TRUE(plan.Build(Input(model, classifier, 4, { 0, 0, 512, 64 })));
	EXPECT_EQ(textCapacity, plan.TextCapacity());
	EXPECT_EQ(advanceCapacity, plan.AdvanceCapacity());
	EXPECT_EQ(commandCapacity, plan.CommandCapacity());
	EXPECT_EQ(0u, plan.Counters().capacityGrowths);
}

TEST(TerminalRenderPlan, CachesOnlyImmediatelyPreviousStyleWithinBuild)
{
	RecordingClassifier classifier;
	CountingStyleResolverContext context;
	TerminalRenderPlan plan;

	// Repeated default attributes resolve once within a Build, but the cache is
	// deliberately not retained by the plan for the next frame.
	TerminalModel repeated(3, 1);
	Print(repeated, U"ABC");
	auto repeatedInput = Input(repeated, classifier, 1, { 0, 0, 24, 16 });
	repeatedInput.styleResolver = &ResolveCountingStyle;
	repeatedInput.styleResolverContext = &context;
	ASSERT_TRUE(plan.Build(repeatedInput));
	EXPECT_EQ(1u, context.calls);
	ASSERT_EQ(1u, plan.GdiRuns().size());
	EXPECT_EQ(CountingStyle({}, false), plan.GdiRuns()[0].style);

	ASSERT_TRUE(plan.Build(repeatedInput));
	EXPECT_EQ(2u, context.calls);

	// A -> B -> A must resolve all three transitions: only the immediately
	// preceding key is eligible for reuse, never a non-adjacent key.
	const TerminalAttributes attributesA{ TerminalColor::Rgb(0x11, 0x22, 0x33), {}, true, false, false };
	const TerminalAttributes attributesB{ TerminalColor::Rgb(0xAA, 0xBB, 0xCC), {}, false, true, false };
	TerminalModel transitions(3, 1);
	transitions.SetForeground(attributesA.foreground);
	transitions.SetBold(attributesA.bold);
	transitions.Print(U'A');
	transitions.SetForeground(attributesB.foreground);
	transitions.SetBold(attributesB.bold);
	transitions.SetUnderline(attributesB.underline);
	transitions.Print(U'B');
	transitions.SetForeground(attributesA.foreground);
	transitions.SetBold(attributesA.bold);
	transitions.SetUnderline(attributesA.underline);
	transitions.Print(U'C');
	context.calls = 0;
	TerminalRenderPlan transitionPlan;
	auto transitionInput = Input(transitions, classifier, 1, { 0, 0, 24, 16 });
	transitionInput.styleResolver = &ResolveCountingStyle;
	transitionInput.styleResolverContext = &context;
	ASSERT_TRUE(transitionPlan.Build(transitionInput));
	EXPECT_EQ(3u, context.calls);
	ASSERT_EQ(3u, transitionPlan.GdiRuns().size());
	const std::array expectedTransitionStyles{
		CountingStyle(attributesA, false), CountingStyle(attributesB, false), CountingStyle(attributesA, false),
	};
	for( std::size_t i = 0; i < expectedTransitionStyles.size(); ++i )
		EXPECT_EQ(expectedTransitionStyles[i], transitionPlan.GdiRuns()[i].style) << i;

	// Selection is part of the cache key.  False -> true -> false therefore
	// resolves three times even though the attributes stay identical.
	TerminalModel selection(3, 1);
	Print(selection, U"XYZ");
	context.calls = 0;
	TerminalRenderPlan selectionPlan;
	auto selectionInput = Input(selection, classifier, 1, { 0, 0, 24, 16 });
	selectionInput.styleResolver = &ResolveCountingStyle;
	selectionInput.styleResolverContext = &context;
	selectionInput.hasSelection = true;
	selectionInput.selectionAnchor = { 0, 1 };
	selectionInput.selectionActive = { 0, 2 };
	ASSERT_TRUE(selectionPlan.Build(selectionInput));
	EXPECT_EQ(3u, context.calls);
	ASSERT_EQ(3u, selectionPlan.GdiRuns().size());
	const std::array expectedSelectionStyles{
		CountingStyle({}, false), CountingStyle({}, true), CountingStyle({}, false),
	};
	for( std::size_t i = 0; i < expectedSelectionStyles.size(); ++i )
		EXPECT_EQ(expectedSelectionStyles[i], selectionPlan.GdiRuns()[i].style) << i;
}

TEST(TerminalRenderPlan, RejectsMalformedAndOversizedInputsBoundedly)
{
	RecordingClassifier classifier;
	TerminalRenderPlan plan;
	TerminalRenderPlanBuildInput malformed{};
	EXPECT_FALSE(plan.Build(malformed));

	TerminalModel oversized(TerminalRenderPlan::kMaximumVisibleCells + 1, 1, 0);
	const auto input = Input(oversized, classifier, 1, { 0, 0, 8, 16 });
	EXPECT_FALSE(plan.Build(input));
	EXPECT_EQ(std::numeric_limits<std::size_t>::max(), plan.Counters().rejectedViewportCells);
	EXPECT_EQ(0u, plan.GdiRuns().size());
	EXPECT_EQ(0u, plan.ShapedClusters().size());
}

TEST(TerminalRenderPlan, UsesAbsoluteCellGeometryForNonzeroPaintOrigin)
{
	TerminalModel model(8, 2);
	model.Print(U'\u2500');
	model.Print(U'A');
	RecordingClassifier classifier;
	TerminalRenderPlan plan;
	ASSERT_TRUE(plan.Build(Input(model, classifier, 2, { 1, 1, 37, 39 }, 8, 16)));
	ASSERT_EQ(1u, plan.BuiltinGlyphs().size());
	EXPECT_EQ(0, plan.BuiltinGlyphs()[0].rect.left);
	EXPECT_EQ(0, plan.BuiltinGlyphs()[0].rect.top);
	EXPECT_EQ(8, plan.BuiltinGlyphs()[0].rect.right);
	EXPECT_EQ(16, plan.BuiltinGlyphs()[0].rect.bottom);
	EXPECT_EQ(8, plan.GdiRuns()[0].rect.left);
}

} // namespace
} // namespace terminal
