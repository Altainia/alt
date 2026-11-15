#include <gtest/gtest.h>

#include <alt/async_event.hpp>
#include <alt/awaitable.hpp>
#include <alt/detail/combinator_results.hpp>
#include <alt/task.hpp>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using alt::detail::assemble_combinator_results;
using alt::detail::make_combinator_slots;
using alt::detail::result_store;
using alt::detail::when_all_slot_t;
using alt::detail::when_all_value_t;

// What each position of a when_all contributes. An rvalue-reference result (what
// awaiting an alt::task rvalue yields) is a value, not a reference.
static_assert(std::same_as<alt::await_result_t<alt::task<int>>, int&&>);
static_assert(std::same_as<when_all_value_t<alt::task<int>>, int>);
static_assert(std::same_as<when_all_value_t<alt::task<void>>, std::monostate>);
static_assert(std::same_as<when_all_value_t<alt::task<int&>>, std::reference_wrapper<int>>);
static_assert(std::same_as<when_all_value_t<alt::task<std::string>>, std::string>);
// alt::async_event yields alt::wait_result, a value type.
static_assert(alt::awaitable<alt::async_event&>);
static_assert(std::same_as<when_all_value_t<alt::async_event&>, alt::wait_result>);
static_assert(std::same_as<when_all_slot_t<alt::task<void>>, result_store<std::monostate>>);
static_assert(std::same_as<when_all_slot_t<alt::task<int>>, result_store<int>>);

namespace
{

	/** Counts how it was constructed, to prove assembly moves rather than copies. */
	struct move_counter
	{
		static inline int copies = 0;
		static inline int moves  = 0;

		move_counter() = default;

		move_counter(const move_counter&)
		{
			++copies;
		}

		move_counter(move_counter&&) noexcept
		{
			++moves;
		}

		move_counter& operator=(const move_counter&) = default;
		move_counter& operator=(move_counter&&)      = default;
		~move_counter()                              = default;
	};

	std::exception_ptr failure(const char* what)
	{
		return std::make_exception_ptr(std::runtime_error{what});
	}

}

TEST(CombinatorResultsTest, MakeSlotsDefaultConstructsATuple)
{
	using slots     = std::tuple<result_store<int>, result_store<std::string>>;
	const auto made = make_combinator_slots<slots>(2);
	EXPECT_FALSE(std::get<0>(made).is_set());
	EXPECT_FALSE(std::get<1>(made).is_set());
}

TEST(CombinatorResultsTest, MakeSlotsSizesAVector)
{
	const auto made = make_combinator_slots<std::vector<result_store<int>>>(4);
	EXPECT_EQ(made.size(), 4U);
	for(const auto& slot: made)
	{
		EXPECT_FALSE(slot.is_set());
	}
}

TEST(CombinatorResultsTest, MakeSlotsBuildsAnEmptyVectorForZero)
{
	const auto made = make_combinator_slots<std::vector<result_store<int>>>(0);
	EXPECT_TRUE(made.empty());
}

TEST(CombinatorResultsTest, AssemblyMovesEveryTupleSlotIntoPosition)
{
	using slots   = std::tuple<result_store<int>, result_store<std::string>, result_store<std::monostate>>;
	using results = std::tuple<int, std::string, std::monostate>;

	auto made = make_combinator_slots<slots>(3);
	std::get<0>(made).set_value(1);
	std::get<1>(made).set_value(std::string{"two"});
	std::get<2>(made).set_value(std::monostate{});

	const results assembled = assemble_combinator_results<results>(std::move(made));
	EXPECT_EQ(std::get<0>(assembled), 1);
	EXPECT_EQ(std::get<1>(assembled), "two");
	EXPECT_EQ(assembled, (results{1, "two", {}}));
}

