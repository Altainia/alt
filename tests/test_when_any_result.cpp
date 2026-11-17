#include <gtest/gtest.h>

#include <alt/detail/when_any_result.hpp>
#include <alt/task.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "death_test_support.hpp"

using alt::when_any_range_result;
using alt::when_any_result;
using alt::detail::when_any_value_t;

// What each position of a when_any contributes: the same mapping when_all uses. The
// const case is stated because a plain remove_cvref would silently drop it.
static_assert(std::same_as<when_any_value_t<alt::task<void>>, std::monostate>);
static_assert(std::same_as<when_any_value_t<alt::task<int>>, int>);
static_assert(std::same_as<when_any_value_t<alt::task<int&>>, std::reference_wrapper<int>>);
static_assert(std::same_as<when_any_value_t<alt::task<const int&>>, std::reference_wrapper<const int>>);

namespace
{

	// The build-detection constants and the death test style holder are shared from
	// tests/death_test_support.hpp, which records why they live there. The skip messages
	// below stay local, each naming this file's own assert.
	using alt_test::asserts_are_compiled_in;
	using alt_test::scoped_death_test_style;
	using alt_test::thread_sanitizer_is_active;

	/** Why the death test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "the position check is an assert, and NDEBUG compiles it away, so this build has no guard "
	  "for a death test to observe.";

	/** Why the death test is skipped under ThreadSanitizer. */
	constexpr std::string_view skipped_under_thread_sanitizer =
	  "a GoogleTest death test forks, and ThreadSanitizer does not support fork, so the child would "
	  "die of the tool rather than of the assert. The debug and AddressSanitizer presets run it.";

} // namespace

TEST(WhenAnyResultTest, IndexAndValueComeBackForTheWinningPosition)
{
	const when_any_result<int, std::string, double> first{std::in_place_index<0>, 7};
	EXPECT_EQ(first.index(), 0U);
	EXPECT_EQ(first.get<0>(), 7);

	const when_any_result<int, std::string, double> second{std::in_place_index<1>, "won"};
	EXPECT_EQ(second.index(), 1U);
	EXPECT_EQ(second.get<1>(), "won");

	const when_any_result<int, std::string, double> third{std::in_place_index<2>, 2.5};
	EXPECT_EQ(third.index(), 2U);
	EXPECT_EQ(third.get<2>(), 2.5);
}

TEST(WhenAnyResultTest, DuplicateValueTypesAreToldApartByPosition)
{
	const when_any_result<int, int> second{std::in_place_index<1>, 42};
	EXPECT_EQ(second.index(), 1U);
	EXPECT_EQ(second.get<1>(), 42);

	const when_any_result<int, int> first{std::in_place_index<0>, 42};
	EXPECT_EQ(first.index(), 0U);
}

TEST(WhenAnyResultTest, AVoidChildsValueIsMonostate)
{
	const when_any_result<int, std::monostate> result{std::in_place_index<1>};
	EXPECT_EQ(result.index(), 1U);
	static_assert(std::same_as<decltype(result.get<1>()), const std::monostate&>);
}

TEST(WhenAnyResultTest, AReferenceChildsValueIsAReferenceWrapper)
{
	int                                                             target = 5;
	const when_any_result<std::reference_wrapper<int>, std::string> result{std::in_place_index<0>, std::ref(target)};
	EXPECT_EQ(result.index(), 0U);

	// The wrapper refers to the caller's object, not a copy of it.
	result.get<0>().get() = 9;
	EXPECT_EQ(target, 9);
	EXPECT_EQ(&result.get<0>().get(), &target);
}

// The overload set: a const lvalue reads, an rvalue takes. The rvalue overload yields an
// rvalue reference, the const one a const lvalue reference.
static_assert(std::same_as<decltype(std::declval<const when_any_result<int, int>&>().get<0>()), const int&>);
static_assert(std::same_as<decltype(std::declval<when_any_result<int, int>>().get<0>()), int&&>);
static_assert(std::same_as<decltype(std::declval<const when_any_range_result<int>&>().value()), const int&>);
static_assert(std::same_as<decltype(std::declval<when_any_range_result<int>>().value()), int&&>);

TEST(WhenAnyResultTest, AMoveOnlyWinnerCanBeTakenOutOfAnRvalueResult)
{
	when_any_result<int, std::unique_ptr<int>> result{std::in_place_index<1>, std::make_unique<int>(8)};

	// Reading does not take.
	ASSERT_NE(result.get<1>(), nullptr);
	EXPECT_EQ(*result.get<1>(), 8);

	const std::unique_ptr<int> taken = std::move(result).get<1>();
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 8);

	// The source is left moved-from: its alternative is still engaged, now null.
	// NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what this test observes.
	EXPECT_EQ(result.index(), 1U);
	// NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what this test observes.
	EXPECT_EQ(result.get<1>(), nullptr);
}

TEST(WhenAnyResultTest, AMoveOnlyRangeWinnerCanBeTakenOutOfAnRvalueResult)
{
	when_any_range_result<std::unique_ptr<int>> result{2, std::make_unique<int>(4)};
	ASSERT_NE(result.value(), nullptr);

	const std::unique_ptr<int> taken = std::move(result).value();
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 4);
	// NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what this test observes.
	EXPECT_EQ(result.value(), nullptr);
	// NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what this test observes.
	EXPECT_EQ(result.index(), 2U);
}

TEST(WhenAnyResultTest, TheRangeFormCarriesOneValueTypeAndAnIndex)
{
	const when_any_range_result<std::string> result{3, "third"};
	EXPECT_EQ(result.index(), 3U);
	EXPECT_EQ(result.value(), "third");

	// Position 0 is just another winner, not a sentinel.
	const when_any_range_result<int> zero{0, 11};
	EXPECT_EQ(zero.index(), 0U);
	EXPECT_EQ(zero.value(), 11);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): same cause and same shape as the death tests in tests/test_waiter_list.cpp, whose suppression carries the measurement (clang-tidy-19 scores EXPECT_DEATH 0 alone in a body but 37-39 once a skip guard precedes it, against a threshold of 25). Not re-measured for this body, which has the same two skip guards and one expectation.
TEST(WhenAnyResultDeathTest, GettingAPositionThatDidNotWinAborts)
{
	if(!asserts_are_compiled_in)
	{
		GTEST_SKIP() << skipped_without_asserts;
	}

	if(thread_sanitizer_is_active)
	{
		GTEST_SKIP() << skipped_under_thread_sanitizer;
	}

	const scoped_death_test_style style{"threadsafe"};

	const when_any_result<int, int> result{std::in_place_index<0>, 1};

	EXPECT_DEATH(static_cast<void>(result.get<1>()), "reading an alt when_any result for a position that did not win");
}
