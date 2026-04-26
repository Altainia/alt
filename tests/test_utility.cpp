#include <gtest/gtest.h>

#include <alt/utility.hpp>
#include <cstdint>
#include <type_traits>

namespace
{

	enum class color : uint8_t
	{
		red   = 1,
		green = 2,
		blue  = 255
	};

	enum class signed_tag : int16_t
	{
		neg = -10,
		pos = 10
	};

	enum unscoped_val
	{
		zero = 0,
		one  = 1
	};

	// --- return type correctness ---

	static_assert(std::is_same_v<decltype(alt::to_underlying(color::red)), uint8_t>);
	static_assert(std::is_same_v<decltype(alt::to_underlying(signed_tag::neg)), int16_t>);
	static_assert(std::is_same_v<decltype(alt::to_underlying(zero)), unsigned int>);
	static_assert(std::is_same_v<decltype(alt::to_underlying(42)), int>);
	static_assert(std::is_same_v<decltype(alt::to_underlying(true)), bool>);

	// --- enum values ---

	TEST(ToUnderlying, ScopedEnumUint8)
	{
		EXPECT_EQ(alt::to_underlying(color::red), uint8_t{1});
		EXPECT_EQ(alt::to_underlying(color::green), uint8_t{2});
		EXPECT_EQ(alt::to_underlying(color::blue), uint8_t{255});
	}

	TEST(ToUnderlying, ScopedEnumInt16)
	{
		EXPECT_EQ(alt::to_underlying(signed_tag::neg), int16_t{-10});
		EXPECT_EQ(alt::to_underlying(signed_tag::pos), int16_t{10});
	}

	TEST(ToUnderlying, UnscopedEnum)
	{
		EXPECT_EQ(alt::to_underlying(zero), 0u);
		EXPECT_EQ(alt::to_underlying(one), 1u);
	}

	// --- integral identity ---

	TEST(ToUnderlying, Int)
	{
		EXPECT_EQ(alt::to_underlying(0), 0);
		EXPECT_EQ(alt::to_underlying(-1), -1);
		EXPECT_EQ(alt::to_underlying(42), 42);
	}

	TEST(ToUnderlying, Uint64)
	{
		constexpr uint64_t val = 0xDEADBEEFCAFEBABEULL;
		EXPECT_EQ(alt::to_underlying(val), val);
	}

	TEST(ToUnderlying, Bool)
	{
		EXPECT_EQ(alt::to_underlying(true), true);
		EXPECT_EQ(alt::to_underlying(false), false);
	}

} // namespace
