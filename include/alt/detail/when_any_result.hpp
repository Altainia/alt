#pragma once

#include <alt/awaitable.hpp>
#include <alt/detail/combinator_results.hpp>
#include <cassert>
#include <cstddef>
#include <utility>
#include <variant>

namespace alt::detail
{
	/**
	 * The value type that position i of a when_any over A contributes.
	 *
	 * The same mapping when_all uses: void becomes std::monostate and an lvalue
	 * reference becomes a std::reference_wrapper, constness kept.
	 */
	template<alt::awaitable A>
	using when_any_value_t = detail::combinator_value_t<alt::await_result_t<A>>;

} // namespace alt::detail

namespace alt
{

	/**
	 * The result of an argument-list when_any: which position completed first, and
	 * the value it produced.
	 *
	 * Storage is a std::variant over the children's value types. Duplicate
	 * alternatives are legal and expected -- when_any(f(), g()) with both yielding int
	 * gives a variant<int, int> -- so the winner is read by position with get<I>(),
	 * never by type. Switch on index() and call get<I>() in each branch.
	 *
	 * No visitor is offered. It was decided, not forgotten: std::visit dispatches on
	 * the alternative's type, and with duplicate alternatives two positions are
	 * indistinguishable by type, so a visitor would invite exactly the ambiguity the
	 * indexed accessor exists to avoid.
	 *
	 * @tparam Vs The value type of each child, in argument order.
	 */
	template<typename... Vs>
	class when_any_result
	{
	public:
		/**
		 * Constructs the result for position I, holding a value built from args.
		 *
		 * @tparam I    The position that won.
		 * @tparam Args The types of the arguments the value is constructed from.
		 * @param args  The arguments the winning value is constructed from.
		 */
		template<std::size_t I, typename... Args>
		explicit when_any_result(std::in_place_index_t<I> position, Args&&... args): m_value(position, std::forward<Args>(args)...)
		{
		}

		/** The position of the child that completed first. */
		[[nodiscard]] std::size_t index() const noexcept
		{
			return m_value.index();
		}

		/**
		 * The winning value, read as position I.
		 *
		 * Asking for a position other than index() is a caller error: it asserts in a
		 * debug build, and throws std::bad_variant_access otherwise.
		 *
		 * @tparam I The position to read, which must equal index().
		 */
		template<std::size_t I>
		[[nodiscard]] decltype(auto) get() const&
		{
			assert(I == index() && "reading an alt when_any result for a position that did not win");
			return std::get<I>(m_value);
		}

		/**
		 * Takes the winning value out of an rvalue result, so a move-only winner such as
		 * a std::unique_ptr can be had: std::move(result).template get<0>(). The same
		 * position requirement as the const form applies.
		 *
		 * There is deliberately no non-const lvalue overload: reading is the const form
		 * and taking is this one, and no caller needs a mutable reference into the
		 * winner's storage.
		 *
		 * @tparam I The position to take, which must equal index().
		 */
		template<std::size_t I>
		[[nodiscard]] decltype(auto) get() &&
		{
			assert(I == index() && "reading an alt when_any result for a position that did not win");
			return std::get<I>(std::move(m_value));
		}

	private:
		/** The winner's value, held at the winner's position. */
		std::variant<Vs...> m_value;
	};

	/**
	 * The result of a range-form when_any: which element completed first, and the
	 * value it produced.
	 *
	 * Every element of a range has the same type, so there is one value type and the
	 * winner's position is a run-time number rather than a variant index. That is why
	 * this is a class of its own and not when_any_result over a single type, whose
	 * index() is fixed at 0 by construction.
	 *
	 * @tparam V The value type every element contributes.
	 */
	template<typename V>
	class when_any_range_result
	{
	public:
		/**
		 * @param winner The position in the range of the element that completed first.
		 * @param value  The value that element produced.
		 */
		when_any_range_result(std::size_t winner, V value): m_index(winner), m_value(std::move(value))
		{
		}

		/** The position in the range of the element that completed first. */
		[[nodiscard]] std::size_t index() const noexcept
		{
			return m_index;
		}

		/** The value the winning element produced. */
		[[nodiscard]] const V& value() const& noexcept
		{
			return m_value;
		}

		/**
		 * Takes the winning element's value out of an rvalue result, so a move-only value
		 * can be had: std::move(result).value(). As with when_any_result::get, there is
		 * no non-const lvalue overload.
		 */
		[[nodiscard]] V&& value() && noexcept
		{
			return std::move(m_value);
		}

	private:
		/** The winner's position in the range. */
		std::size_t m_index;
		/** The winner's value. */
		V m_value;
	};

} // namespace alt
