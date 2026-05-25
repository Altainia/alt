#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <iterator>

namespace alt
{

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
	 * Implemented via the erase-remove idiom: @c std::remove_if moves matching
	 * elements to the end of the range, then @c Container::erase excises them.
	 *
	 * @tparam Container A sequence container that provides @c begin(), @c end(),
	 *                   @c erase(iterator, iterator), and a @c size_type typedef.
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
	  requires std::invocable<Proj&, const typename Container::value_type&> && std::equality_comparable_with<
	                                                                             std::invoke_result_t<Proj&, const typename Container::value_type&>,
	                                                                             Value>
	constexpr auto erase(Container& c, const Value& value, Proj proj = {}) ->
	  typename Container::size_type
	{
		auto const it = std::remove_if(c.begin(), c.end(), [&](const typename Container::value_type& elem) {
			return std::invoke(proj, elem) == value;
		});
		auto const count =
		  static_cast<typename Container::size_type>(std::distance(it, c.end()));
		c.erase(it, c.end());
		return count;
	}

} // namespace alt
