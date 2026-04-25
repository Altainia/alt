#include <gtest/gtest.h>

#include <alt/concepts.hpp>
#include <alt/flags.hpp>

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

enum class perm : unsigned char
{
	read    = 0x01,
	write   = 0x02,
	execute = 0x04,
};

// Default underlying type is int (signed) — exercises make_unsigned_t.
enum class opt
{
	verbose = 1 << 0,
	debug   = 1 << 1,
	quiet   = 1 << 2,
};

enum unscoped
{
	a,
	b
};

using perm_flags = alt::flags<perm>;
using opt_flags  = alt::flags<opt>;

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

TEST(Concepts, AnyEnum)
{
	static_assert(alt::any_enum<perm>);
	static_assert(alt::any_enum<opt>);
	static_assert(alt::any_enum<unscoped>);
	static_assert(!alt::any_enum<int>);
	static_assert(!alt::any_enum<unsigned char>);
}

TEST(Concepts, ScopedEnum)
{
	static_assert(alt::scoped_enum<perm>);
	static_assert(alt::scoped_enum<opt>);
	static_assert(!alt::scoped_enum<unscoped>);
	static_assert(!alt::scoped_enum<int>);
}

// ---------------------------------------------------------------------------
// Member types
// ---------------------------------------------------------------------------

TEST(FlagsTypes, ValueTypeIsUnsigned)
{
	static_assert(std::is_unsigned_v<perm_flags::value_type>);
	static_assert(std::is_same_v<perm_flags::value_type, unsigned char>);

	// opt has a signed underlying type; value_type must still be unsigned.
	static_assert(std::is_unsigned_v<opt_flags::value_type>);
	static_assert(std::is_same_v<opt_flags::value_type, unsigned int>);
}

