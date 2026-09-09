#pragma once

#include <cstddef>
#include <utility>

namespace alt
{

	namespace detail
	{

		/**
		 * @brief Implementation of the alt::key projection object.
		 *
		 * Kept in detail so that the public name is an object rather than a type,
		 * matching how the standard library spells its niebloids.
		 */
		struct key_fn
		{
			/**
			 * @brief Returns the @c first member of @p pair_like.
			 *
			 * The return type is deduced with @c decltype(auto), so constness and value
			 * category both survive the projection: projecting a const lvalue yields a
			 * const lvalue reference, and projecting an rvalue yields an rvalue reference.
			 */
			template<typename PairLike>
			  requires requires(PairLike&& p) { std::forward<PairLike>(p).first; }
			[[nodiscard]] constexpr decltype(auto) operator()(PairLike&& pair_like) const noexcept
			{
				// The parentheses matter: decltype(auto) over an unparenthesized member
				// access would deduce the member's declared type and copy it, discarding
				// the reference the caller needs.
				return (std::forward<PairLike>(pair_like).first);
			}
		};

		/**
		 * @brief Implementation of the alt::value projection object.
		 */
		struct value_fn
		{
			/**
			 * @brief Returns the @c second member of @p pair_like.
			 *
			 * Preserves constness and value category, as described for key_fn.
			 */
			template<typename PairLike>
			  requires requires(PairLike&& p) { std::forward<PairLike>(p).second; }
			[[nodiscard]] constexpr decltype(auto) operator()(PairLike&& pair_like) const noexcept
			{
				// The parentheses matter: decltype(auto) over an unparenthesized member
				// access would deduce the member's declared type and copy it, discarding
				// the reference the caller needs.
				return (std::forward<PairLike>(pair_like).second);
			}
		};

		// Poison pill: gives the unqualified get<N> calls below a name to bind to, while
		// remaining non-viable itself (it takes no arguments), so that every real candidate
		// arrives through argument-dependent lookup. The standard tuple-like types are
		// found this way too, since std::get lives in the same namespace as they do.
		template<std::size_t>
		void get() = delete;

		/**
		 * @brief Implementation of the alt::element projection object.
		 *
		 * @tparam N Zero-based index of the element to project to.
		 */
		template<std::size_t N>
		struct element_fn
		{
			/**
			 * @brief Returns the Nth element of @p tuple_like.
			 *
			 * Calls @c get<N> unqualified, so the projection works both for the standard
			 * tuple-like types and for user types that opt in by providing a @c get()
			 * reachable through argument-dependent lookup.
			 *
			 * Preserves constness and value category, as described for key_fn.
			 */
			template<typename TupleLike>
			  requires requires(TupleLike&& t) { get<N>(std::forward<TupleLike>(t)); }
			[[nodiscard]] constexpr decltype(auto) operator()(TupleLike&& tuple_like) const
			  noexcept(noexcept(get<N>(std::forward<TupleLike>(tuple_like))))
			{
				return get<N>(std::forward<TupleLike>(tuple_like));
			}
		};

	} // namespace detail

	// ---------------------------------------------------------------------------
	// Projection objects
	// ---------------------------------------------------------------------------

	/**
	 * @brief Projection that selects the @c first member of a pair-like element.
	 *
	 * Intended for the key half of an associative container's elements, and usable
	 * anywhere a range projection is accepted.
	 *
	 * @code
	 *   std::map<int, std::string> m{{1, "one"}, {2, "two"}};
	 *   alt::erase(m, 2, alt::key);
	 *   auto keys = m | std::views::transform(alt::key);
	 * @endcode
	 */
	inline constexpr detail::key_fn key{};

	/**
	 * @brief Projection that selects the @c second member of a pair-like element.
	 *
	 * Intended for the mapped half of an associative container's elements, and usable
	 * anywhere a range projection is accepted.
	 *
	 * @code
	 *   std::map<int, std::string> m{{1, "one"}, {2, "two"}};
	 *   alt::erase(m, std::string{"two"}, alt::value);
	 * @endcode
	 */
	inline constexpr detail::value_fn value{};

	/**
	 * @brief Projection that selects the Nth element of a tuple-like value.
	 *
	 * Works with @c std::tuple, @c std::pair, @c std::array, and any type that opts in
	 * by providing a @c get() reachable through argument-dependent lookup.
	 *
	 * @code
	 *   std::vector<std::tuple<int, char>> rows{{1, 'a'}, {2, 'b'}};
	 *   auto it = std::ranges::find(rows, 'b', alt::element<1>);
	 * @endcode
	 *
	 * @tparam N Zero-based index of the element to project to.
	 */
	template<std::size_t N>
	inline constexpr detail::element_fn<N> element{};

} // namespace alt
