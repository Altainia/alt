#include <gtest/gtest.h>

#include <algorithm>
#include <alt/concepts.hpp>
#include <alt/functional.hpp>
#include <ranges>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers shared across tests
// ---------------------------------------------------------------------------

static constexpr auto positive = [](int x) { return x > 0; };
static constexpr auto negate_b = [](bool b) { return !b; };

// ---------------------------------------------------------------------------
// bool_condition concept
// ---------------------------------------------------------------------------

TEST(BoolCondition, SatisfiedByDirectlyConvertibleTypes)
{
	static_assert(alt::bool_condition<bool>);
	static_assert(alt::bool_condition<int>);
	static_assert(alt::bool_condition<float>);
	static_assert(alt::bool_condition<void*>);
}

TEST(BoolCondition, SatisfiedByNullaryCallableReturningBool)
{
	static_assert(alt::bool_condition<decltype([] { return true; })>);
	static_assert(alt::bool_condition<bool (*)()>);
}

TEST(BoolCondition, NotSatisfiedByUnaryCallable)
{
	static_assert(!alt::bool_condition<decltype(positive)>);
	static_assert(!alt::bool_condition<decltype(negate_b)>);
}

TEST(BoolCondition, NotSatisfiedByNonBoolNonCallable)
{
	struct opaque
	{
	};
	static_assert(!alt::bool_condition<opaque>);
}

// ---------------------------------------------------------------------------
// any_of — overload 1 (conditions)
// ---------------------------------------------------------------------------

TEST(AnyOf, DirectBools)
{
	EXPECT_TRUE(alt::any_of(true));
	EXPECT_TRUE(alt::any_of(true, false));
	EXPECT_TRUE(alt::any_of(false, true));
	EXPECT_TRUE(alt::any_of(false, false, true));
	EXPECT_FALSE(alt::any_of(false));
	EXPECT_FALSE(alt::any_of(false, false));
}

TEST(AnyOf, EmptyPackReturnsFalse)
{
	EXPECT_FALSE(alt::any_of());
}

TEST(AnyOf, NullaryCallablesAreInvoked)
{
	EXPECT_TRUE(alt::any_of([] { return false; }, [] { return true; }));
	EXPECT_FALSE(alt::any_of([] { return false; }, [] { return false; }));
}

TEST(AnyOf, MixedDirectAndCallable)
{
	EXPECT_TRUE(alt::any_of(false, [] { return true; }));
	EXPECT_FALSE(alt::any_of(false, [] { return false; }));
}

