#include <gtest/gtest.h>

#include <algorithm>
#include <alt/projection.hpp>
#include <array>
#include <cstddef>
#include <map>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace adl
{

	// Tuple-like only via an ADL-found get(), with no member access and no std::get
	// specialization. Used to prove element<N> performs unqualified lookup.
	struct adl_pair
	{
		int a{};
		int b{};
	};

	template<std::size_t N>
	constexpr const int& get(const adl_pair& p) noexcept
	{
		static_assert(N < 2, "adl_pair has exactly two elements");
		if constexpr(N == 0)
		{
			return p.a;
		}
		else
		{
			return p.b;
		}
	}

} // namespace adl

// ---------------------------------------------------------------------------
// key / value — basic access
// ---------------------------------------------------------------------------

TEST(ProjectionKey, ReadsFirstOfAPair)
{
	const std::pair<int, std::string> p{7, "seven"};
	EXPECT_EQ(alt::key(p), 7);
}

TEST(ProjectionValue, ReadsSecondOfAPair)
{
	const std::pair<int, std::string> p{7, "seven"};
	EXPECT_EQ(alt::value(p), "seven");
}

TEST(ProjectionKey, ReadsKeyOfAMapEntry)
{
	const std::map<int, std::string> m{{1, "one"}};
	const auto&                      entry = *m.begin();
	EXPECT_EQ(alt::key(entry), 1);
	EXPECT_EQ(alt::value(entry), "one");
}

// ---------------------------------------------------------------------------
// key / value — value category and constness
// ---------------------------------------------------------------------------

TEST(ProjectionKey, PreservesConstness)
{
	std::pair<int, std::string>       mutable_pair{1, "a"};
	const std::pair<int, std::string> const_pair{1, "a"};

	static_assert(std::is_same_v<decltype(alt::key(mutable_pair)), int&>);
	static_assert(std::is_same_v<decltype(alt::key(const_pair)), const int&>);
	static_assert(std::is_same_v<decltype(alt::value(mutable_pair)), std::string&>);
	static_assert(std::is_same_v<decltype(alt::value(const_pair)), const std::string&>);
}

TEST(ProjectionKey, PreservesRvalueness)
{
	static_assert(std::is_same_v<decltype(alt::key(std::pair<int, std::string>{})), int&&>);
	static_assert(
	  std::is_same_v<decltype(alt::value(std::pair<int, std::string>{})), std::string&&>);
}

TEST(ProjectionValue, YieldsAReferenceThatCanBeWrittenThrough)
{
	std::pair<int, std::string> p{1, "a"};
	alt::value(p) = "changed";
	EXPECT_EQ(p.second, "changed");
}

TEST(ProjectionValue, MovesOutOfAnRvaluePair)
{
	std::pair<int, std::string> p{1, "a long string that will not fit in a small buffer"};
	const std::string           moved = alt::value(std::move(p));
	EXPECT_EQ(moved, "a long string that will not fit in a small buffer");
	EXPECT_TRUE(p.second.empty()); // NOLINT(bugprone-use-after-move): checking the moved-from state
}

// ---------------------------------------------------------------------------
// key / value — a map entry's key is const
// ---------------------------------------------------------------------------

TEST(ProjectionKey, MapEntryKeyIsConstEvenThroughAMutableEntry)
{
	std::map<int, std::string> m{{1, "one"}};
	auto&                      entry = *m.begin();
	static_assert(std::is_same_v<decltype(alt::key(entry)), const int&>);
	static_assert(std::is_same_v<decltype(alt::value(entry)), std::string&>);
}

// ---------------------------------------------------------------------------
// element<N>
// ---------------------------------------------------------------------------

TEST(ProjectionElement, ReadsFromATuple)
{
	const std::tuple<int, std::string, double> t{1, "two", 3.0};
	EXPECT_EQ(alt::element<0>(t), 1);
	EXPECT_EQ(alt::element<1>(t), "two");
	EXPECT_DOUBLE_EQ(alt::element<2>(t), 3.0);
}

TEST(ProjectionElement, ReadsFromAPair)
{
	const std::pair<int, std::string> p{4, "four"};
	EXPECT_EQ(alt::element<0>(p), 4);
	EXPECT_EQ(alt::element<1>(p), "four");
}

