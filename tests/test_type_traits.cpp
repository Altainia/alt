#include <alt/type_traits.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace
{

	enum class scoped_i8 : int8_t
	{
		a = -1,
		b = 0
	};
	enum class scoped_u32 : uint32_t
	{
		a = 0,
		b = 0xFFFFFFFF
	};
	enum class scoped_i64 : int64_t
	{
		a = -1,
		b = 100
	};
	enum unscoped_uint
	{
		x = 0,
		y = 1
	};

	// --- underlying_int_t for enum types ---

	static_assert(std::is_same_v<alt::underlying_int_t<scoped_i8>, int8_t>);
	static_assert(std::is_same_v<alt::underlying_int_t<scoped_u32>, uint32_t>);
	static_assert(std::is_same_v<alt::underlying_int_t<scoped_i64>, int64_t>);
	static_assert(std::is_same_v<alt::underlying_int_t<unscoped_uint>, unsigned int>);

	// --- underlying_int_t for integral types (identity) ---

	static_assert(std::is_same_v<alt::underlying_int_t<int>, int>);
	static_assert(std::is_same_v<alt::underlying_int_t<unsigned int>, unsigned int>);
	static_assert(std::is_same_v<alt::underlying_int_t<int8_t>, int8_t>);
	static_assert(std::is_same_v<alt::underlying_int_t<uint64_t>, uint64_t>);
	static_assert(std::is_same_v<alt::underlying_int_t<bool>, bool>);

	// Dummy test so the binary links and the static_asserts are evaluated.
	TEST(UnderlyingInt, StaticAssertionsPass)
	{}

} // namespace
