#pragma once

#include <alt/concepts.hpp>
#include <alt/detail/base64.hpp>
#include <cstddef>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alt
{

	/**
	 * @brief Constrains a type to a usable base64 alphabet.
	 *
	 * A model supplies three static members: a human-readable @c name, exactly 64
	 * @c characters mapping sextet values 0 through 63 to output in order, and a
	 * @c padding character. Padding must not also be an alphabet character, or a
	 * decoder could not tell the end of the data from its content. Use
	 * @c base64_no_padding for an alphabet that omits padding, which RFC 4648
	 * section 3.2 permits only where the length is known implicitly.
	 *
	 * Satisfied by @c base64_standard_alphabet, @c base64_url_alphabet,
	 * @c base64_url_unpadded_alphabet, and any user-defined alphabet meeting the
	 * same requirements.
	 */
	template<typename T>
	concept base64_alphabet = detail::base64_alphabet<T>;

	/**
	 * @brief Constrains a type to a decode error-reporting policy.
	 *
	 * Satisfied by @c base64_expected_errors and @c base64_throw_errors.
	 */
	template<typename T>
	concept base64_error_policy = detail::base64_error_policy<T>;

	/** The value of @c padding that marks an alphabet as emitting no padding. */
	inline constexpr char base64_no_padding = detail::base64_no_padding;

	/**
	 * @brief The base 64 alphabet of RFC 4648 section 4, table 1.
	 *
	 * The default everywhere in this header. Its 62nd and 63rd characters are
	 * @c '+' and @c '/', which RFC 4648 section 3.4 notes are problematic in URLs,
	 * in file names, and to legacy text indexers. Prefer @c base64_url_alphabet
	 * where the output travels through any of those.
	 */
	struct base64_standard_alphabet
	{
		/** Human-readable alphabet name. */
		static constexpr std::string_view name = "base64";

		/** Sextet values 0 through 63 in order, RFC 4648 table 1. */
		static constexpr std::string_view characters =
		  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		/** The pad character, RFC 4648 section 4. */
		static constexpr char padding = '=';
	};

	/**
	 * @brief The URL and filename safe alphabet of RFC 4648 section 5, table 2.
	 *
	 * Technically identical to the standard alphabet but for the 62nd and 63rd
	 * characters, which are @c '-' and @c '_'. RFC 4648 section 5 asks that this
	 * encoding be called "base64url" and not simply "base64", so @c name says so.
	 */
	struct base64_url_alphabet
	{
		/** Human-readable alphabet name. */
		static constexpr std::string_view name = "base64url";

		/** Sextet values 0 through 63 in order, RFC 4648 table 2. */
		static constexpr std::string_view characters =
		  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

		/** The pad character, RFC 4648 section 4. */
		static constexpr char padding = '=';
	};

	/**
	 * @brief The URL and filename safe alphabet with padding omitted.
	 *
	 * RFC 4648 section 5 observes that @c '=' is normally percent-encoded inside a
	 * URI, and that the padding may be skipped where the data length is known
	 * implicitly. This alphabet is that variant, the one used by JSON Web
	 * Signature and its relatives. Encoding emits no padding and decoding rejects
	 * it as an invalid character.
	 */
	struct base64_url_unpadded_alphabet
	{
		/** Human-readable alphabet name. */
		static constexpr std::string_view name = "base64url-unpadded";

		/** Sextet values 0 through 63 in order, RFC 4648 table 2. */
		static constexpr std::string_view characters = base64_url_alphabet::characters;

		/** No padding is emitted or accepted. */
		static constexpr char padding = base64_no_padding;
	};

	/**
	 * @brief Returns the number of characters @p byte_count bytes encode to.
	 *
	 * Exact, not an upper bound, so it is the right size to reserve.
	 *
	 * @tparam Alphabet Selects whether the final quantum is padded out.
	 */
	template<base64_alphabet Alphabet = base64_standard_alphabet>
	[[nodiscard]] constexpr std::size_t base64_encoded_size(std::size_t byte_count) noexcept
	{
		constexpr std::size_t bytes_per = detail::base64_bytes_per_quantum;
		constexpr std::size_t chars_per = detail::base64_chars_per_quantum;

		if constexpr(detail::base64_pads<Alphabet>)
		{
			return ((byte_count + bytes_per - 1) / bytes_per) * chars_per;
		}
		else
		{
			const std::size_t remainder = byte_count % bytes_per;
			return ((byte_count / bytes_per) * chars_per) + (remainder == 0 ? 0 : remainder + 1);
		}
	}

	namespace ranges
	{
		namespace views
		{

			/**
			 * @brief Pipeable range-adaptor closure produced by @c base64_encode().
			 *
			 * Deriving @c std::ranges::range_adaptor_closure provides the pipe operator
			 * for both `range | base64_encode<...>()` and composition with other adaptors.
			 */
			template<base64_alphabet Alphabet>
			struct base64_encode_closure:
			  std::ranges::range_adaptor_closure<base64_encode_closure<Alphabet>>
			{
				/** Builds the encoding view over @p r. */
				template<std::ranges::viewable_range R>
				  requires byte_range<R>
				constexpr auto operator()(R&& r) const
				{
					return detail::base64_encode_view<std::views::all_t<R>, Alphabet>{
					  std::views::all(std::forward<R>(r))};
				}
			};

			/**
			 * @brief Creates an adaptor encoding a range of bytes to base64 characters.
			 *
			 * Encoding cannot fail, so unlike decoding this adaptor takes no error policy.
			 *
			 * @tparam Alphabet The alphabet to encode with. Defaults to RFC 4648 section 4.
			 */
			template<base64_alphabet Alphabet = base64_standard_alphabet>
			[[nodiscard]] constexpr auto base64_encode()
			{
				return base64_encode_closure<Alphabet>{};
			}

			/**
			 * @brief Pipeable range-adaptor closure produced by @c base64_decode().
			 */
			template<base64_alphabet Alphabet, base64_error_policy Policy>
			struct base64_decode_closure:
			  std::ranges::range_adaptor_closure<base64_decode_closure<Alphabet, Policy>>
			{
				/** Builds the decoding view over @p r. */
				template<std::ranges::viewable_range R>
				  requires std::convertible_to<std::ranges::range_value_t<R>, char>
				constexpr auto operator()(R&& r) const
				{
					return detail::base64_decode_view<std::views::all_t<R>, Alphabet, Policy>{
					  std::views::all(std::forward<R>(r))};
				}
			};

			/**
			 * @brief Creates an adaptor decoding a range of base64 characters to bytes.
			 *
			 * @tparam Alphabet The alphabet the input is expected to use. Defaults to
			 *                  RFC 4648 section 4.
			 * @tparam Policy   How a malformed input is reported. Defaults to yielding
			 *                  @c std::expected elements rather than throwing.
			 */
			template<base64_alphabet     Alphabet = base64_standard_alphabet,
			         base64_error_policy Policy   = base64_expected_errors>
			[[nodiscard]] constexpr auto base64_decode()
			{
				return base64_decode_closure<Alphabet, Policy>{};
			}

		} // namespace views

		using views::base64_decode;
		using views::base64_encode;

	} // namespace ranges

	namespace views = ranges::views;

	namespace detail
	{

		/**
		 * @brief Collects the base64 encoding of @p input into a string.
		 *
		 * Shared by both @c base64_encode overloads. They cannot delegate to each
		 * other: @c std::views::all of a @c std::string_view is a @c std::string_view,
		 * so the string_view overload would forever reselect itself.
		 */
		template<base64_alphabet Alphabet, std::ranges::viewable_range R>
		constexpr std::string base64_encode_string(R&& input)
		{
			std::string out;
			if constexpr(std::ranges::sized_range<R>)
			{
				out.reserve(base64_encoded_size<Alphabet>(std::ranges::size(input)));
			}

			for(const char c: std::forward<R>(input) | views::base64_encode<Alphabet>())
			{
				out.push_back(c);
			}
			return out;
		}

	} // namespace detail

	/**
	 * @brief Returns the bytes that the base64 text @p input encodes.
	 *
	 * Decoding is strict, as RFC 4648 section 3.3 requires by default: any
	 * character outside the alphabet is rejected, whitespace and line breaks
	 * included. A caller decoding MIME, which wraps lines at 76 characters, should
	 * filter the breaks out first, for which the decode view composes directly:
	 *
	 * @code
	 * auto stripped = text | std::views::filter([](char c) { return c != '\n' && c != '\r'; });
	 * auto bytes    = stripped | alt::views::base64_decode();
	 * @endcode
	 *
	 * Encodings whose final quantum leaves non-zero discarded bits are rejected as
	 * well, which RFC 4648 section 3.5 permits and which keeps the encoding
	 * canonical, so distinct texts cannot decode to identical bytes.
	 *
	 * @tparam Alphabet The alphabet the input is expected to use.
	 *
	 * @param input Base64 text.
	 *
	 * @return The decoded bytes, or the reason @p input was rejected.
	 */
	template<base64_alphabet Alphabet = base64_standard_alphabet>
	[[nodiscard]] constexpr std::expected<std::vector<std::byte>, base64_error> base64_decode(std::string_view input)
	{
		std::vector<std::byte> out;
		out.reserve((input.size() / detail::base64_chars_per_quantum + 1) * detail::base64_bytes_per_quantum);

		for(const auto& byte: input | views::base64_decode<Alphabet>())
		{
			if(!byte.has_value())
			{
				return std::unexpected(byte.error());
			}
			out.push_back(*byte);
		}
		return out;
	}

	/**
	 * @brief Returns the base64 encoding of @p input.
	 *
	 * @tparam Alphabet The alphabet to encode with. Defaults to RFC 4648 section 4.
	 *
	 * @param input Any range of bytes, contiguous or lazy.
	 */
	template<base64_alphabet Alphabet = base64_standard_alphabet>
	[[nodiscard]] constexpr std::string base64_encode(byte_range auto&& input)
	{
		return detail::base64_encode_string<Alphabet>(std::forward<decltype(input)>(input));
	}

	/**
	 * @brief Returns the base64 encoding of the characters of @p input.
	 *
	 * This overload is what a string literal binds to, and it encodes exactly the
	 * characters of the string, excluding the terminating NUL. Binary data held in
	 * a @c char buffer must be passed as a @c std::span instead: converting it to a
	 * @c std::string_view would stop at the first NUL.
	 */
	template<base64_alphabet Alphabet = base64_standard_alphabet>
	[[nodiscard]] constexpr std::string base64_encode(std::string_view input)
	{
		return detail::base64_encode_string<Alphabet>(input);
	}

} // namespace alt