TEST(CombinatorResultsTest, AssemblyMovesRatherThanCopies)
{
	using slots   = std::tuple<result_store<move_counter>, result_store<move_counter>>;
	using results = std::tuple<move_counter, move_counter>;

	auto made = make_combinator_slots<slots>(2);
	std::get<0>(made).set_value(move_counter{});
	std::get<1>(made).set_value(move_counter{});

	move_counter::copies    = 0;
	move_counter::moves     = 0;
	const results assembled = assemble_combinator_results<results>(std::move(made));
	(void)assembled;
	EXPECT_EQ(move_counter::copies, 0);
	EXPECT_EQ(move_counter::moves, 2);
}

TEST(CombinatorResultsTest, AssemblyRethrowsAStoredException)
{
	using slots   = std::tuple<result_store<int>, result_store<int>>;
	using results = std::tuple<int, int>;

	auto made = make_combinator_slots<slots>(2);
	std::get<0>(made).set_value(1);
	std::get<1>(made).set_exception(failure("child"));

	EXPECT_THROW((void)assemble_combinator_results<results>(std::move(made)), std::runtime_error);

	auto again = make_combinator_slots<slots>(2);
	std::get<0>(again).set_value(1);
	std::get<1>(again).set_exception(failure("child"));
	try
	{
		(void)assemble_combinator_results<results>(std::move(again));
		FAIL() << "expected a throw";
	}
	catch(const std::runtime_error& e)
	{
		EXPECT_STREQ(e.what(), "child");
	}
}

TEST(CombinatorResultsTest, TheLowestIndexedThrowingSlotIsTheOneRethrown)
{
	using slots   = std::tuple<result_store<int>, result_store<int>, result_store<int>>;
	using results = std::tuple<int, int, int>;

	auto made = make_combinator_slots<slots>(3);
	std::get<0>(made).set_value(0);
	std::get<1>(made).set_exception(failure("position one"));
	std::get<2>(made).set_exception(failure("position two"));

	try
	{
		(void)assemble_combinator_results<results>(std::move(made));
		FAIL() << "expected a throw";
	}
	catch(const std::runtime_error& e)
	{
		EXPECT_STREQ(e.what(), "position one");
	}
}

TEST(CombinatorResultsTest, AssemblyFillsAVectorInOrder)
{
	auto made = make_combinator_slots<std::vector<result_store<int>>>(4);
	for(std::size_t i = 0; i < made.size(); ++i)
	{
		made[i].set_value(static_cast<int>(i) * 10);
	}

	const auto assembled = assemble_combinator_results<std::vector<int>>(std::move(made));
	EXPECT_EQ(assembled, (std::vector<int>{0, 10, 20, 30}));
}

TEST(CombinatorResultsTest, AssemblyOfAnEmptyVectorYieldsAnEmptyVector)
{
	auto       made      = make_combinator_slots<std::vector<result_store<int>>>(0);
	const auto assembled = assemble_combinator_results<std::vector<int>>(std::move(made));
	EXPECT_TRUE(assembled.empty());
}

TEST(CombinatorResultsTest, AssemblyOfAVectorRethrowsTheLowestIndexedException)
{
	auto made = make_combinator_slots<std::vector<result_store<int>>>(3);
	made[0].set_value(0);
	made[1].set_exception(failure("position one"));
	made[2].set_exception(failure("position two"));

	try
	{
		(void)assemble_combinator_results<std::vector<int>>(std::move(made));
		FAIL() << "expected a throw";
	}
	catch(const std::runtime_error& e)
	{
		EXPECT_STREQ(e.what(), "position one");
	}
}

TEST(CombinatorResultsTest, AReferenceSlotAssemblesToAWrapperAroundTheSameObject)
{
	using slots   = std::tuple<result_store<std::reference_wrapper<int>>>;
	using results = std::tuple<std::reference_wrapper<int>>;

	int  x    = 7;
	auto made = make_combinator_slots<slots>(1);
	std::get<0>(made).set_value(std::ref(x));

	const results assembled = assemble_combinator_results<results>(std::move(made));
	EXPECT_EQ(&std::get<0>(assembled).get(), &x);
}
