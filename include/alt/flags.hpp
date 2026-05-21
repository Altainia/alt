#pragma once

#include <alt/concepts.hpp>
#include <bit>
#include <type_traits>

namespace alt
{

	/**
	 * @brief A type-safe bitfield wrapper over a scoped enum.
	 *
	 * @tparam Enum A scoped enumeration whose enumerators represent individual bits.
	 *
	 * Storage is the unsigned equivalent of the enum's underlying type.
	 * All operations are constexpr.
	 */
	template<scoped_enum Enum>
	class flags
	{
	public:
		using enum_type  = Enum;
		using value_type = std::make_unsigned_t<std::underlying_type_t<Enum>>;

		/** Constructs an empty flags value with all bits clear. */
		constexpr flags() noexcept = default;

		/** Constructs a flags value representing a single enumerator. */
		constexpr explicit flags(Enum e) noexcept:
		  m_value{static_cast<value_type>(e)}
		{
		}

		// -------------------------------------------------------------------------
		// Bitwise operators
		// -------------------------------------------------------------------------

		/** Returns the union of two flag sets. */
		[[nodiscard]] constexpr flags operator|(flags other) const noexcept
		{
			return from_value(m_value | other.m_value);
		}

		/** Returns the intersection of two flag sets. */
		[[nodiscard]] constexpr flags operator&(flags other) const noexcept
		{
			return from_value(m_value & other.m_value);
		}

		/** Returns the symmetric difference of two flag sets. */
		[[nodiscard]] constexpr flags operator^(flags other) const noexcept
		{
			return from_value(m_value ^ other.m_value);
		}

		/** Returns the bitwise complement of all bits in the underlying storage. */
		[[nodiscard]] constexpr flags operator~() const noexcept
		{
			return from_value(static_cast<value_type>(~m_value));
		}

		/** Sets all bits present in @p other. */
		constexpr flags& operator|=(flags other) noexcept
		{
			m_value |= other.m_value;
			return *this;
		}

		/** Clears all bits not present in @p other. */
		constexpr flags& operator&=(flags other) noexcept
		{
			m_value &= other.m_value;
			return *this;
		}

		/** Toggles all bits present in @p other. */
		constexpr flags& operator^=(flags other) noexcept
		{
			m_value ^= other.m_value;
			return *this;
		}

		// -------------------------------------------------------------------------
		// Mutation
		// -------------------------------------------------------------------------

		/** Sets all bits in @p mask. */
		constexpr flags& set(flags mask) noexcept
		{
			m_value |= mask.m_value;
			return *this;
		}

		/** Clears all bits in @p mask. */
		constexpr flags& clear(flags mask) noexcept
		{
			m_value &= ~mask.m_value;
			return *this;
		}

		/** Toggles all bits in @p mask. */
		constexpr flags& toggle(flags mask) noexcept
		{
			m_value ^= mask.m_value;
			return *this;
		}

		// -------------------------------------------------------------------------
		// Queries
		// -------------------------------------------------------------------------

		/** Returns true if every bit in @p mask is set. Vacuously true for an empty mask. */
		[[nodiscard]] constexpr bool has_all(flags mask) const noexcept
		{
			return (m_value & mask.m_value) == mask.m_value;
		}

		/** Returns true if at least one bit in @p mask is set. */
		[[nodiscard]] constexpr bool has_any(flags mask) const noexcept
		{
			return (m_value & mask.m_value) != value_type{0};
		}

		/** Returns true if no bit in @p mask is set. Vacuously true for an empty mask. */
		[[nodiscard]] constexpr bool has_none(flags mask) const noexcept
		{
			return (m_value & mask.m_value) == value_type{0};
		}

		/** Returns true if no bits are set. */
		[[nodiscard]] constexpr bool empty() const noexcept
		{
			return m_value == value_type{0};
		}

		/** Returns true if this value equals @p other. Synonym for operator==. */
		[[nodiscard]] constexpr bool matches(flags other) const noexcept
		{
			return m_value == other.m_value;
		}

		/** Returns the number of set bits. */
		[[nodiscard]] constexpr int count() const noexcept
		{
			return std::popcount(m_value);
		}

		// -------------------------------------------------------------------------
		// Conversions
		// -------------------------------------------------------------------------

		/** Returns true if any bit is set. */
		[[nodiscard]] constexpr explicit operator bool() const noexcept
		{
			return !empty();
		}

		/** Returns the raw underlying unsigned integer value. */
		[[nodiscard]] constexpr explicit operator value_type() const noexcept
		{
			return m_value;
		}

		/**
		 * @brief Converts to the enum type via static_cast.
		 *
		 * The result may not name a valid enumerator if multiple bits are set.
		 */
		[[nodiscard]] constexpr explicit operator enum_type() const noexcept
		{
			return static_cast<enum_type>(m_value);
		}

		/** Returns the raw underlying unsigned integer value. */
		[[nodiscard]] constexpr value_type value() const noexcept
		{
			return m_value;
		}

		/**
		 * @brief Constructs a flags value directly from a raw integer.
		 *
		 * The caller is responsible for ensuring @p v is a meaningful combination
		 * of enumerator bits.
		 */
		[[nodiscard]] static constexpr flags from_value(value_type v) noexcept
		{
			flags f;
			f.m_value = v;
			return f;
		}

		// -------------------------------------------------------------------------
		// Equality
		// -------------------------------------------------------------------------

		[[nodiscard]] constexpr bool operator==(const flags& other) const noexcept = default;

	private:
		value_type m_value{};
	};

} // namespace alt
