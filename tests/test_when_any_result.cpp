#include <gtest/gtest.h>

#include <alt/detail/when_any_result.hpp>
#include <alt/task.hpp>
#include <concepts>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using alt::when_any_range_result;
using alt::when_any_result;
using alt::when_any_value_t;

// What each position of a when_any contributes: the same mapping when_all uses. The
// const case is stated because a plain remove_cvref would silently drop it.
static_assert(std::same_as<when_any_value_t<alt::task<void>>, std::monostate>);
static_assert(std::same_as<when_any_value_t<alt::task<int>>, int>);
static_assert(std::same_as<when_any_value_t<alt::task<int&>>, std::reference_wrapper<int>>);
static_assert(std::same_as<when_any_value_t<alt::task<const int&>>, std::reference_wrapper<const int>>);

namespace
{

	/**
	 * Whether this build still has the asserts the death test below observes.
	 *
	 * The accessor's check is an assert, so NDEBUG compiles it away and the call it
	 * expects to die returns normally. That is a property of the build, not a defect,
	 * so the test skips and says so.
	 */
#if defined(NDEBUG)
	constexpr bool asserts_are_compiled_in = false;
#else
	constexpr bool asserts_are_compiled_in = true;
#endif

	/**
	 * Whether ThreadSanitizer is instrumenting this build. Nested because GCC before 14
	 * has no __has_feature, where a single condition would fail to preprocess.
	 */
#if defined(__SANITIZE_THREAD__)
	constexpr bool thread_sanitizer_is_active = true;
#elif defined(__has_feature)
#	if __has_feature(thread_sanitizer)
	constexpr bool thread_sanitizer_is_active = true;
#	else
	constexpr bool thread_sanitizer_is_active = false;
#	endif
#else
	constexpr bool thread_sanitizer_is_active = false;
#endif

	/** Why the death test is skipped in a build whose asserts are gone. */
	constexpr std::string_view skipped_without_asserts =
	  "the position check is an assert, and NDEBUG compiles it away, so this build has no guard "
	  "for a death test to observe.";

	/** Why the death test is skipped under ThreadSanitizer. */
	constexpr std::string_view skipped_under_thread_sanitizer =
	  "a GoogleTest death test forks, and ThreadSanitizer does not support fork, so the child would "
	  "die of the tool rather than of the assert. The debug and AddressSanitizer presets run it.";

	/**
	 * Sets GoogleTest's death test style for one scope and puts back what was there, so
	 * the process-global flag does not leak to later tests. See the same helper in
	 * tests/test_waiter_list.cpp, which says why each copy is deliberate.
	 */
	class scoped_death_test_style
	{
	public:
		explicit scoped_death_test_style(const std::string& style): m_previous(GTEST_FLAG_GET(death_test_style))
		{
			GTEST_FLAG_SET(death_test_style, style);
		}

		~scoped_death_test_style()
		{
			GTEST_FLAG_SET(death_test_style, m_previous);
		}

		scoped_death_test_style(const scoped_death_test_style&)            = delete;
		scoped_death_test_style& operator=(const scoped_death_test_style&) = delete;
		scoped_death_test_style(scoped_death_test_style&&)                 = delete;
		scoped_death_test_style& operator=(scoped_death_test_style&&)      = delete;

	private:
		/** What the flag held before this object changed it. */
		std::string m_previous;
	};

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
	EXPECT_EQ(result.get<1>(), std::monostate{});
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): EXPECT_DEATH's own expansion is what measures over the threshold, not this body, and clang-tidy-19 counts it only when some `if` precedes it in the same function. Measured with the pinned tool at threshold 0: the macro alone in a test body is 0, the macro alone in a named helper is 37, and one skip guard ahead of it is 38 -- so hoisting the macro into a helper moves the finding rather than removing it, and the guard cannot move because GTEST_SKIP only skips the function it is written in. What this body actually contains is two skip guards and one expectation.
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
