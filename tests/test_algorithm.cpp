#include <gtest/gtest.h>

#include <alt/algorithm.hpp>
#include <alt/projection.hpp>
#include <cstddef>
#include <deque>
#include <forward_list>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
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
	  {.name = "Alice", .department_id = 1},
	  {.name = "Bob", .department_id = 2},
	  {.name = "Carol", .department_id = 1},
	  {.name = "Dave", .department_id = 3},
	};
	const auto removed = alt::erase(employees, 1, &employee::department_id);
	EXPECT_EQ(removed, 2u);
	ASSERT_EQ(employees.size(), 2u);
	EXPECT_EQ(employees[0].name, "Bob");
	EXPECT_EQ(employees[1].name, "Dave");
}

TEST(EraseProjection, MemberPointerNoMatchLeavesContainerUnchanged)
{
	std::vector<employee> employees{{.name = "Alice", .department_id = 1}, {.name = "Bob", .department_id = 2}};
	const auto            removed = alt::erase(employees, 99, &employee::department_id);
	EXPECT_EQ(removed, 0u);
	EXPECT_EQ(employees.size(), 2u);
}

TEST(EraseProjection, MemberPointerRemovesAllWhenAllMatch)
{
	std::vector<employee> employees{{.name = "Alice", .department_id = 5}, {.name = "Bob", .department_id = 5}};
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

// ---------------------------------------------------------------------------
// erase — node-based containers
// ---------------------------------------------------------------------------

TEST(EraseNodeBased, MapByMappedValue)
{
	std::map<int, std::string> m{{1, "a"}, {2, "b"}, {3, "a"}};
	const auto                 removed = alt::erase(m, std::string{"a"}, alt::value);
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(m, (std::map<int, std::string>{{2, "b"}}));
}

TEST(EraseNodeBased, MapByKey)
{
	std::map<int, std::string> m{{1, "a"}, {2, "b"}, {3, "c"}};
	const auto                 removed = alt::erase(m, 2, alt::key);
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(m, (std::map<int, std::string>{{1, "a"}, {3, "c"}}));
}

TEST(EraseNodeBased, MapByWholeElementWithDefaultProjection)
{
	std::map<int, std::string> m{{1, "a"}, {2, "b"}};
	const auto                 removed = alt::erase(m, std::pair<const int, std::string>{2, "b"});
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(m, (std::map<int, std::string>{{1, "a"}}));
}

TEST(EraseNodeBased, MultimapRemovesEveryMatchingKey)
{
	std::multimap<int, std::string> m{{1, "a"}, {1, "b"}, {2, "c"}, {1, "d"}};
	const auto                      removed = alt::erase(m, 1, alt::key);
	EXPECT_EQ(removed, 3u);
	ASSERT_EQ(m.size(), 1u);
	EXPECT_EQ(m.begin()->second, "c");
}

TEST(EraseNodeBased, SetWithDefaultProjection)
{
	std::set<int> s{1, 2, 3};
	const auto    removed = alt::erase(s, 2);
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(s, (std::set<int>{1, 3}));
}

TEST(EraseNodeBased, UnorderedMapByMappedValue)
{
	std::unordered_map<int, int> m{{1, 10}, {2, 20}, {3, 10}};
	const auto                   removed = alt::erase(m, 10, alt::value);
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(m, (std::unordered_map<int, int>{{2, 20}}));
}

TEST(EraseNodeBased, ListIsErasedThroughTheEraseRemovePath)
{
	std::list<int> l{1, 2, 3, 2};
	const auto     removed = alt::erase(l, 2);
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(l, (std::list<int>{1, 3}));
}

TEST(EraseDispatch, SequenceContainersTakeEraseRemoveAndNodeContainersDoNot)
{
	// Sequence containers must keep the linear erase-remove idiom rather than quietly
	// falling back to the quadratic node-by-node loop.
	static_assert(alt::detail::erase_remove_capable<std::vector<int>>);
	static_assert(alt::detail::erase_remove_capable<std::deque<int>>);
	static_assert(alt::detail::erase_remove_capable<std::string>);
	static_assert(alt::detail::erase_remove_capable<std::list<int>>);

	// Node-based containers cannot permute their elements, so they must not.
	static_assert(!alt::detail::erase_remove_capable<std::map<int, int>>);
	static_assert(!alt::detail::erase_remove_capable<std::multimap<int, int>>);
	static_assert(!alt::detail::erase_remove_capable<std::set<int>>);
	static_assert(!alt::detail::erase_remove_capable<std::unordered_map<int, int>>);
	static_assert(alt::detail::node_erase_capable<std::map<int, int>>);
	static_assert(alt::detail::node_erase_capable<std::set<int>>);
	static_assert(alt::detail::node_erase_capable<std::unordered_map<int, int>>);

	// forward_list supports neither, which is why it is rejected outright.
	static_assert(!alt::detail::erase_remove_capable<std::forward_list<int>>);
	static_assert(!alt::detail::node_erase_capable<std::forward_list<int>>);
}

TEST(EraseNodeBased, NoMatchLeavesMapUnchanged)
{
	std::map<int, std::string> m{{1, "a"}, {2, "b"}};
	const auto                 removed = alt::erase(m, 99, alt::key);
	EXPECT_EQ(removed, 0u);
	EXPECT_EQ(m.size(), 2u);
}

TEST(EraseNodeBased, AllMatchEmptiesTheMap)
{
	std::map<int, std::string> m{{1, "a"}, {2, "a"}};
	const auto                 removed = alt::erase(m, std::string{"a"}, alt::value);
	EXPECT_EQ(removed, 2u);
	EXPECT_TRUE(m.empty());
}

TEST(EraseNodeBased, EmptyMapReturnsZero)
{
	std::map<int, std::string> m;
	const auto                 removed = alt::erase(m, 1, alt::key);
	EXPECT_EQ(removed, 0u);
}

TEST(EraseNodeBased, ErasingTheFirstAndLastEntriesKeepsIterationValid)
{
	std::map<int, int> m{{1, 0}, {2, 1}, {3, 0}, {4, 1}, {5, 0}};
	const auto         removed = alt::erase(m, 0, alt::value);
	EXPECT_EQ(removed, 3u);
	EXPECT_EQ(m, (std::map<int, int>{{2, 1}, {4, 1}}));
}

// ---------------------------------------------------------------------------
// erase_if — sequence containers
// ---------------------------------------------------------------------------

TEST(EraseIf, RemovesMatchingElementsFromAVector)
{
	std::vector<int> v{1, 2, 3, 4, 5, 6};
	const auto       removed = alt::erase_if(v, [](int x) { return x % 2 == 0; });
	EXPECT_EQ(removed, 3u);
	EXPECT_EQ(v, (std::vector<int>{1, 3, 5}));
}

TEST(EraseIf, ReturnsZeroWhenNothingMatches)
{
	std::vector<int> v{1, 3, 5};
	const auto       removed = alt::erase_if(v, [](int x) { return x % 2 == 0; });
	EXPECT_EQ(removed, 0u);
	EXPECT_EQ(v, (std::vector<int>{1, 3, 5}));
}

TEST(EraseIf, RemovesEverythingWhenAllMatch)
{
	std::vector<int> v{2, 4, 6};
	const auto       removed = alt::erase_if(v, [](int x) { return x % 2 == 0; });
	EXPECT_EQ(removed, 3u);
	EXPECT_TRUE(v.empty());
}

TEST(EraseIf, EmptyContainerReturnsZero)
{
	std::vector<int> v;
	const auto       removed = alt::erase_if(v, [](int) { return true; });
	EXPECT_EQ(removed, 0u);
}

TEST(EraseIf, WorksOnADequeAndAString)
{
	std::deque<int> d{1, 2, 3, 4};
	EXPECT_EQ(alt::erase_if(d, [](int x) { return x > 2; }), 2u);
	EXPECT_EQ(d, (std::deque<int>{1, 2}));

	std::string s{"hello world"};
	EXPECT_EQ(alt::erase_if(s, [](char c) { return c == 'l'; }), 3u);
	EXPECT_EQ(s, "heo word");
}

// ---------------------------------------------------------------------------
// erase_if — projections
// ---------------------------------------------------------------------------

TEST(EraseIfProjection, PredicateSeesTheProjectedValue)
{
	std::vector<employee> employees{{.name = "Alice", .department_id = 1}, {.name = "Bob", .department_id = 2}, {.name = "Carol", .department_id = 3}};
	const auto            removed =
	  alt::erase_if(employees, [](int dept) { return dept > 1; }, &employee::department_id);
	EXPECT_EQ(removed, 2u);
	ASSERT_EQ(employees.size(), 1u);
	EXPECT_EQ(employees[0].name, "Alice");
}

TEST(EraseIfProjection, LambdaProjection)
{
	std::vector<std::string> words{"hi", "hello", "by", "world"};
	const auto               removed =
	  alt::erase_if(words, [](std::size_t n) { return n < 3; }, [](const std::string& s) { return s.size(); });
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(words, (std::vector<std::string>{"hello", "world"}));
}

// ---------------------------------------------------------------------------
// erase_if — node-based containers
// ---------------------------------------------------------------------------

TEST(EraseIfNodeBased, MapOnTheKey)
{
	std::map<int, std::string> m{{1, "a"}, {2, "b"}, {3, "c"}};
	const auto                 removed = alt::erase_if(m, [](int k) { return k > 1; }, alt::key);
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(m, (std::map<int, std::string>{{1, "a"}}));
}

TEST(EraseIfNodeBased, MapOnTheMappedValue)
{
	std::map<int, int> m{{1, 10}, {2, 25}, {3, 30}};
	const auto         removed = alt::erase_if(m, [](int v) { return v % 10 != 0; }, alt::value);
	EXPECT_EQ(removed, 1u);
	EXPECT_EQ(m, (std::map<int, int>{{1, 10}, {3, 30}}));
}

TEST(EraseIfNodeBased, MapWithDefaultProjectionSeesTheWholeEntry)
{
	std::map<int, int> m{{1, 1}, {2, 5}, {3, 3}};
	const auto         removed =
	  alt::erase_if(m, [](const std::pair<const int, int>& e) { return e.first == e.second; });
	EXPECT_EQ(removed, 2u);
	EXPECT_EQ(m, (std::map<int, int>{{2, 5}}));
}

TEST(EraseIfNodeBased, UnorderedMapAndSet)
{
	std::unordered_map<int, int> m{{1, 1}, {2, 2}, {3, 3}};
	EXPECT_EQ(alt::erase_if(m, [](int k) { return k != 2; }, alt::key), 2u);
	EXPECT_EQ(m, (std::unordered_map<int, int>{{2, 2}}));

	std::set<int> s{1, 2, 3, 4};
	EXPECT_EQ(alt::erase_if(s, [](int x) { return x % 2 == 0; }), 2u);
	EXPECT_EQ(s, (std::set<int>{1, 3}));
}

// ---------------------------------------------------------------------------
// Constraints and return types
// ---------------------------------------------------------------------------

namespace
{
	// Always-true predicate, so the concepts below stay free of lambdas.
	struct always
	{
		constexpr bool operator()(const auto&) const noexcept
		{
			return true;
		}
	};

	// alt::erase and alt::erase_if are constrained function templates, and a
	// requires-expression naming them only reports an unsatisfied requirement from a
	// dependent context. Hence these wrappers rather than a bare static_assert.
	template<typename Container, typename Value, typename Proj = std::identity>
	concept erase_usable = requires(Container& c, const Value& v, Proj p) { alt::erase(c, v, p); };

	template<typename Container, typename Proj = std::identity>
	concept erase_if_usable = requires(Container& c, Proj p) { alt::erase_if(c, always{}, p); };
} // namespace

TEST(EraseConstraints, ForwardListIsRejected)
{
	static_assert(!erase_usable<std::forward_list<int>, int>);
	static_assert(!erase_if_usable<std::forward_list<int>>);
}

TEST(EraseConstraints, SequenceAndNodeBasedContainersAreAccepted)
{
	static_assert(erase_usable<std::vector<int>, int>);
	static_assert(erase_usable<std::list<int>, int>);
	static_assert(erase_usable<std::map<int, int>, int, decltype(alt::key)>);
	static_assert(erase_usable<std::set<int>, int>);
	static_assert(erase_if_usable<std::vector<int>>);
	static_assert(erase_if_usable<std::map<int, int>, decltype(alt::key)>);
	static_assert(erase_if_usable<std::unordered_map<int, int>, decltype(alt::value)>);
}

TEST(EraseIfReturnType, ReturnTypeIsSizeType)
{
	std::vector<int>   v{1, 2, 3};
	std::map<int, int> m{{1, 1}};
	static_assert(std::is_same_v<decltype(alt::erase_if(v, [](int) { return true; })),
	                             std::vector<int>::size_type>);
	static_assert(std::is_same_v<decltype(alt::erase_if(m, [](const std::pair<const int, int>&) { return true; })),
	                             std::map<int, int>::size_type>);
}

// ---------------------------------------------------------------------------
// constexpr evaluation
// ---------------------------------------------------------------------------

TEST(EraseConstexpr, EraseAndEraseIfWorkInConstantEvaluation)
{
	static_assert([] {
		std::vector<int> v{1, 2, 3, 2};
		return alt::erase(v, 2) == 2u && v == std::vector<int>{1, 3};
	}());

	static_assert([] {
		std::vector<int> v{1, 2, 3, 4};
		return alt::erase_if(v, [](int x) { return x % 2 == 0; }) == 2u &&
		       v == std::vector<int>{1, 3};
	}());
}