TEST(ProjectionElement, ReadsFromAnArray)
{
	const std::array<int, 3> a{10, 20, 30};
	EXPECT_EQ(alt::element<0>(a), 10);
	EXPECT_EQ(alt::element<2>(a), 30);
}

TEST(ProjectionElement, PreservesConstnessAndValueCategory)
{
	std::tuple<int, std::string>       mutable_tuple{1, "a"};
	const std::tuple<int, std::string> const_tuple{1, "a"};

	static_assert(std::is_same_v<decltype(alt::element<0>(mutable_tuple)), int&>);
	static_assert(std::is_same_v<decltype(alt::element<0>(const_tuple)), const int&>);
	static_assert(
	  std::is_same_v<decltype(alt::element<1>(std::tuple<int, std::string>{})), std::string&&>);
}

TEST(ProjectionElement, FindsGetByArgumentDependentLookup)
{
	// adl_pair is tuple-like only through a get() found by argument-dependent lookup,
	// so this fails unless element<N> calls get() unqualified.
	const adl::adl_pair p{.a = 11, .b = 22};
	EXPECT_EQ(alt::element<0>(p), 11);
	EXPECT_EQ(alt::element<1>(p), 22);
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

TEST(ProjectionConstraints, KeyAndValueRejectTypesWithoutThoseMembers)
{
	static_assert(!std::invocable<decltype(alt::key), int>);
	static_assert(!std::invocable<decltype(alt::value), int>);
	static_assert(!std::invocable<decltype(alt::key), std::string>);
}

TEST(ProjectionConstraints, KeyAndValueAcceptPairLikeTypes)
{
	static_assert(std::invocable<decltype(alt::key), std::pair<int, int>&>);
	static_assert(std::invocable<decltype(alt::value), const std::pair<int, int>&>);
}

TEST(ProjectionConstraints, ElementRejectsNonTupleLikeTypes)
{
	static_assert(!std::invocable<decltype(alt::element<0>), int>);
}

TEST(ProjectionConstraints, ElementRejectsAnOutOfRangeIndex)
{
	static_assert(std::invocable<decltype(alt::element<1>), std::pair<int, int>&>);
	static_assert(!std::invocable<decltype(alt::element<2>), std::pair<int, int>&>);
}

// ---------------------------------------------------------------------------
// Use as range projections and adaptors
// ---------------------------------------------------------------------------

TEST(Projection, WorksWithViewsTransform)
{
	const std::map<int, std::string> m{{1, "one"}, {2, "two"}, {3, "three"}};

	std::vector<int> keys;
	for(const int k: m | std::views::transform(alt::key))
	{
		keys.push_back(k);
	}
	EXPECT_EQ(keys, (std::vector<int>{1, 2, 3}));
}

TEST(Projection, WorksAsARangesAlgorithmProjection)
{
	const std::vector<std::pair<int, std::string>> rows{{3, "c"}, {1, "a"}, {2, "b"}};

	const auto it = std::ranges::find(rows, 2, alt::key);
	ASSERT_NE(it, rows.end());
	EXPECT_EQ(it->second, "b");

	const auto smallest = std::ranges::min_element(rows, {}, alt::key);
	ASSERT_NE(smallest, rows.end());
	EXPECT_EQ(smallest->second, "a");
}

TEST(Projection, ElementWorksAsARangesAlgorithmProjection)
{
	const std::vector<std::tuple<int, char>> rows{{1, 'a'}, {2, 'b'}, {3, 'c'}};

	const auto it = std::ranges::find(rows, 'b', alt::element<1>);
	ASSERT_NE(it, rows.end());
	EXPECT_EQ(std::get<0>(*it), 2);
}

// ---------------------------------------------------------------------------
// constexpr evaluation
// ---------------------------------------------------------------------------

TEST(Projection, UsableInConstantEvaluation)
{
	constexpr std::pair<int, char>  p{5, 'z'};
	constexpr std::tuple<int, char> t{6, 'y'};

	static_assert(alt::key(p) == 5);
	static_assert(alt::value(p) == 'z');
	static_assert(alt::element<0>(t) == 6);
	static_assert(alt::element<1>(t) == 'y');
}
