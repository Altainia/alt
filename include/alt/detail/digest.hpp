#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace alt
{

	/** Reasons @c digest::from_hex can reject its input. */
	enum class hex_error
	{
		/** The input was not exactly twice the digest size in characters. */
		invalid_length,
		/** The input contained a character outside @c [0-9a-fA-F]. */
		invalid_character
	};

	namespace detail
	{

		/**
		 * @brief The part of a hash algorithm that a digest needs to know.
		 *
		 * Deliberately narrower than @c alt::hash_algorithm so that this header does
		 * not depend on the engine that produces digests.
		 */
		template<typename T>
		concept digest_traits = requires {
			{ T::digest_size } -> std::convertible_to<std::size_t>;
			{ T::name } -> std::convertible_to<std::string_view>;
		};

		/** Returns the value of a hexadecimal digit, or nothing if @p c is not one. */
		constexpr std::optional<std::uint8_t> decode_nibble(char c) noexcept
		{
			if(c >= '0' && c <= '9')
			{
				return static_cast<std::uint8_t>(c - '0');
			}
			if(c >= 'a' && c <= 'f')
			{
				return static_cast<std::uint8_t>(c - 'a' + 10);
			}
			if(c >= 'A' && c <= 'F')
			{
				return static_cast<std::uint8_t>(c - 'A' + 10);
			}
			return std::nullopt;
		}

	} // namespace detail

	/**
	 * @brief The output of a hash algorithm, as a value.
	 *
	 * Keyed on the algorithm rather than on the digest length. SHA-224 and SHA-512/224
	 * both produce 28 bytes, as SHA-256 and SHA-512/256 both produce 32; keying on
	 * length would make each such pair the same type and turn a mixed-up algorithm
	 * into a silent comparison failure instead of a compile error.
	 *
	 * @tparam Algorithm The algorithm that produced the digest.
	 */
	template<detail::digest_traits Algorithm>
	class digest
	{
	public:
		/** Number of bytes in the digest. */
		static constexpr std::size_t size_bytes = Algorithm::digest_size;

		/** The underlying byte array type. */
		using array_type = std::array<std::uint8_t, size_bytes>;

		/** Constructs an all-zero digest. */
		constexpr digest() noexcept = default;

		/** Constructs a digest holding @p bytes. */
		constexpr explicit digest(const array_type& bytes) noexcept: m_bytes(bytes)
		{}

		/** Returns the digest bytes. */
		[[nodiscard]] constexpr std::span<const std::uint8_t, size_bytes> bytes() const noexcept
		{
			return m_bytes;
		}

		/** Returns the number of bytes in the digest. */
		[[nodiscard]] static constexpr std::size_t size() noexcept
		{
			return size_bytes;
		}

		/** Returns the byte at @p index, which must be less than @c size(). */
		[[nodiscard]] constexpr std::uint8_t operator[](std::size_t index) const
		{
			return m_bytes[index];
		}

		/** Returns an iterator to the first byte. */
		[[nodiscard]] constexpr auto begin() const noexcept
		{
			return m_bytes.begin();
		}

		/** Returns an iterator past the last byte. */
		[[nodiscard]] constexpr auto end() const noexcept
		{
			return m_bytes.end();
		}

		/** Returns the name of the producing algorithm, e.g. @c "SHA-256". */
		[[nodiscard]] static constexpr std::string_view algorithm_name() noexcept
		{
			return Algorithm::name;
		}

		/**
		 * @brief Returns the digest as lowercase hexadecimal.
		 *
		 * Fixed size and not null-terminated, so the result is usable inside a
		 * constant expression where a @c std::string could not escape. Use
		 * @c to_string() for an owning, printable form.
		 */
		[[nodiscard]] constexpr std::array<char, size_bytes * 2> to_hex() const noexcept
		{
			constexpr std::string_view digits = "0123456789abcdef";

			std::array<char, size_bytes * 2> text{};
			for(std::size_t i = 0; i < size_bytes; ++i)
			{
				text[i * 2]     = digits[m_bytes[i] >> 4];
				text[i * 2 + 1] = digits[m_bytes[i] & 0x0F];
			}
			return text;
		}

		/** Returns the digest as a lowercase hexadecimal string. */
		[[nodiscard]] std::string to_string() const
		{
			const auto text = to_hex();
			return std::string(text.begin(), text.end());
		}

		/**
		 * @brief Parses a lowercase or uppercase hexadecimal digest.
		 *
		 * @param text Exactly @c 2 * size() hexadecimal characters.
		 *
		 * @return The parsed digest, or the reason @p text was rejected.
		 */
		[[nodiscard]] static constexpr std::expected<digest, hex_error> from_hex(std::string_view text) noexcept
		{
			if(text.size() != size_bytes * 2)
			{
				return std::unexpected(hex_error::invalid_length);
			}

			digest result{};
			for(std::size_t i = 0; i < size_bytes; ++i)
			{
				const auto high = detail::decode_nibble(text[i * 2]);
				const auto low  = detail::decode_nibble(text[i * 2 + 1]);
				if(!high.has_value() || !low.has_value())
				{
					return std::unexpected(hex_error::invalid_character);
				}
				result.m_bytes[i] = static_cast<std::uint8_t>((*high << 4) | *low);
			}
			return result;
		}

		/** Digests of the same algorithm compare bytewise. */
		[[nodiscard]] friend constexpr bool operator==(const digest&, const digest&) noexcept = default;

		/** Digests of the same algorithm order lexicographically by byte. */
		[[nodiscard]] friend constexpr auto operator<=>(const digest&, const digest&) noexcept = default;

	private:
		array_type m_bytes{};
	};

} // namespace alt

namespace std
{

	/**
	 * @brief Hash support so a digest can key an unordered container.
	 *
	 * A digest is already uniformly distributed, so its leading bytes are used
	 * directly rather than being hashed again.
	 */
	template<alt::detail::digest_traits Algorithm>
	struct hash<alt::digest<Algorithm>>
	{
		[[nodiscard]] std::size_t operator()(const alt::digest<Algorithm>& value) const noexcept
		{
			constexpr std::size_t used = std::min(sizeof(std::size_t), alt::digest<Algorithm>::size_bytes);

			std::size_t result = 0;
			for(std::size_t i = 0; i < used; ++i)
			{
				result = (result << 8) | value[i];
			}
			return result;
		}
	};

	/** Formats a digest as lowercase hexadecimal. Accepts no format specification. */
	template<alt::detail::digest_traits Algorithm>
	struct formatter<alt::digest<Algorithm>, char>
	{
		constexpr auto parse(std::format_parse_context& context)
		{
			if(context.begin() != context.end() && *context.begin() != '}')
			{
				throw std::format_error("alt::digest supports no format specification");
			}
			return context.begin();
		}

		auto format(const alt::digest<Algorithm>& value, std::format_context& context) const
		{
			const auto text = value.to_hex();
			return std::copy(text.begin(), text.end(), context.out());
		}
	};

} // namespace std