TEST(AnyOf, ShortCircuitsOnFirstTrue)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_TRUE(alt::any_of(true, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// any_of — overload 2 (callable + args)
// ---------------------------------------------------------------------------

TEST(AnyOfCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::any_of(positive, -1, 1));
	EXPECT_TRUE(alt::any_of(positive, 1, -1));
	EXPECT_FALSE(alt::any_of(positive, -1, -2));
}

TEST(AnyOfCallable, EmptyArgListReturnsFalse)
{
	EXPECT_FALSE(alt::any_of(positive));
}

TEST(AnyOfCallable, UnwrapsNullaryArgBeforeTransforming)
{
	// positive(make_pos()) and positive(make_neg())
	auto make_pos = [] { return 5; };
	auto make_neg = [] { return -1; };
	EXPECT_TRUE(alt::any_of(positive, make_neg, make_pos));
	EXPECT_FALSE(alt::any_of(positive, make_neg, make_neg));
}

TEST(AnyOfCallable, ShortCircuitsOnFirstTrue)
{
	int  calls   = 0;
	auto counted = [&](int x) { ++calls; return x > 0; };
	EXPECT_TRUE(alt::any_of(counted, 1, -1, -2));
	EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// all_of — overload 1
// ---------------------------------------------------------------------------

TEST(AllOf, DirectBools)
{
	EXPECT_TRUE(alt::all_of(true));
	EXPECT_TRUE(alt::all_of(true, true));
	EXPECT_FALSE(alt::all_of(true, false));
	EXPECT_FALSE(alt::all_of(false, true));
	EXPECT_FALSE(alt::all_of(false, false));
}

TEST(AllOf, EmptyPackReturnsTrue)
{
	EXPECT_TRUE(alt::all_of());
}

TEST(AllOf, ShortCircuitsOnFirstFalse)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_FALSE(alt::all_of(false, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// all_of — overload 2
// ---------------------------------------------------------------------------

TEST(AllOfCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::all_of(positive, 1, 2, 3));
	EXPECT_FALSE(alt::all_of(positive, 1, -1, 2));
	EXPECT_FALSE(alt::all_of(positive, -1, -2));
}

TEST(AllOfCallable, EmptyArgListReturnsTrue)
{
	EXPECT_TRUE(alt::all_of(positive));
}

TEST(AllOfCallable, UnwrapsNullaryArgBeforeTransforming)
{
	auto make_pos = [] { return 5; };
	auto make_neg = [] { return -1; };
	EXPECT_TRUE(alt::all_of(positive, make_pos, make_pos));
	EXPECT_FALSE(alt::all_of(positive, make_pos, make_neg));
}

// ---------------------------------------------------------------------------
// not_all_of — overload 1
// ---------------------------------------------------------------------------

TEST(NotAllOf, DirectBools)
{
	EXPECT_FALSE(alt::not_all_of(true, true));
	EXPECT_TRUE(alt::not_all_of(true, false));
	EXPECT_TRUE(alt::not_all_of(false, true));
	EXPECT_TRUE(alt::not_all_of(false, false));
}

TEST(NotAllOf, EmptyPackReturnsFalse)
{
	EXPECT_FALSE(alt::not_all_of()); // !vacuously-all-true
}

TEST(NotAllOf, ShortCircuitsOnFirstFalse)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_TRUE(alt::not_all_of(false, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// not_all_of — overload 2
// ---------------------------------------------------------------------------

TEST(NotAllOfCallable, TransformsDirectArgs)
{
	EXPECT_FALSE(alt::not_all_of(positive, 1, 2));
	EXPECT_TRUE(alt::not_all_of(positive, 1, -1));
	EXPECT_TRUE(alt::not_all_of(positive, -1, -2));
}

// ---------------------------------------------------------------------------
// none_of — overload 1
// ---------------------------------------------------------------------------

TEST(NoneOf, DirectBools)
{
	EXPECT_TRUE(alt::none_of(false, false));
	EXPECT_FALSE(alt::none_of(false, true));
	EXPECT_FALSE(alt::none_of(true, false));
	EXPECT_FALSE(alt::none_of(true, true));
}

TEST(NoneOf, EmptyPackReturnsTrue)
{
	EXPECT_TRUE(alt::none_of());
}

TEST(NoneOf, ShortCircuitsOnFirstTrue)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_FALSE(alt::none_of(true, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// none_of — overload 2
// ---------------------------------------------------------------------------

TEST(NoneOfCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::none_of(positive, -1, -2));
	EXPECT_FALSE(alt::none_of(positive, -1, 1));
	EXPECT_FALSE(alt::none_of(positive, 1, 2));
}

// ---------------------------------------------------------------------------
// at_least<N> — overload 1
// ---------------------------------------------------------------------------

TEST(AtLeast, ZeroAlwaysTrue)
{
	EXPECT_TRUE(alt::at_least<0>());
	EXPECT_TRUE(alt::at_least<0>(false, false, false));
	EXPECT_TRUE(alt::at_least<0>(true));
}

TEST(AtLeast, ZeroDoesNotEvaluateAnyCondition)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return false; };
	EXPECT_TRUE(alt::at_least<0>(counted, counted, counted));
	EXPECT_EQ(calls, 0);
}

TEST(AtLeast, Basic)
{
	EXPECT_TRUE(alt::at_least<1>(false, true));
	EXPECT_FALSE(alt::at_least<1>(false, false));
	EXPECT_TRUE(alt::at_least<2>(true, true, false));
	EXPECT_FALSE(alt::at_least<2>(true, false, false));
	EXPECT_TRUE(alt::at_least<3>(true, true, true));
	EXPECT_FALSE(alt::at_least<4>(true, true, true)); // only 3 available
}

TEST(AtLeast, EmptyPackWithNonZeroReturnsFalse)
{
	EXPECT_FALSE(alt::at_least<1>());
}

TEST(AtLeast, ShortCircuitsOnceSatisfied)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_TRUE(alt::at_least<1>(true, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// at_least<N> — overload 2
// ---------------------------------------------------------------------------

TEST(AtLeastCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::at_least<2>(positive, 1, 2, -1));
	EXPECT_FALSE(alt::at_least<2>(positive, 1, -1, -2));
}

TEST(AtLeastCallable, ZeroAlwaysTrue)
{
	EXPECT_TRUE(alt::at_least<0>(positive, -1, -2, -3));
}

// ---------------------------------------------------------------------------
// at_most<N> — overload 1
// ---------------------------------------------------------------------------

TEST(AtMost, Basic)
{
	EXPECT_TRUE(alt::at_most<2>(true, true, false));
	EXPECT_TRUE(alt::at_most<2>(true, false, false));
	EXPECT_FALSE(alt::at_most<2>(true, true, true));
	EXPECT_TRUE(alt::at_most<0>(false, false));
	EXPECT_FALSE(alt::at_most<0>(true));
}

TEST(AtMost, EmptyPackAlwaysTrue)
{
	EXPECT_TRUE(alt::at_most<0>());
	EXPECT_TRUE(alt::at_most<5>());
}

TEST(AtMost, ShortCircuitsOnceExceeded)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_FALSE(alt::at_most<0>(true, counted));
	EXPECT_EQ(calls, 0);
}

// ---------------------------------------------------------------------------
// at_most<N> — overload 2
// ---------------------------------------------------------------------------

TEST(AtMostCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::at_most<1>(positive, 1, -1, -2));
	EXPECT_FALSE(alt::at_most<1>(positive, 1, 2, -1));
}

// ---------------------------------------------------------------------------
// exactly<N> — overload 1
// ---------------------------------------------------------------------------

TEST(Exactly, Basic)
{
	EXPECT_TRUE(alt::exactly<1>(true));
	EXPECT_FALSE(alt::exactly<1>(false));
	EXPECT_TRUE(alt::exactly<2>(true, true, false));
	EXPECT_FALSE(alt::exactly<2>(true, true, true));
	EXPECT_FALSE(alt::exactly<2>(true, false, false));
	EXPECT_TRUE(alt::exactly<0>(false, false));
	EXPECT_FALSE(alt::exactly<0>(true));
}

TEST(Exactly, EmptyPackOnlyTrueForZero)
{
	EXPECT_TRUE(alt::exactly<0>());
	EXPECT_FALSE(alt::exactly<1>());
	EXPECT_FALSE(alt::exactly<2>());
}

TEST(Exactly, ShortCircuitsOnceExceeded)
{
	int  calls   = 0;
	auto counted = [&] { ++calls; return true; };
	EXPECT_FALSE(alt::exactly<1>(true, true, counted));
	EXPECT_EQ(calls, 0);
}

TEST(Exactly, EquivalentToAllOfWhenNMatchesTotal)
{
	EXPECT_EQ(alt::exactly<3>(true, true, true), alt::all_of(true, true, true));
	EXPECT_EQ(alt::exactly<3>(true, false, true), alt::all_of(true, false, true));
}

// ---------------------------------------------------------------------------
// exactly<N> — overload 2
// ---------------------------------------------------------------------------

TEST(ExactlyCallable, TransformsDirectArgs)
{
	EXPECT_TRUE(alt::exactly<2>(positive, 1, 2, -1));
	EXPECT_FALSE(alt::exactly<2>(positive, 1, 2, 3));
	EXPECT_FALSE(alt::exactly<2>(positive, 1, -1, -2));
}

// ---------------------------------------------------------------------------
// Disambiguation: nullary callable with bool_condition → always overload 1
// ---------------------------------------------------------------------------

TEST(Disambiguation, NullaryCallableTreatedAsConditionNotTransformer)
{
	// cond is a bool_condition (nullary, returns bool). When passed alongside
	// another argument, it goes to overload 1 (both are conditions), not
	// overload 2 (cond as transformer applied to the second arg).
	int  calls = 0;
	auto cond  = [&] { ++calls; return true; };
	auto other = [&] { ++calls; return false; };
	EXPECT_TRUE(alt::any_of(cond, other)); // short-circuits after cond()
	EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// Constexpr correctness
// ---------------------------------------------------------------------------

TEST(Constexpr, StaticAsserts)
{
	static_assert(alt::any_of(true, false));
	static_assert(!alt::any_of(false, false));
	static_assert(!alt::any_of());

	static_assert(alt::all_of(true, true));
	static_assert(!alt::all_of(true, false));
	static_assert(alt::all_of());

	static_assert(alt::not_all_of(true, false));
	static_assert(!alt::not_all_of(true, true));
	static_assert(!alt::not_all_of());

	static_assert(alt::none_of(false, false));
	static_assert(!alt::none_of(false, true));
	static_assert(alt::none_of());

	static_assert(alt::at_least<0>());
	static_assert(alt::at_least<2>(true, true, false));
	static_assert(!alt::at_least<2>(true, false));
	static_assert(!alt::at_least<1>());

	static_assert(alt::at_most<1>(true, false));
	static_assert(!alt::at_most<1>(true, true));
	static_assert(alt::at_most<0>());

	static_assert(alt::exactly<1>(true, false));
	static_assert(!alt::exactly<1>(true, true));
	static_assert(alt::exactly<0>());
	static_assert(!alt::exactly<1>());
}

TEST(Constexpr, CallableOverloadStaticAsserts)
{
	static_assert(alt::any_of(positive, -1, 1));
	static_assert(!alt::any_of(positive, -1, -2));
	static_assert(alt::all_of(positive, 1, 2, 3));
	static_assert(!alt::all_of(positive, 1, -1));
	static_assert(alt::none_of(positive, -1, -2));
	static_assert(!alt::none_of(positive, -1, 1));
	static_assert(alt::at_least<2>(positive, 1, 2, -1));
	static_assert(!alt::at_least<2>(positive, 1, -1));
	static_assert(alt::at_most<1>(positive, 1, -1, -2));
	static_assert(!alt::at_most<1>(positive, 1, 2));
	static_assert(alt::exactly<2>(positive, 1, 2, -1));
	static_assert(!alt::exactly<2>(positive, 1, 2, 3));
}

// ---------------------------------------------------------------------------
// equals
// ---------------------------------------------------------------------------

TEST(Equals, BasicInt)
{
	EXPECT_TRUE(alt::equals{2}(2));
	EXPECT_FALSE(alt::equals{2}(3));
	EXPECT_FALSE(alt::equals{2}(-2));
}

TEST(Equals, CrossTypeIntAndShort)
{
	short const s2{2};
	short const s3{3};
	EXPECT_TRUE(alt::equals{2}(s2));
	EXPECT_FALSE(alt::equals{2}(s3));
}

TEST(Equals, StringComparison)
{
	std::string const hello{"hello"};
	EXPECT_TRUE(alt::equals{std::string{"hello"}}(hello));
	EXPECT_FALSE(alt::equals{std::string{"world"}}(hello));
}

TEST(Equals, RangesFilter)
{
	std::vector<int> const v{1, 2, 3, 2, 1};
	auto const             result = v | std::views::filter(alt::equals{2}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{2, 2}));
}

TEST(Equals, Constexpr)
{
	static_assert(alt::equals{5}(5));
	static_assert(!alt::equals{5}(4));
}

// ---------------------------------------------------------------------------
// not_equals
// ---------------------------------------------------------------------------

TEST(NotEquals, BasicInt)
{
	EXPECT_TRUE(alt::not_equals{2}(3));
	EXPECT_TRUE(alt::not_equals{2}(-2));
	EXPECT_FALSE(alt::not_equals{2}(2));
}

TEST(NotEquals, RangesFilter)
{
	std::vector<int> const v{1, 2, 3, 2, 1};
	auto const             result = v | std::views::filter(alt::not_equals{2}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{1, 3, 1}));
}

TEST(NotEquals, Constexpr)
{
	static_assert(alt::not_equals{5}(4));
	static_assert(!alt::not_equals{5}(5));
}

// ---------------------------------------------------------------------------
// less
// ---------------------------------------------------------------------------

TEST(Less, BasicInt)
{
	EXPECT_TRUE(alt::less{5}(3));
	EXPECT_FALSE(alt::less{5}(5));
	EXPECT_FALSE(alt::less{5}(6));
}

TEST(Less, CrossTypeIntAndShort)
{
	EXPECT_TRUE(alt::less{5}(short{3}));
	EXPECT_FALSE(alt::less{5}(short{5}));
}

TEST(Less, RangesFilter)
{
	std::vector<int> const v{1, 3, 5, 7, 9};
	auto const             result = v | std::views::filter(alt::less{5}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{1, 3}));
}

TEST(Less, Constexpr)
{
	static_assert(alt::less{5}(3));
	static_assert(!alt::less{5}(5));
	static_assert(!alt::less{5}(7));
}

// ---------------------------------------------------------------------------
// greater
// ---------------------------------------------------------------------------

TEST(Greater, BasicInt)
{
	EXPECT_TRUE(alt::greater{5}(7));
	EXPECT_FALSE(alt::greater{5}(5));
	EXPECT_FALSE(alt::greater{5}(3));
}

TEST(Greater, RangesFilter)
{
	std::vector<int> const v{1, 3, 5, 7, 9};
	auto const             result = v | std::views::filter(alt::greater{5}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{7, 9}));
}

