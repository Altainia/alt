#include <gtest/gtest.h>

#include <alt/algorithm.hpp>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct employee
{
	std::string name;
	int         department_id{};
};

// ---------------------------------------------------------------------------
// erase — identity projection (no projection)
// ---------------------------------------------------------------------------

TEST(Erase, RemovesAllMatchingIntegers)
{
	std::vector<int> v{1, 2, 3, 2, 4, 2};
	const auto       removed = alt::erase(v, 2);
	EXPECT_EQ(removed, 3u);
	EXPECT_EQ(v, (std::vector<int>{1, 3, 4}));
}

TEST(Erase, ReturnsZeroWhenNoMatch)
{
	std::vector<int> v{1, 3, 5};
	const auto       removed = alt::erase(v, 9);
	EXPECT_EQ(removed, 0u);
	EXPECT_EQ(v, (std::vector<int>{1, 3, 5}));
}

TEST(Erase, RemovesAllElementsWhenAllMatch)
{
	std::vector<int> v{7, 7, 7};
	const auto       removed = alt::erase(v, 7);
	EXPECT_EQ(removed, 3u);
	EXPECT_TRUE(v.empty());
}

TEST(Erase, EmptyContainerReturnsZero)
{
	std::vector<int> v;
	const auto       removed = alt::erase(v, 42);
	EXPECT_EQ(removed, 0u);
	EXPECT_TRUE(v.empty());
}

TEST(Erase, RemovesSingleElement)
{
	std::vector<int> v{10};
	const auto       removed = alt::erase(v, 10);
	EXPECT_EQ(removed, 1u);
	EXPECT_TRUE(v.empty());
}

TEST(Erase, PreservesRelativeOrderOfSurvivors)
{
	std::vector<int> v{5, 1, 5, 2, 5, 3};
	alt::erase(v, 5);
	EXPECT_EQ(v, (std::vector<int>{1, 2, 3}));
}

TEST(Erase, WorksWithStrings)
{
	std::vector<std::string> v{"a", "b", "a", "c"};
	const auto               removed = alt::erase(v, std::string{"a"});
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(v, (std::vector<std::string>{"b", "c"}));
}

TEST(Erase, WorksWithDeque)
{
	std::deque<int> d{1, 2, 3, 2, 1};
	const auto      removed = alt::erase(d, 2);
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(d, (std::deque<int>{1, 3, 1}));
}

// ---------------------------------------------------------------------------
// erase — member-pointer projection
// ---------------------------------------------------------------------------

TEST(EraseProjection, MemberPointerRemovesMatchingDepartment)
{
	std::vector<employee> employees{
	  {"Alice", 1},
	  {"Bob", 2},
	  {"Carol", 1},
	  {"Dave", 3},
	};
	const auto removed = alt::erase(employees, 1, &employee::department_id);
	EXPECT_EQ(removed, 2u);
	ASSERT_EQ(employees.size(), 2u);
	EXPECT_EQ(employees[0].name, "Bob");
	EXPECT_EQ(employees[1].name, "Dave");
}

TEST(EraseProjection, MemberPointerNoMatchLeavesContainerUnchanged)
{
	std::vector<employee> employees{{"Alice", 1}, {"Bob", 2}};
	const auto            removed = alt::erase(employees, 99, &employee::department_id);
	EXPECT_EQ(removed, 0u);
	EXPECT_EQ(employees.size(), 2u);
}

TEST(EraseProjection, MemberPointerRemovesAllWhenAllMatch)
{
	std::vector<employee> employees{{"Alice", 5}, {"Bob", 5}};
	const auto            removed = alt::erase(employees, 5, &employee::department_id);
	EXPECT_EQ(removed, 2u);
	EXPECT_TRUE(employees.empty());
}

// ---------------------------------------------------------------------------
// erase — lambda projection
// ---------------------------------------------------------------------------

TEST(EraseProjection, LambdaProjectionOnStringLength)
{
	std::vector<std::string> words{"hi", "hello", "by", "world"};
	const auto               removed = alt::erase(words, 2u, [](const std::string& s) { return s.size(); });
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(words, (std::vector<std::string>{"hello", "world"}));
}

TEST(EraseProjection, LambdaProjectionAbsoluteValue)
{
	std::vector<int> v{-3, 1, -3, 2, 3};
	const auto       removed = alt::erase(v, 3, [](int x) { return x < 0 ? -x : x; });
	EXPECT_EQ(removed, 3u);
	EXPECT_EQ(v, (std::vector<int>{1, 2}));
}

TEST(EraseProjection, LambdaProjectionEmptyContainerReturnsZero)
{
	std::vector<employee> employees;
	const auto            removed =
	  alt::erase(employees, std::string{"Alice"}, [](const employee& e) { return e.name; });
	EXPECT_EQ(removed, 0u);
}

// ---------------------------------------------------------------------------
// erase — return type
// ---------------------------------------------------------------------------

TEST(EraseReturnType, ReturnTypeIsSizeType)
{
	std::vector<int> v{1, 2, 3};
	using expected_t = std::vector<int>::size_type;
	static_assert(
	  std::is_same_v<decltype(alt::erase(v, 1)), expected_t>,
	  "erase must return Container::size_type");
}
