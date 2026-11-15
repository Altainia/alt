#pragma once

#include <alt/awaitable.hpp>
#include <alt/detail/coroutine.hpp>
#include <cassert>
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace alt::detail
{

	/**
	 * How a child's await result is stored and handed back.
	 *
	 * The primary template strips references and cv-qualifiers, which is the
	 * decision that matters most: awaiting an alt::task rvalue yields T&&, and that
	 * must become a T moved out of the dying child frame, not a reference into it.
	 * The slots live off the frame for exactly this reason.
	 */
	template<typename R>
	struct combinator_value
	{
		using type = std::remove_cvref_t<R>;
	};

	/**
	 * A child that yields nothing contributes std::monostate, and its slot is a
	 * result_store<std::monostate> rather than a result_store<void>. Every slot
	 * then has a value() that returns something, so assembly is one expression per
	 * position with no if constexpr.
	 */
	template<>
	struct combinator_value<void>
	{
		using type = std::monostate;
	};

	/**
	 * An lvalue reference becomes a std::reference_wrapper, following
	 * detail::completion_value in task_completion_source.hpp: std::expected<T&, E>
	 * is ill-formed (libstdc++ rejects it with a static_assert that the value type
	 * is not a reference), and the assembled results travel inside a std::expected.
	 * The referenced object must outlive the await. Matches lvalue references only;
	 * T&& is handled by the primary template.
	 */
	template<typename T>
	struct combinator_value<T&>
	{
		using type = std::reference_wrapper<T>;
	};

	/** The value type that a result of type R is stored and returned as. */
	template<typename R>
	using combinator_value_t = typename combinator_value<R>::type;

	/** The value type position i of a when_all over A contributes. */
	template<alt::awaitable A>
	using when_all_value_t = combinator_value_t<alt::await_result_t<A>>;

	/** The slot position i writes into. */
	template<alt::awaitable A>
	using when_all_slot_t = result_store<when_all_value_t<A>>;

	/** Satisfied by a std::tuple specialization. */
	template<typename T>
	concept tuple_like_slots = requires { std::tuple_size<T>::value; };

	/**
	 * Builds the slot container. A tuple ignores n beyond a debug check that it
	 * matches the tuple's size; a vector is sized by it.
	 */
	template<typename Slots>
	[[nodiscard]] Slots make_combinator_slots([[maybe_unused]] const std::size_t n)
	{
		if constexpr(tuple_like_slots<Slots>)
		{
			assert(n == std::tuple_size_v<Slots> && "slot count must match the tuple");
			return Slots{};
		}
		else
		{
			return Slots(n);
		}
	}

	/**
	 * Moves every slot's value into Results, rethrowing the lowest-indexed stored
	 * exception.
	 *
	 * The tuple form is a braced-init-list, whose elements are evaluated strictly
	 * left to right; that is what makes "the lowest-indexed throwing child is the
	 * one rethrown" true rather than incidental. The vector form visits in index
	 * order for the same reason.
	 */
	template<typename Results, typename Slots>
	[[nodiscard]] Results assemble_combinator_results(Slots&& slots)
	{
		if constexpr(tuple_like_slots<std::remove_cvref_t<Slots>>)
		{
			return [&]<std::size_t... I>(std::index_sequence<I...>) {
				return Results{std::move(std::get<I>(slots)).value()...};
			}(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<Slots>>>{});
		}
		else
		{
			Results results;
			results.reserve(slots.size());
			for(auto& slot: slots)
			{
				results.push_back(std::move(slot).value());
			}
			return results;
		}
	}

}