TEST(FlagsTypes, EnumType)
{
	static_assert(std::is_same_v<perm_flags::enum_type, perm>);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(FlagsConstruction, DefaultIsEmpty)
{
	constexpr perm_flags f;
	EXPECT_EQ(f.value(), 0u);
	EXPECT_TRUE(f.empty());
}

TEST(FlagsConstruction, ExplicitFromEnumerator)
{
	constexpr perm_flags r{perm::read};
	constexpr perm_flags w{perm::write};
	constexpr perm_flags x{perm::execute};
	EXPECT_EQ(r.value(), 0x01u);
	EXPECT_EQ(w.value(), 0x02u);
	EXPECT_EQ(x.value(), 0x04u);
}

// ---------------------------------------------------------------------------
// Bitwise operators
// ---------------------------------------------------------------------------

TEST(FlagsBitwiseOps, Union)
{
	constexpr perm_flags rw = perm_flags{perm::read} | perm_flags{perm::write};
	EXPECT_EQ(rw.value(), 0x03u);
}

TEST(FlagsBitwiseOps, Intersection)
{
	constexpr perm_flags rw   = perm_flags{perm::read} | perm_flags{perm::write};
	constexpr perm_flags r    = rw & perm_flags{perm::read};
	constexpr perm_flags none = perm_flags{perm::read} & perm_flags{perm::write};
	EXPECT_EQ(r.value(), 0x01u);
	EXPECT_EQ(none.value(), 0u);
}

TEST(FlagsBitwiseOps, SymmetricDifference)
{
	constexpr perm_flags rw    = perm_flags{perm::read} | perm_flags{perm::write};
	constexpr perm_flags xored = rw ^ perm_flags{perm::read};
	constexpr perm_flags same  = perm_flags{perm::read} ^ perm_flags { perm::read };
	EXPECT_EQ(xored.value(), 0x02u);
	EXPECT_EQ(same.value(), 0u);
}

TEST(FlagsBitwiseOps, Complement)
{
	constexpr perm_flags    r      = perm_flags{perm::read};
	constexpr perm_flags    not_r  = ~r;
	constexpr unsigned char expect = static_cast<unsigned char>(~static_cast<unsigned char>(0x01));
	EXPECT_EQ(not_r.value(), expect);
}

TEST(FlagsBitwiseOps, OrAssign)
{
	perm_flags f;
	f |= perm_flags{perm::read};
	f |= perm_flags{perm::write};
	EXPECT_EQ(f.value(), 0x03u);
}

TEST(FlagsBitwiseOps, AndAssign)
{
	perm_flags f = perm_flags{perm::read} | perm_flags{perm::write};
	f &= perm_flags{perm::read};
	EXPECT_EQ(f.value(), 0x01u);
}

TEST(FlagsBitwiseOps, XorAssign)
{
	perm_flags f = perm_flags{perm::read} | perm_flags{perm::write};
	f ^= perm_flags{perm::read};
	EXPECT_EQ(f.value(), 0x02u);
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

TEST(FlagsMutation, Set)
{
	perm_flags f;
	f.set(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0x01u);
	f.set(perm_flags{perm::write});
	EXPECT_EQ(f.value(), 0x03u);
	// Setting an already-set bit is idempotent.
	f.set(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0x03u);
}

TEST(FlagsMutation, Clear)
{
	perm_flags f = perm_flags{perm::read} | perm_flags{perm::write};
	f.clear(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0x02u);
	// Clearing an already-clear bit is idempotent.
	f.clear(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0x02u);
}

TEST(FlagsMutation, Toggle)
{
	perm_flags f;
	f.toggle(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0x01u);
	f.toggle(perm_flags{perm::read});
	EXPECT_EQ(f.value(), 0u);
}

TEST(FlagsMutation, Chaining)
{
	perm_flags f;
	f.set(perm_flags{perm::read})
	  .set(perm_flags{perm::write})
	  .set(perm_flags{perm::execute})
	  .clear(perm_flags{perm::read})
	  .toggle(perm_flags{perm::execute});
	// write remains, read cleared, execute toggled off.
	EXPECT_EQ(f.value(), 0x02u);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(FlagsQueries, HasAll)
{
	constexpr perm_flags rwx = perm_flags{perm::read} | perm_flags{perm::write} | perm_flags{perm::execute};
	EXPECT_TRUE(rwx.has_all(perm_flags{perm::read}));
	EXPECT_TRUE(rwx.has_all(perm_flags{perm::read} | perm_flags{perm::write}));
	EXPECT_TRUE(rwx.has_all(rwx));
	EXPECT_FALSE(perm_flags{perm::read}.has_all(perm_flags{perm::write}));
	EXPECT_FALSE(perm_flags{perm::read}.has_all(perm_flags{perm::read} | perm_flags{perm::write}));
}

TEST(FlagsQueries, HasAllEmptyMaskIsVacuouslyTrue)
{
	EXPECT_TRUE(perm_flags{}.has_all(perm_flags{}));
	EXPECT_TRUE((perm_flags{perm::read}).has_all(perm_flags{}));
}

TEST(FlagsQueries, HasAny)
{
	constexpr perm_flags rw = perm_flags{perm::read} | perm_flags{perm::write};
	EXPECT_TRUE(rw.has_any(perm_flags{perm::read}));
	EXPECT_TRUE(rw.has_any(perm_flags{perm::read} | perm_flags{perm::execute}));
	EXPECT_FALSE(rw.has_any(perm_flags{perm::execute}));
	EXPECT_FALSE(perm_flags{}.has_any(perm_flags{perm::read}));
}

TEST(FlagsQueries, HasAnyEmptyMaskIsFalse)
{
	EXPECT_FALSE(perm_flags{}.has_any(perm_flags{}));
	EXPECT_FALSE((perm_flags{perm::read}).has_any(perm_flags{}));
}

TEST(FlagsQueries, HasNone)
{
	constexpr perm_flags rw = perm_flags{perm::read} | perm_flags{perm::write};
	EXPECT_TRUE(rw.has_none(perm_flags{perm::execute}));
	EXPECT_FALSE(rw.has_none(perm_flags{perm::read}));
	EXPECT_FALSE(rw.has_none(perm_flags{perm::read} | perm_flags{perm::execute}));
}

TEST(FlagsQueries, HasNoneEmptyMaskIsVacuouslyTrue)
{
	EXPECT_TRUE(perm_flags{}.has_none(perm_flags{}));
	EXPECT_TRUE((perm_flags{perm::read}).has_none(perm_flags{}));
}

TEST(FlagsQueries, Empty)
{
	EXPECT_TRUE(perm_flags{}.empty());
	EXPECT_FALSE(perm_flags{perm::read}.empty());
}

TEST(FlagsQueries, Matches)
{
	constexpr perm_flags r = perm_flags{perm::read};
	EXPECT_TRUE(r.matches(perm_flags{perm::read}));
	EXPECT_FALSE(r.matches(perm_flags{perm::write}));
	EXPECT_FALSE(r.matches(perm_flags{}));
}

TEST(FlagsQueries, Count)
{
	EXPECT_EQ(perm_flags{}.count(), 0);
	EXPECT_EQ(perm_flags{perm::read}.count(), 1);
	EXPECT_EQ((perm_flags{perm::read} | perm_flags{perm::write}).count(), 2);
	EXPECT_EQ((perm_flags{perm::read} | perm_flags{perm::write} | perm_flags{perm::execute}).count(), 3);
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

TEST(FlagsConversions, ToBool)
{
	EXPECT_FALSE(static_cast<bool>(perm_flags{}));
	EXPECT_TRUE(static_cast<bool>(perm_flags{perm::read}));
}

TEST(FlagsConversions, ToValueType)
{
	constexpr perm_flags rw = perm_flags{perm::read} | perm_flags{perm::write};
	EXPECT_EQ(static_cast<perm_flags::value_type>(rw), 0x03u);
}

TEST(FlagsConversions, ToEnumType)
{
	constexpr perm_flags r = perm_flags{perm::read};
	EXPECT_EQ(static_cast<perm>(r), perm::read);
}

TEST(FlagsConversions, Value)
{
	constexpr perm_flags rw = perm_flags{perm::read} | perm_flags{perm::write};
	EXPECT_EQ(rw.value(), 0x03u);
}

TEST(FlagsConversions, FromValueRoundtrip)
{
	constexpr perm_flags::value_type raw = 0x05u;
	constexpr perm_flags             f   = perm_flags::from_value(raw);
	EXPECT_EQ(f.value(), raw);
	EXPECT_EQ(static_cast<perm_flags::value_type>(f), raw);
}

// ---------------------------------------------------------------------------
// Equality
// ---------------------------------------------------------------------------

TEST(FlagsEquality, EqualOperator)
{
	EXPECT_EQ(perm_flags{}, perm_flags{});
	EXPECT_EQ(perm_flags{perm::read}, perm_flags{perm::read});
	EXPECT_NE(perm_flags{perm::read}, perm_flags{perm::write});
	EXPECT_NE(perm_flags{perm::read}, perm_flags{});
}

TEST(FlagsEquality, MatchesAgreesWithEqualOperator)
{
	const perm_flags a = perm_flags{perm::read} | perm_flags{perm::write};
	const perm_flags b = perm_flags{perm::read} | perm_flags{perm::write};
	const perm_flags c = perm_flags{perm::execute};
	EXPECT_TRUE(a.matches(b));
	EXPECT_EQ(a.matches(b), a == b);
	EXPECT_FALSE(a.matches(c));
	EXPECT_EQ(a.matches(c), a == c);
}

// ---------------------------------------------------------------------------
// Signed underlying type
// ---------------------------------------------------------------------------

TEST(FlagsSignedUnderlying, BasicOps)
{
	constexpr opt_flags vd = opt_flags{opt::verbose} | opt_flags{opt::debug};
	EXPECT_EQ(vd.count(), 2);
	EXPECT_TRUE(vd.has_all(opt_flags{opt::verbose}));
	EXPECT_FALSE(vd.has_any(opt_flags{opt::quiet}));
}

// ---------------------------------------------------------------------------
// Constexpr correctness
// ---------------------------------------------------------------------------

TEST(FlagsConstexpr, StaticAsserts)
{
	constexpr perm_flags empty{};
	static_assert(empty.empty());
	static_assert(empty.value() == 0u);
	static_assert(!static_cast<bool>(empty));

	constexpr perm_flags r{perm::read};
	static_assert(!r.empty());
	static_assert(r.value() == 0x01u);
	static_assert(static_cast<bool>(r));
	static_assert(r.count() == 1);

	constexpr perm_flags rw = r | perm_flags{perm::write};
	static_assert(rw.value() == 0x03u);
	static_assert(rw.count() == 2);
	static_assert(rw.has_all(r));
	static_assert(rw.has_any(perm_flags{perm::write}));
	static_assert(rw.has_none(perm_flags{perm::execute}));
	static_assert(rw.matches(perm_flags::from_value(0x03u)));
	static_assert(rw == perm_flags::from_value(0x03u));
}
