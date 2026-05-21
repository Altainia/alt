// Implementation of P0052r10 "Generic Scope Guard and RAII Wrapper for the Standard Library"
// See: https://wg21.link/p0052r10

#pragma once

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace alt
{

	namespace detail
	{

		// ---------------------------------------------------------------------------
		// Scope guard helpers
		// ---------------------------------------------------------------------------

		/**
		 * @brief Constructs EF from f: moves if rvalue + nothrow constructible, otherwise copies.
		 *
		 * Used by scope guard primary constructors to select the cheapest safe initialization.
		 */
		template<typename EF, typename EFP>
		EF make_ef(EFP&& f)
		{
			if constexpr(!std::is_lvalue_reference_v<EFP> && std::is_nothrow_constructible_v<EF, EFP>)
			{
				return EF(std::forward<EFP>(f));
			}
			else
			{
				return EF(f);
			}
		}

		/**
		 * @brief Constructs EF from an existing EF member: moves if nothrow, otherwise copies.
		 *
		 * Used by scope guard move constructors.
		 */
		template<typename EF>
		EF move_or_copy_ef(EF& ef)
		{
			if constexpr(std::is_nothrow_move_constructible_v<EF>)
			{
				return std::move(ef);
			}
			else
			{
				return ef;
			}
		}

		// ---------------------------------------------------------------------------
		// unique_resource helpers
		// ---------------------------------------------------------------------------

		/**
		 * @brief Unwraps a reference_wrapper for reference resource types, or returns the value.
		 *
		 * This implements the *RESOURCE* exposition-only macro from the spec:
		 * - resource.get() when R is a reference type (R1 is reference_wrapper)
		 * - resource       otherwise
		 */
		template<typename R, typename R1>
		[[nodiscard]] decltype(auto) get_resource_value(R1& r1) noexcept
		{
			if constexpr(std::is_reference_v<R>)
			{
				return r1.get();
			}
			else
			{
				return (r1);
			}
		}

		/**
		 * @brief Exception-safe initialization of unique_resource's resource member.
		 *
		 * Moves r into R1 if nothrow constructible; otherwise copies. If construction
		 * throws, calls d(r) before rethrowing to prevent a resource leak.
		 */
		template<typename R1, typename RR, typename DD>
		R1 init_ur_resource(RR&& r, DD& d)
		{
			if constexpr(std::is_nothrow_constructible_v<R1, RR>)
			{
				return R1(std::forward<RR>(r));
			}
			else
			{
				try
				{
					return R1(r);
				}
				catch(...)
				{
					d(r);
					throw;
				}
			}
		}

		/**
		 * @brief Exception-safe initialization of unique_resource's deleter member.
		 *
		 * DD_outer is the outer constructor's DD template parameter — used to determine
		 * whether to forward (move) or copy the deleter, matching the original value
		 * category of the argument. The resource member (already initialized) is passed
		 * so that RESOURCE can be evaluated in the cleanup path if construction throws.
		 */
		template<typename R, typename D, typename DDOuter, typename R1>
		D init_ur_deleter(R1& resource, std::remove_reference_t<DDOuter>& d)
		{
			if constexpr(std::is_nothrow_constructible_v<D, DDOuter>)
			{
				return D(std::forward<DDOuter>(d));
			}
			else
			{
				try
				{
					return D(d);
				}
				catch(...)
				{
					d(get_resource_value<R>(resource));
					throw;
				}
			}
		}

		/**
		 * @brief Move-or-copy a unique_resource resource member.
		 */
		template<typename R1>
		R1 move_or_copy_ur_resource(R1& r1)
		{
			if constexpr(std::is_nothrow_move_constructible_v<R1>)
			{
				return std::move(r1);
			}
			else
			{
				return r1;
			}
		}

		/**
		 * @brief Move-or-copy a unique_resource deleter member, with cleanup on failure.
		 *
		 * If the copy throws and resource was nothrow-moved (ownership transferred to the
		 * new object), the original rhs must clean up via its own deleter.
		 * execute_on_reset is passed by reference so it can be set to false (release) in
		 * that case.
		 */
		template<typename R, typename D, typename R1>
		D move_or_copy_ur_deleter(D& deleter, R1& rhs_resource, bool& rhs_execute_on_reset)
		{
			if constexpr(std::is_nothrow_move_constructible_v<D>)
			{
				return std::move(deleter);
			}
			else
			{
				try
				{
					return D(deleter);
				}
				catch(...)
				{
					if constexpr(std::is_nothrow_move_constructible_v<R1>)
					{
						if(rhs_execute_on_reset)
						{
							deleter(get_resource_value<R>(rhs_resource));
							rhs_execute_on_reset = false;
						}
					}
					throw;
				}
			}
		}

	} // namespace detail

	// ---------------------------------------------------------------------------
	// scope_exit
	// ---------------------------------------------------------------------------

	/**
	 * @brief Scope guard that calls its exit function unconditionally on destruction.
	 *
	 * Implements [scope.scope_guard] for the scope_exit variant (§7.5.2 of P0052r10).
	 * The exit function is called whenever the scope_exit object is destroyed, unless
	 * release() has been called.
	 *
	 * @tparam EF A function object type, lvalue reference to function, or lvalue
	 *            reference to function object type. Must be Destructible (if an
	 *            object type). Given an lvalue g of type remove_reference_t<EF>,
	 *            g() must be well-formed.
	 */
	template<typename EF>
	class scope_exit
	{
	public:
		/**
		 * @brief Constructs a scope_exit with the given exit function (nothrow path).
		 *
		 * Selected when construction of EF from EFP is guaranteed not to throw.
		 * No try/catch overhead needed.
		 */
		template<typename EFP>
		  requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_exit> &&
		           std::is_constructible_v<EF, EFP> &&
		           requires(std::remove_cvref_t<EFP> g) { g(); } &&
		           (std::is_nothrow_constructible_v<EF, EFP> ||
		            std::is_nothrow_constructible_v<EF, EFP&>))
		explicit scope_exit(EFP&& f) noexcept
		  : m_exit_function(detail::make_ef<EF>(std::forward<EFP>(f)))
		{}

		/**
		 * @brief Constructs a scope_exit with the given exit function (may-throw path).
		 *
		 * Selected when construction of EF from EFP may throw. If initialization
		 * throws, f() is called before rethrowing.
		 */
		template<typename EFP>
		  requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_exit> &&
		           std::is_constructible_v<EF, EFP> &&
		           requires(std::remove_cvref_t<EFP> g) { g(); } &&
		           !std::is_nothrow_constructible_v<EF, EFP> &&
		           !std::is_nothrow_constructible_v<EF, EFP&>)
		explicit scope_exit(EFP&& f)
		try: m_exit_function(detail::make_ef<EF>(std::forward<EFP>(f)))
		{}
		catch(...)
		{
			f();
			throw;
		}

		/**
		 * @brief Move constructor.
		 *
		 * Constraints: EF is nothrow-move-constructible or copy-constructible.
		 * Effects: Initializes exit_function by moving (if nothrow) or copying. On
		 * success, calls rhs.release(). Copying provides the strong exception guarantee:
		 * if it throws, rhs retains ownership.
		 */
		scope_exit(scope_exit&& rhs) noexcept(std::is_nothrow_move_constructible_v<EF> ||
		                                      std::is_nothrow_copy_constructible_v<EF>)
		  requires(std::is_nothrow_move_constructible_v<EF> || std::is_copy_constructible_v<EF>)
		  : m_exit_function(detail::move_or_copy_ef(rhs.m_exit_function)),
		    m_execute_on_destruction(rhs.m_execute_on_destruction),
		    m_uncaught_on_creation(rhs.m_uncaught_on_creation)
		{
			rhs.release();
		}

		scope_exit(const scope_exit&)            = delete;
		scope_exit& operator=(const scope_exit&) = delete;
		scope_exit& operator=(scope_exit&&)      = delete;

		/** @brief Calls the exit function if execute_on_destruction is true. */
		~scope_exit() noexcept
		{
			if(m_execute_on_destruction)
			{
				m_exit_function();
			}
		}

		/** @brief Disables the exit function call on destruction. */
		void release() noexcept
		{
			m_execute_on_destruction = false;
		}

	private:
		EF   m_exit_function;
		bool m_execute_on_destruction{true};
		int  m_uncaught_on_creation{std::uncaught_exceptions()};
	};

	template<typename EF>
	scope_exit(EF) -> scope_exit<EF>;

	// ---------------------------------------------------------------------------
	// scope_fail
	// ---------------------------------------------------------------------------

	/**
	 * @brief Scope guard that calls its exit function only when the scope exits via exception.
	 *
	 * Implements [scope.scope_guard] for the scope_fail variant (§7.5.2 of P0052r10).
	 * The exit function is called on destruction only when std::uncaught_exceptions()
	 * is greater than the count recorded at construction time, indicating an active
	 * exception is unwinding the stack.
	 *
	 * @tparam EF Same requirements as scope_exit.
	 */
	template<typename EF>
	class scope_fail
	{
	public:
		/**
		 * @brief Constructs a scope_fail with the given exit function (nothrow path).
		 */
		template<typename EFP>
		  requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_fail> &&
		           std::is_constructible_v<EF, EFP> &&
		           requires(std::remove_cvref_t<EFP> g) { g(); } &&
		           (std::is_nothrow_constructible_v<EF, EFP> ||
		            std::is_nothrow_constructible_v<EF, EFP&>))
		explicit scope_fail(EFP&& f) noexcept
		  : m_exit_function(detail::make_ef<EF>(std::forward<EFP>(f)))
		{}

		/**
		 * @brief Constructs a scope_fail with the given exit function (may-throw path).
		 *
		 * If initialization throws, f() is called before rethrowing.
		 */
		template<typename EFP>
		  requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_fail> &&
		           std::is_constructible_v<EF, EFP> &&
		           requires(std::remove_cvref_t<EFP> g) { g(); } &&
		           !std::is_nothrow_constructible_v<EF, EFP> &&
		           !std::is_nothrow_constructible_v<EF, EFP&>)
		explicit scope_fail(EFP&& f)
		try: m_exit_function(detail::make_ef<EF>(std::forward<EFP>(f)))
		{}
		catch(...)
		{
			f();
			throw;
		}

		/**
		 * @brief Move constructor. Constraints and effects identical to scope_exit.
		 */
		scope_fail(scope_fail&& rhs) noexcept(std::is_nothrow_move_constructible_v<EF> ||
		                                      std::is_nothrow_copy_constructible_v<EF>)
		  requires(std::is_nothrow_move_constructible_v<EF> || std::is_copy_constructible_v<EF>)
		  : m_exit_function(detail::move_or_copy_ef(rhs.m_exit_function)),
		    m_execute_on_destruction(rhs.m_execute_on_destruction),
		    m_uncaught_on_creation(rhs.m_uncaught_on_creation)
		{
			rhs.release();
		}

		scope_fail(const scope_fail&)            = delete;
		scope_fail& operator=(const scope_fail&) = delete;
		scope_fail& operator=(scope_fail&&)      = delete;

		/**
		 * @brief Calls the exit function if execute_on_destruction is true and an
		 *        exception is active (uncaught_exceptions() > value at construction).
		 */
		~scope_fail() noexcept
		{
			if(m_execute_on_destruction && std::uncaught_exceptions() > m_uncaught_on_creation)
			{
				m_exit_function();
			}
		}

		/** @brief Disables the exit function call on destruction. */
		void release() noexcept
		{
			m_execute_on_destruction = false;
		}

	private:
		EF   m_exit_function;
		bool m_execute_on_destruction{true};
		int  m_uncaught_on_creation{std::uncaught_exceptions()};
	};

	template<typename EF>
	scope_fail(EF) -> scope_fail<EF>;

	// ---------------------------------------------------------------------------
	// scope_success
	// ---------------------------------------------------------------------------

	/**
	 * @brief Scope guard that calls its exit function only when the scope exits normally.
	 *
	 * Implements [scope.scope_guard] for the scope_success variant (§7.5.2 of P0052r10).
	 * The exit function is called on destruction only when std::uncaught_exceptions()
	 * is less than or equal to the count recorded at construction, indicating no new
	 * exception is active.
	 *
	 * Unlike scope_exit and scope_fail, if initialization of the exit function throws,
	 * the function is NOT called (per spec §18).
	 *
	 * @tparam EF Same requirements as scope_exit.
	 */
	template<typename EF>
	class scope_success
	{
	public:
		/**
		 * @brief Constructs a scope_success with the given exit function.
		 *
		 * If initialization throws, f() is NOT called (unlike scope_exit/scope_fail).
		 */
		template<typename EFP>
		  requires(!std::is_same_v<std::remove_cvref_t<EFP>, scope_success> &&
		           std::is_constructible_v<EF, EFP> &&
		           requires(std::remove_cvref_t<EFP> g) { g(); })
		explicit scope_success(EFP&& f) noexcept(std::is_nothrow_constructible_v<EF, EFP> ||
		                                         std::is_nothrow_constructible_v<EF, EFP&>): m_exit_function(detail::make_ef<EF>(std::forward<EFP>(f)))
		{}

		/**
		 * @brief Move constructor. Constraints and effects identical to scope_exit.
		 */
		scope_success(scope_success&& rhs) noexcept(std::is_nothrow_move_constructible_v<EF> ||
		                                            std::is_nothrow_copy_constructible_v<EF>)
		  requires(std::is_nothrow_move_constructible_v<EF> || std::is_copy_constructible_v<EF>)
		  : m_exit_function(detail::move_or_copy_ef(rhs.m_exit_function)),
		    m_execute_on_destruction(rhs.m_execute_on_destruction),
		    m_uncaught_on_creation(rhs.m_uncaught_on_creation)
		{
			rhs.release();
		}

		scope_success(const scope_success&)            = delete;
		scope_success& operator=(const scope_success&) = delete;
		scope_success& operator=(scope_success&&)      = delete;

		/**
		 * @brief Calls the exit function if execute_on_destruction is true and no new
		 *        exception is active (uncaught_exceptions() <= value at construction).
		 *
		 * The destructor is conditionally noexcept: noexcept iff exit_function() is noexcept.
		 */
		~scope_success() noexcept(noexcept(m_exit_function()))
		{
			if(m_execute_on_destruction && std::uncaught_exceptions() <= m_uncaught_on_creation)
			{
				m_exit_function();
			}
		}

		/** @brief Disables the exit function call on destruction. */
		void release() noexcept
		{
			m_execute_on_destruction = false;
		}

	private:
		EF   m_exit_function;
		bool m_execute_on_destruction{true};
		int  m_uncaught_on_creation{std::uncaught_exceptions()};
	};

	template<typename EF>
	scope_success(EF) -> scope_success<EF>;

	// ---------------------------------------------------------------------------
	// unique_resource
	// ---------------------------------------------------------------------------

	/**
	 * @brief Universal RAII wrapper for a resource and its cleanup (deleter) function.
	 *
	 * Implements [scope.unique_resource] (§7.6 of P0052r10). Ties one resource handle
	 * to a cleanup routine, guaranteeing the cleanup is called exactly once (on
	 * destruction, reset(), or explicit deletion) unless release() has been called.
	 *
	 * Access to the underlying resource is via get(). For pointer resources, operator*
	 * and operator-> are also provided.
	 *
	 * @tparam R Resource type (value type, or lvalue reference type). If R is an lvalue
	 *           reference type, the resource is stored as a std::reference_wrapper.
	 * @tparam D Deleter/cleanup type. Must be a Destructible function object for which
	 *           d(resource) is well-formed.
	 */
	template<typename R, typename D>
	class unique_resource
	{
		// R1 is the actual stored type: reference_wrapper when R is a reference, otherwise R.
		using R1 = std::conditional_t<std::is_reference_v<R>,
		                              std::reference_wrapper<std::remove_reference_t<R>>,
		                              R>;

	public:
		/**
		 * @brief Default constructor.
		 *
		 * Constraints: R and D are both default-constructible.
		 * Effects: Value-initializes resource and deleter; execute_on_reset is false.
		 */
		unique_resource()
		  requires(std::is_default_constructible_v<R> && std::is_default_constructible_v<D>)
		  : m_resource{},
		    m_deleter{},
		    m_execute_on_reset{false}
		{}

		/**
		 * @brief Constructs from a resource and a deleter.
		 *
		 * Constraints: R1 is constructible from RR; and (R1 is nothrow-constructible
		 * from RR, or R1 is constructible from RR&); and (D is nothrow-constructible
		 * from DD, or D is constructible from DD&).
		 *
		 * Effects: Initializes resource (moving if nothrow, otherwise copying), then
		 * initializes deleter (moving if nothrow, otherwise copying). If resource
		 * initialization throws, calls d(r). If deleter initialization throws, calls
		 * d(RESOURCE). Sets execute_on_reset to true.
		 */
		template<typename RR, typename DD>
		  requires(std::is_constructible_v<R1, RR> &&
		           (std::is_nothrow_constructible_v<R1, RR> || std::is_constructible_v<R1, RR&>) &&
		           (std::is_nothrow_constructible_v<D, DD> || std::is_constructible_v<D, DD&>))
		unique_resource(RR&& r, DD&& d) noexcept((std::is_nothrow_constructible_v<R1, RR> || std::is_nothrow_constructible_v<R1, RR&>) &&
		                                         (std::is_nothrow_constructible_v<D, DD> || std::is_nothrow_constructible_v<D, DD&>)): m_resource(detail::init_ur_resource<R1, RR>(std::forward<RR>(r), d)),
		                                                                                                                               m_deleter(detail::init_ur_deleter<R, D, DD>(m_resource, d)),
		                                                                                                                               m_execute_on_reset{true}
		{}

		/**
		 * @brief Move constructor.
		 *
		 * Effects: Initializes resource from rhs (moving if nothrow, otherwise copying),
		 * then deleter (moving if nothrow, otherwise copying). If deleter copy throws and
		 * resource was nothrow-moved and rhs.execute_on_reset, calls rhs.deleter(RESOURCE
		 * of rhs) and rhs.release() to prevent a leak. Sets execute_on_reset =
		 * exchange(rhs.execute_on_reset, false).
		 */
		unique_resource(unique_resource&& rhs) noexcept(std::is_nothrow_move_constructible_v<R1> &&
		                                                std::is_nothrow_move_constructible_v<D>): m_resource(detail::move_or_copy_ur_resource(rhs.m_resource)),
		                                                                                          m_deleter(detail::move_or_copy_ur_deleter<R>(rhs.m_deleter, rhs.m_resource, rhs.m_execute_on_reset)),
		                                                                                          m_execute_on_reset(std::exchange(rhs.m_execute_on_reset, false))
		{}

		unique_resource(const unique_resource&)            = delete;
		unique_resource& operator=(const unique_resource&) = delete;

		/** @brief Calls reset(). */
		~unique_resource()
		{
			reset();
		}

		/**
		 * @brief Move assignment.
		 *
		 * Requires: R1 is move- or copy-assignable; D is move- or copy-assignable.
		 *
		 * Effects: Calls reset(), then assigns resource and deleter from rhs with the
		 * assignment order designed for strong exception safety. Sets
		 * execute_on_reset = exchange(rhs.execute_on_reset, false).
		 */
		unique_resource& operator=(unique_resource&& rhs) noexcept(std::is_nothrow_move_assignable_v<R1> && std::is_nothrow_move_assignable_v<D>)
		{
			reset();

			if constexpr(std::is_nothrow_move_assignable_v<R1>)
			{
				if constexpr(std::is_nothrow_move_assignable_v<D>)
				{
					m_resource = std::move(rhs.m_resource);
					m_deleter  = std::move(rhs.m_deleter);
				}
				else
				{
					m_deleter  = rhs.m_deleter;
					m_resource = std::move(rhs.m_resource);
				}
			}
			else
			{
				if constexpr(std::is_nothrow_move_assignable_v<D>)
				{
					m_resource = rhs.m_resource;
					m_deleter  = std::move(rhs.m_deleter);
				}
				else
				{
					m_resource = rhs.m_resource;
					m_deleter  = rhs.m_deleter;
				}
			}

			m_execute_on_reset = std::exchange(rhs.m_execute_on_reset, false);
			return *this;
		}

		// -------------------------------------------------------------------------
		// Member functions
		// -------------------------------------------------------------------------

		/**
		 * @brief Releases the resource if execute_on_reset is true.
		 *
		 * Effects: If execute_on_reset is true, sets it to false, then calls
		 * deleter(RESOURCE).
		 */
		void reset() noexcept
		{
			if(m_execute_on_reset)
			{
				m_execute_on_reset = false;
				m_deleter(detail::get_resource_value<R>(m_resource));
			}
		}

		/**
		 * @brief Replaces the managed resource with r.
		 *
		 * Constraints: The assignment of resource from r is well-formed.
		 * Mandates: deleter(r) is well-formed.
		 *
		 * Effects: Calls reset(), then assigns resource (moving if nothrow-move-assignable,
		 * otherwise copying). If copy-assignment throws, calls deleter(r). Sets
		 * execute_on_reset to true.
		 */
		template<typename RR>
		  requires(std::is_nothrow_assignable_v<R1&, RR> ||
		           std::is_assignable_v<R1&, const std::remove_reference_t<RR>&>)
		void reset(RR&& r)
		{
			reset();

			if constexpr(std::is_nothrow_assignable_v<R1&, RR>)
			{
				m_resource = std::forward<RR>(r);
			}
			else
			{
				try
				{
					m_resource = std::as_const(r);
				}
				catch(...)
				{
					m_deleter(r);
					throw;
				}
			}

			m_execute_on_reset = true;
		}

		/**
		 * @brief Disables cleanup on destruction.
		 *
		 * Effects: Sets execute_on_reset to false.
		 */
		void release() noexcept
		{
			m_execute_on_reset = false;
		}

		/**
		 * @brief Returns the managed resource.
		 *
		 * Returns: The underlying resource value (unwrapping reference_wrapper for
		 * reference resource types).
		 */
		[[nodiscard]] const R& get() const noexcept
		{
			return detail::get_resource_value<R>(m_resource);
		}

		/**
		 * @brief Dereferences the managed pointer resource.
		 *
		 * Constraints: R is a pointer type and the pointed-to type is not void.
		 * Return type: add_lvalue_reference_t<remove_pointer_t<R>>
		 */
		[[nodiscard]] std::add_lvalue_reference_t<std::remove_pointer_t<R>>
		  operator*() const noexcept
		  requires(std::is_pointer_v<R> && !std::is_void_v<std::remove_pointer_t<R>>)
		{
			return *get();
		}

		/**
		 * @brief Returns the managed pointer resource for member access.
		 *
		 * Constraints: R is a pointer type.
		 */
		[[nodiscard]] R operator->() const noexcept
		  requires std::is_pointer_v<R>
		{
			return get();
		}

		/** @brief Returns a const reference to the deleter. */
		[[nodiscard]] const D& get_deleter() const noexcept
		{
			return m_deleter;
		}

	private:
		R1   m_resource;
		D    m_deleter;
		bool m_execute_on_reset;

		/**
		 * @brief Private constructor used by make_unique_resource_checked.
		 *
		 * Directly initializes members without the exception-safety guard of the
		 * two-arg constructor. execute_on_reset is set by the caller so that invalid
		 * resources are never passed to the deleter.
		 */
		template<typename RR, typename DD>
		unique_resource(RR&& r, DD&& d, bool execute_on_reset) noexcept(std::is_nothrow_constructible_v<R1, RR> &&
		                                                                std::is_nothrow_constructible_v<D, DD>): m_resource(std::forward<RR>(r)),
		                                                                                                         m_deleter(std::forward<DD>(d)),
		                                                                                                         m_execute_on_reset(execute_on_reset)
		{}

		// Friend declaration matches the definition exactly (same requires clause).
		template<typename R2, typename D2, typename S2>
		  requires requires(const R2& r, const S2& s) { static_cast<bool>(r == s); }
		friend unique_resource<std::decay_t<R2>, std::decay_t<D2>>
		  make_unique_resource_checked(R2&&, const S2&, D2&&) noexcept(std::is_nothrow_constructible_v<std::decay_t<R2>, R2> &&
		                                                               std::is_nothrow_constructible_v<std::decay_t<D2>, D2>);
	};

	template<typename R, typename D>
	unique_resource(R, D) -> unique_resource<R, D>;

	// ---------------------------------------------------------------------------
	// make_unique_resource_checked
	// ---------------------------------------------------------------------------

	/**
	 * @brief Factory for unique_resource that treats a sentinel value as "no resource".
	 *
	 * Implements [scope.make_unique_resource] (§7.6.6 of P0052r10).
	 *
	 * If resource == invalid, the returned unique_resource has execute_on_reset = false,
	 * meaning the deleter will never be called. This prevents calling the deleter on an
	 * invalid resource handle (e.g., a NULL file pointer from a failed fopen).
	 *
	 * @param resource  The resource handle to manage.
	 * @param invalid   The sentinel value representing "no resource".
	 * @param d         The deleter to call on the resource when it is released.
	 * @return          A unique_resource owning the resource, or a released-state object
	 *                  if resource == invalid.
	 *
	 * Mandates: The expression static_cast<bool>(resource == invalid) must be well-formed.
	 */
	template<typename R, typename D, typename S = std::decay_t<R>>
	  requires requires(const R& r, const S& s) { static_cast<bool>(r == s); }
	unique_resource<std::decay_t<R>, std::decay_t<D>>
	  make_unique_resource_checked(R&& resource, const S& invalid, D&& d) noexcept(std::is_nothrow_constructible_v<std::decay_t<R>, R> &&
	                                                                               std::is_nothrow_constructible_v<std::decay_t<D>, D>)
	{
		const bool valid = !bool(resource == invalid);
		return unique_resource<std::decay_t<R>, std::decay_t<D>>(
		  std::forward<R>(resource),
		  std::forward<D>(d),
		  valid);
	}

} // namespace alt
