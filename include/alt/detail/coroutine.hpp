#pragma once

#include <cassert>
#include <exception>
#include <memory>
#include <utility>
#include <variant>

namespace alt::detail
{

	/**
	 * @brief Holds the outcome of a coroutine: a value or an exception.
	 *
	 * Access through value() rethrows a stored exception, so a caller never has to
	 * ask which channel is occupied. The alternatives are addressed by index
	 * rather than by type, so a store whose value type is itself
	 * std::exception_ptr keeps its two channels distinct.
	 *
	 * Specializations exist for void and for reference types, which is why
	 * alt::task needs no specialization of its own.
	 */
	template<typename T>
	class result_store
	{
	public:
		/** Stores a value, replacing whatever was held. */
		template<typename U>
		void set_value(U&& value)
		{
			m_state.template emplace<1>(std::forward<U>(value));
		}

		/** Stores an exception to be rethrown by value(). */
		// NOLINTNEXTLINE(bugprone-exception-escape): the only statement in the body moves a std::exception_ptr into the variant, which is itself noexcept.
		void set_exception(std::exception_ptr error) noexcept
		{
			// Moving a std::exception_ptr is itself noexcept, so emplace<2> cannot throw
			m_state.template emplace<2>(std::move(error));
		}

		/** True once either channel has been filled. */
		[[nodiscard]] bool is_set() const noexcept
		{
			return m_state.index() != 0;
		}

		/** The stored value, or a rethrow of the stored exception. */
		[[nodiscard]] T& value() &
		{
			rethrow_if_failed();
			return std::get<1>(m_state);
		}

		/** The stored value moved out, or a rethrow of the stored exception. */
		[[nodiscard]] T&& value() &&
		{
			rethrow_if_failed();
			return std::get<1>(std::move(m_state));
		}

		/** The stored value, or a rethrow of the stored exception. */
		[[nodiscard]] const T& value() const&
		{
			rethrow_if_failed();
			return std::get<1>(m_state);
		}

	private:
		void rethrow_if_failed() const
		{
			assert(is_set() && "reading an alt coroutine result before it was produced");
			if(m_state.index() == 2)
			{
				std::rethrow_exception(std::get<2>(m_state));
			}
		}

		std::variant<std::monostate, T, std::exception_ptr> m_state;
	};

	/** @brief result_store for a coroutine that produces no value. */
	template<>
	class result_store<void>
	{
	public:
		/** Records successful completion. */
		void set_value() noexcept
		{
			m_completed = true;
		}

		/** Stores an exception to be rethrown by value(). */
		void set_exception(std::exception_ptr error) noexcept
		{
			m_error     = std::move(error);
			m_completed = true;
		}

		/** True once completion has been recorded, successfully or not. */
		[[nodiscard]] bool is_set() const noexcept
		{
			return m_completed;
		}

		/** Returns nothing, or rethrows the stored exception. */
		void value() const
		{
			assert(m_completed && "reading an alt coroutine result before it was produced");
			if(m_error)
			{
				std::rethrow_exception(m_error);
			}
		}

	private:
		std::exception_ptr m_error;
		bool               m_completed = false;
	};

	/** @brief result_store for a coroutine that produces a reference. */
	template<typename T>
	class result_store<T&>
	{
	public:
		/** Stores a reference to an object the caller keeps alive. */
		void set_value(T& value) noexcept
		{
			m_value = std::addressof(value);
		}

		/** Stores an exception to be rethrown by value(). */
		void set_exception(std::exception_ptr error) noexcept
		{
			m_error = std::move(error);
		}

		/** True once either channel has been filled. */
		[[nodiscard]] bool is_set() const noexcept
		{
			return m_value != nullptr || m_error != nullptr;
		}

		/** The referenced object, or a rethrow of the stored exception. */
		[[nodiscard]] T& value() const
		{
			assert(is_set() && "reading an alt coroutine result before it was produced");
			if(m_error)
			{
				std::rethrow_exception(m_error);
			}
			return *m_value;
		}

	private:
		T*                 m_value = nullptr;
		std::exception_ptr m_error;
	};

}