TEST(Greater, Constexpr)
{
	static_assert(alt::greater{5}(7));
	static_assert(!alt::greater{5}(5));
	static_assert(!alt::greater{5}(3));
}

// ---------------------------------------------------------------------------
// less_equal
// ---------------------------------------------------------------------------

TEST(LessEqual, BasicInt)
{
	EXPECT_TRUE(alt::less_equal{5}(3));
	EXPECT_TRUE(alt::less_equal{5}(5));
	EXPECT_FALSE(alt::less_equal{5}(6));
}

TEST(LessEqual, RangesFilter)
{
	std::vector<int> const v{1, 3, 5, 7, 9};
	auto const             result = v | std::views::filter(alt::less_equal{5}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{1, 3, 5}));
}

TEST(LessEqual, Constexpr)
{
	static_assert(alt::less_equal{5}(3));
	static_assert(alt::less_equal{5}(5));
	static_assert(!alt::less_equal{5}(7));
}

// ---------------------------------------------------------------------------
// greater_equal
// ---------------------------------------------------------------------------

TEST(GreaterEqual, BasicInt)
{
	EXPECT_TRUE(alt::greater_equal{5}(7));
	EXPECT_TRUE(alt::greater_equal{5}(5));
	EXPECT_FALSE(alt::greater_equal{5}(3));
}

