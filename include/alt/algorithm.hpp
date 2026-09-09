#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>

namespace alt
{

	namespace detail
	{

		// ---------------------------------------------------------------------------
		// erase / erase_if helpers
		// ---------------------------------------------------------------------------

		/**
		 * @brief Containers that support the erase-remove idiom.
		 *
		 * Requires permutable iterators, so that std::remove_if may shuffle the surviving
		 * elements forward, and a range erase to excise the tail. Satisfied by the sequence
		 * containers: vector, deque, string, and list.
		 */
		template<typename Container>
		concept erase_remove_capable =
		  std::permutable<typename Container::iterator> &&
		  requires(Container& c) { c.erase(c.begin(), c.end()); };

		/**
		 * @brief Containers that support erasing one element at a time by iterator.
		 *
		 * Erasing must return the iterator following the erased element, so that iteration
		 * can continue. Satisfied by the associative and unordered containers, whose
		 * elements cannot be permuted, as well as by the sequence containers.
		 */
		template<typename Container>
		concept node_erase_capable = requires(Container& c) {
			{ c.erase(c.begin()) } -> std::same_as<typename Container::iterator>;
		};

		/**
		 * @brief Containers that alt::erase and alt::erase_if can remove elements from.
		 *
		 * Excludes std::forward_list, which offers neither erase form: it splices with
		 * erase_after instead.
		 */
		template<typename Container>
		concept element_erasable =
		  requires { typename Container::size_type; } &&
		  (erase_remove_capable<Container> || node_erase_capable<Container>);

		/**
		 * @brief Erases every element of @p c for which @p pred(element) is true.
		 *
		 * The single place where the two removal strategies live. Containers with
		 * permutable iterators take the erase-remove idiom, which touches each element
		 * once. The rest are erased node by node, since their elements cannot be moved
		 * within the container.
		 *
		 * @return The number of elements removed.
		 */
		template<typename Container, typename Pred>
		  requires element_erasable<Container>
		constexpr auto erase_elements_if(Container& c, Pred pred) -> typename Container::size_type
		{
			using size_type = typename Container::size_type;

			if constexpr(erase_remove_capable<Container>)
			{
				const auto first = std::remove_if(c.begin(), c.end(), pred);
				const auto count = static_cast<size_type>(std::distance(first, c.end()));
				c.erase(first, c.end());
				return count;
			}
			else
			{
				size_type count = 0;
				for(auto it = c.begin(); it != c.end();)
				{
					if(pred(*it))
					{
						it = c.erase(it);
						++count;
					}
					else
					{
						++it;
					}
				}
				return count;
			}
		}

		/** @brief The type a projection yields when applied to a container's elements. */
		template<typename Container, typename Proj>
		using projected_element_t =
		  std::invoke_result_t<Proj&, const typename Container::value_type&>;

	} // namespace detail

	// ---------------------------------------------------------------------------
	// erase
	// ---------------------------------------------------------------------------

	/**
	 * @brief Erases all elements from @p c for which @p proj(element) == @p value.
	 *
	 * Analogous to C++20's @c std::erase, but accepts a projection callable that
	 * is applied to each element before the equality comparison, mirroring the
	 * projection parameter found in the C++ ranges algorithms.
	 *
	 * Works with sequence containers such as @c std::vector, @c std::deque,
	 * @c std::string and @c std::list, and with the associative and unordered
	 * containers such as @c std::map, @c std::multimap, @c std::set and
	 * @c std::unordered_map. The removal strategy is chosen automatically: see
	 * @c detail::erase_elements_if. For map-like containers the projection is what
	 * selects the interesting half of each element, so pair with @c alt::key or
	 * @c alt::value from @c <alt/projection.hpp>.
	 *
	 * @tparam Container A container satisfying @c detail::element_erasable.
	 * @tparam Value     The type to compare projected elements against.
	 * @tparam Proj      A callable projection applied to each element before
	 *                   comparison. Defaults to @c std::identity (no projection).
	 *
	 * @param c     The container to modify in-place.
	 * @param value The value to compare projected elements against.
	 * @param proj  Projection applied to each element before comparison.
	 *
	 * @return The number of elements removed.
	 */
	template<typename Container, typename Value, typename Proj = std::identity>
	  requires detail::element_erasable<Container> &&
	           std::invocable<Proj&, const typename Container::value_type&> &&
	           std::equality_comparable_with<detail::projected_element_t<Container, Proj>, Value>
	constexpr auto erase(Container& c, const Value& value, Proj proj = {}) ->
	  typename Container::size_type
	{
		return detail::erase_elements_if(
		  c, [&](const typename Container::value_type& elem) { return std::invoke(proj, elem) == value; });
	}

	// ---------------------------------------------------------------------------
	// erase_if
	// ---------------------------------------------------------------------------

	/**
	 * @brief Erases all elements from @p c for which @p pred(proj(element)) is true.
	 *
	 * Analogous to C++20's @c std::erase_if, but accepts a projection callable that is
	 * applied to each element before the predicate, mirroring the projection parameter
	 * found in the C++ ranges algorithms. The predicate therefore sees the projected
	 * value, not the element.
	 *
	 * Supports the same containers as @c alt::erase.
	 *
	 * @tparam Container A container satisfying @c detail::element_erasable.
	 * @tparam Pred      A predicate applied to each projected element.
	 * @tparam Proj      A callable projection applied to each element before the
	 *                   predicate. Defaults to @c std::identity (no projection).
	 *
	 * @param c    The container to modify in-place.
	 * @param pred Predicate deciding whether a projected element is removed.
	 * @param proj Projection applied to each element before the predicate.
	 *
	 * @return The number of elements removed.
	 */
	template<typename Container, typename Pred, typename Proj = std::identity>
	  requires detail::element_erasable<Container> &&
	           std::invocable<Proj&, const typename Container::value_type&> &&
	           std::predicate<Pred&, detail::projected_element_t<Container, Proj>>
	constexpr auto erase_if(Container& c, Pred pred, Proj proj = {}) ->
	  typename Container::size_type
	{
		return detail::erase_elements_if(c, [&](const typename Container::value_type& elem) {
			return static_cast<bool>(std::invoke(pred, std::invoke(proj, elem)));
		});
	}

} // namespace alt