TEST(GreaterEqual, RangesFilter)
{
	std::vector<int> const v{1, 3, 5, 7, 9};
	auto const             result = v | std::views::filter(alt::greater_equal{5}) | std::ranges::to<std::vector>();
	EXPECT_EQ(result, (std::vector<int>{5, 7, 9}));
}

TEST(GreaterEqual, Constexpr)
{
	static_assert(alt::greater_equal{5}(7));
	static_assert(alt::greater_equal{5}(5));
	static_assert(!alt::greater_equal{5}(3));
}

// ---------------------------------------------------------------------------
// Composability with existing functional combinators
// ---------------------------------------------------------------------------

TEST(PartialComparison, ComposedWithAnyOf)
{
	EXPECT_TRUE(alt::any_of(alt::equals{2}, 1, 2, 3));
	EXPECT_FALSE(alt::any_of(alt::equals{9}, 1, 2, 3));
}

TEST(PartialComparison, ComposedWithAllOf)
{
	EXPECT_TRUE(alt::all_of(alt::greater{0}, 1, 2, 3));
	EXPECT_FALSE(alt::all_of(alt::greater{0}, 1, -1, 3));
}

TEST(PartialComparison, ComposedWithNoneOf)
{
	EXPECT_TRUE(alt::none_of(alt::equals{9}, 1, 2, 3));
	EXPECT_FALSE(alt::none_of(alt::equals{2}, 1, 2, 3));
}
