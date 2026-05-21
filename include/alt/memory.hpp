#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#	include <windows.h>
#elif (defined(__GLIBC__) && __GLIBC_PREREQ(2, 25)) || defined(__APPLE__)
#	include <string.h>
#endif

namespace alt
{

	/**
	 * @brief Zeroes n bytes starting at p in a way the compiler will not optimize away.
	 * @param p  Pointer to memory to zero. Must point to at least n addressable bytes.
	 *           May be null only when n == 0.
	 * @param n  Number of bytes to zero. A value of 0 is a no-op.
	 */
	inline void clear_memory(void* p, std::size_t n) noexcept
	{
#ifdef _WIN32
		SecureZeroMemory(p, n);
#elif (defined(__GLIBC__) && __GLIBC_PREREQ(2, 25)) || defined(__APPLE__)
		explicit_bzero(p, n);
#else
		auto* vp = static_cast<volatile unsigned char*>(p);
		for(std::size_t i = 0; i < n; ++i)
			vp[i] = 0u;
#endif
	}

	/**
	 * @brief Allocator adaptor that zeroes memory before releasing it.
	 *
	 * Conforms to the C++ named requirement Allocator. All operations are forwarded
	 * to the underlying allocator @p Alloc, except @c deallocate which calls
	 * @c clear_memory on the block before delegating, ensuring sensitive data is
	 * erased before the memory is returned to the allocator.
	 *
	 * @tparam T     The value type to allocate.
	 * @tparam Alloc The underlying allocator. Defaults to @c std::allocator<T>.
	 */
	template<typename T, typename Alloc = std::allocator<T>>
	class clearing_allocator
	{
	public:
		using value_type     = T;
		using allocator_type = Alloc;

		using propagate_on_container_copy_assignment =
		  typename std::allocator_traits<Alloc>::propagate_on_container_copy_assignment;
		using propagate_on_container_move_assignment =
		  typename std::allocator_traits<Alloc>::propagate_on_container_move_assignment;
		using propagate_on_container_swap =
		  typename std::allocator_traits<Alloc>::propagate_on_container_swap;
		using is_always_equal =
		  typename std::allocator_traits<Alloc>::is_always_equal;

		/**
		 * @brief Rebind helper required so containers that internally allocate a
		 *        different type (e.g. std::list allocates its node type) receive a
		 *        properly rebound clearing_allocator.
		 */
		template<typename U>
		struct rebind
		{
			using other = clearing_allocator<
			  U,
			  typename std::allocator_traits<Alloc>::template rebind_alloc<U>>;
		};

		/** @brief Default-constructs the underlying allocator. */
		clearing_allocator() = default;

		/**
		 * @brief Constructs from an existing underlying allocator instance.
		 * @param alloc  The allocator to copy.
		 */
		explicit clearing_allocator(Alloc const& alloc): m_alloc(alloc)
		{}

		/** @brief Constructs with a moved underlying allocator. */
		explicit clearing_allocator(Alloc&& alloc) noexcept(std::is_nothrow_move_constructible_v<Alloc>): m_alloc(std::move(alloc))
		{}

		/**
		 * @brief Converting constructor for rebound allocators.
		 *
		 * Invoked when a container rebinds and constructs
		 * @c clearing_allocator<U,...> from @c clearing_allocator<T,...>.
		 *
		 * @tparam U          The source value type.
		 * @tparam OtherAlloc The source underlying allocator type.
		 * @param other  The source allocator to convert from.
		 */
		template<typename U, typename OtherAlloc>
		clearing_allocator(clearing_allocator<U, OtherAlloc> const& other) noexcept(
		  std::is_nothrow_constructible_v<
		    Alloc,
		    typename std::allocator_traits<OtherAlloc>::template rebind_alloc<T>>): m_alloc(typename std::allocator_traits<Alloc>::template rebind_alloc<T>(other.underlying()))
		{}

		/**
		 * @brief Allocates storage for @p n objects of type @c T.
		 * @param n  Number of objects to allocate.
		 * @return   Pointer to the allocated storage.
		 * @throws   std::bad_alloc (or equivalent) on failure.
		 */
		[[nodiscard]] T* allocate(std::size_t n)
		{
			return std::allocator_traits<Alloc>::allocate(m_alloc, n);
		}

		/**
		 * @brief Zeroes and then releases storage for @p n objects of type @c T.
		 *
		 * @c clear_memory is called before delegating to the underlying allocator
		 * so that sensitive data is erased while the memory is still valid.
		 *
		 * @param p  Pointer previously returned by @c allocate.
		 * @param n  Number of objects (must match the value passed to @c allocate).
		 */
		void deallocate(T* p, std::size_t n) noexcept
		{
			clear_memory(p, n * sizeof(T));
			std::allocator_traits<Alloc>::deallocate(m_alloc, p, n);
		}

		/** @brief Returns a reference to the underlying allocator. */
		[[nodiscard]] Alloc& underlying() noexcept
		{
			return m_alloc;
		}

		/**
		 * @brief Returns a const reference to the underlying allocator.
		 */
		[[nodiscard]] Alloc const& underlying() const noexcept
		{
			return m_alloc;
		}

	private:
		[[no_unique_address]] Alloc m_alloc;
	};

	/**
	 * @brief Compares two clearing_allocator instances for equality.
	 *
	 * Two instances are equal when their underlying allocators are equal, meaning
	 * memory allocated by one may be deallocated by the other.
	 *
	 * @param lhs  Left-hand allocator.
	 * @param rhs  Right-hand allocator.
	 * @return     @c true if the underlying allocators compare equal.
	 */
	template<typename T, typename U, typename Alloc>
	bool operator==(clearing_allocator<T, Alloc> const& lhs,
	                clearing_allocator<U, Alloc> const& rhs) noexcept
	{
		return lhs.underlying() == rhs.underlying();
	}

} // namespace alt

namespace alt::detail
{

	/**
	 * @brief Internal allocator adaptor used as the storage allocator for the
	 *        @c std::basic_string partial specialization on @c alt::clearing_allocator.
	 *
	 * Wraps @c alt::clearing_allocator<T, Alloc> with identical allocate/deallocate
	 * behavior but is a **distinct C++ type**, preventing the partial specialization
	 * from triggering when @c std::basic_string internally rebinds its own allocator.
	 */
	template<typename T, typename Alloc = std::allocator<T>>
	class basic_string_clearing_alloc
	{
	public:
		using value_type = T;

		using propagate_on_container_copy_assignment =
		  typename std::allocator_traits<alt::clearing_allocator<T, Alloc>>::propagate_on_container_copy_assignment;
		using propagate_on_container_move_assignment =
		  typename std::allocator_traits<alt::clearing_allocator<T, Alloc>>::propagate_on_container_move_assignment;
		using propagate_on_container_swap =
		  typename std::allocator_traits<alt::clearing_allocator<T, Alloc>>::propagate_on_container_swap;
		using is_always_equal =
		  typename std::allocator_traits<alt::clearing_allocator<T, Alloc>>::is_always_equal;

		template<typename U>
		struct rebind
		{
			using other = basic_string_clearing_alloc<
			  U,
			  typename std::allocator_traits<Alloc>::template rebind_alloc<U>>;
		};

		basic_string_clearing_alloc() = default;

		explicit basic_string_clearing_alloc(
		  alt::clearing_allocator<T, Alloc> const& a) noexcept(std::is_nothrow_copy_constructible_v<alt::clearing_allocator<T, Alloc>>):
		  m_alloc(a)
		{}

		/** @brief Converting constructor for rebound allocators. */
		template<typename U, typename OtherAlloc>
		basic_string_clearing_alloc(basic_string_clearing_alloc<U, OtherAlloc> const& other) noexcept(
		  std::is_nothrow_constructible_v<
		    alt::clearing_allocator<T, Alloc>,
		    alt::clearing_allocator<U, OtherAlloc> const&>):
		  m_alloc(other.to_clearing_allocator())
		{}

		/** @brief Allocates storage for @p n objects of type @c T. */
		[[nodiscard]] T* allocate(std::size_t n)
		{
			return m_alloc.allocate(n);
		}

		/** @brief Zeroes and releases storage (delegates to the wrapped clearing_allocator). */
		void deallocate(T* p, std::size_t n) noexcept
		{
			m_alloc.deallocate(p, n);
		}

		/** @brief Returns the wrapped @c clearing_allocator. */
		[[nodiscard]] alt::clearing_allocator<T, Alloc> to_clearing_allocator() const noexcept
		{
			return m_alloc;
		}

	private:
		[[no_unique_address]] alt::clearing_allocator<T, Alloc> m_alloc;
	};

	template<typename T, typename U, typename Alloc>
	bool operator==(basic_string_clearing_alloc<T, Alloc> const& lhs,
	                basic_string_clearing_alloc<U, Alloc> const& rhs) noexcept
	{
		return lhs.to_clearing_allocator() == rhs.to_clearing_allocator();
	}

} // namespace alt::detail

namespace std
{

	/**
	 * @brief Partial specialization of @c std::basic_string for @c alt::clearing_allocator.
	 *
	 * Provides the full @c std::basic_string interface while guaranteeing that the
	 * string's character data is zeroed via @c alt::clear_memory when the object is
	 * destroyed or moved from.  This closes the security gap where short strings stored
	 * in the SSO buffer would otherwise survive the object's lifetime uncleared.
	 *
	 * - **Heap allocations**: zeroed on deallocation by @c alt::clearing_allocator.
	 * - **SSO strings**: zeroed in the destructor before the base destructor runs.
	 * - **Moved-from objects**: their SSO buffer is zeroed immediately after the move.
	 *
	 * Available whenever @c <alt/memory.hpp> is included.
	 */
	template<typename CharT, typename Traits, typename Alloc>
	class basic_string<CharT, Traits, alt::clearing_allocator<CharT, Alloc>>: public basic_string<CharT, Traits, alt::detail::basic_string_clearing_alloc<CharT, Alloc>>
	{
		using StorageBase =
		  basic_string<CharT, Traits, alt::detail::basic_string_clearing_alloc<CharT, Alloc>>;
		using Spec = basic_string<CharT, Traits, alt::clearing_allocator<CharT, Alloc>>;

		static alt::detail::basic_string_clearing_alloc<CharT, Alloc>
		  to_base_alloc(alt::clearing_allocator<CharT, Alloc> const& a)
		{
			return alt::detail::basic_string_clearing_alloc<CharT, Alloc>{a};
		}

		static void zero_sso(Spec& s) noexcept
		{
			alt::clear_memory(
			  const_cast<CharT*>(s.StorageBase::data()),
			  (s.StorageBase::capacity() + 1) * sizeof(CharT));
		}

	public:
		// ---- Type aliases ----

		using allocator_type = alt::clearing_allocator<CharT, Alloc>;
		using typename StorageBase::const_iterator;
		using typename StorageBase::const_pointer;
		using typename StorageBase::const_reference;
		using typename StorageBase::const_reverse_iterator;
		using typename StorageBase::difference_type;
		using typename StorageBase::iterator;
		using typename StorageBase::pointer;
		using typename StorageBase::reference;
		using typename StorageBase::reverse_iterator;
		using typename StorageBase::size_type;
		using typename StorageBase::traits_type;
		using typename StorageBase::value_type;
		static constexpr size_type npos = StorageBase::npos;

		// ---- Constructors ----

		// Bring in all StorageBase constructors. Those that default-construct the
		// allocator will use basic_string_clearing_alloc{} (identical behavior to
		// default-constructed clearing_allocator{}).
		using StorageBase::StorageBase; // NOLINT(modernize-use-equals-default)

		/** @brief Default constructor. */
		basic_string() = default;

		/** @brief Constructs with the given allocator. */
		explicit basic_string(allocator_type const& a) noexcept:
		  StorageBase(to_base_alloc(a))
		{}

		/** @brief Constructs @p count copies of @p ch with the given allocator. */
		basic_string(size_type count, CharT ch, allocator_type const& a):
		  StorageBase(count, ch, to_base_alloc(a))
		{}

		/** @brief Constructs a suffix copy starting at @p pos with the given allocator. */
		basic_string(Spec const& other, size_type pos, allocator_type const& a = allocator_type{}):
		  StorageBase(static_cast<StorageBase const&>(other), pos, to_base_alloc(a))
		{}

		/** @brief Constructs a substring [@p pos, @p pos + @p count) with the given allocator. */
		basic_string(
		  Spec const&           other,
		  size_type             pos,
		  size_type             count,
		  allocator_type const& a = allocator_type{}):
		  StorageBase(static_cast<StorageBase const&>(other), pos, count, to_base_alloc(a))
		{}

		/** @brief Constructs from @p count characters starting at @p s with the given allocator. */
		basic_string(CharT const* s, size_type count, allocator_type const& a):
		  StorageBase(s, count, to_base_alloc(a))
		{}

		/** @brief Constructs from the null-terminated string @p s with the given allocator. */
		basic_string(CharT const* s, allocator_type const& a):
		  StorageBase(s, to_base_alloc(a))
		{}

		/** @brief Constructs from the range [@p first, @p last) with the given allocator. */
		template<typename InputIt>
		basic_string(InputIt first, InputIt last, allocator_type const& a):
		  StorageBase(first, last, to_base_alloc(a))
		{}

		/** @brief Constructs from an initializer list with the given allocator. */
		basic_string(std::initializer_list<CharT> il, allocator_type const& a):
		  StorageBase(il, to_base_alloc(a))
		{}

		/** @brief Constructs from a @c basic_string_view with the given allocator. */
		basic_string(std::basic_string_view<CharT, Traits> sv, allocator_type const& a):
		  StorageBase(sv, to_base_alloc(a))
		{}

		/** @brief Constructs a substring of a @c basic_string_view with the given allocator. */
		basic_string(
		  std::basic_string_view<CharT, Traits> sv,
		  size_type                             pos,
		  size_type                             count,
		  allocator_type const&                 a):
		  StorageBase(sv, pos, count, to_base_alloc(a))
		{}

		/** @brief Copy constructor. */
		basic_string(Spec const&) = default;

		/** @brief Copy-constructs with the given allocator. */
		basic_string(Spec const& other, allocator_type const& a):
		  StorageBase(static_cast<StorageBase const&>(other), to_base_alloc(a))
		{}

		/**
		 * @brief Move constructor.
		 *
		 * After the base move, zeroes the source's full SSO buffer to prevent
		 * stale characters persisting in the moved-from object.
		 */
		basic_string(Spec&& other) noexcept:
		  StorageBase(static_cast<StorageBase&&>(other))
		{
			zero_sso(other);
		}

		/**
		 * @brief Move-constructs with the given allocator.
		 *
		 * Zeroes the source's SSO buffer after the base move completes.
		 */
		basic_string(Spec&& other, allocator_type const& a):
		  StorageBase(static_cast<StorageBase&&>(other), to_base_alloc(a))
		{
			zero_sso(other);
		}

		/** @brief Constructs a suffix rvalue-move starting at @p pos (C++23). */
		basic_string(Spec&& other, size_type pos, allocator_type const& a = allocator_type{}):
		  StorageBase(static_cast<StorageBase&&>(other), pos, to_base_alloc(a))
		{
			zero_sso(other);
		}

		/** @brief Constructs a substring rvalue-move (C++23). */
		basic_string(
		  Spec&&                other,
		  size_type             pos,
		  size_type             count,
		  allocator_type const& a = allocator_type{}):
		  StorageBase(static_cast<StorageBase&&>(other), pos, count, to_base_alloc(a))
		{
			zero_sso(other);
		}

		// These constructors accept the internal StorageBase type directly.
		// They are needed by substr() (which returns StorageBase) and by any
		// operator+ delegation that produces a StorageBase temporary.
		// The using-declaration alone does not reliably provide them when
		// explicit Spec(Spec&&)/Spec(Spec const&) declarations are present.

		/** @brief Constructs from a @c StorageBase lvalue. */
		basic_string(StorageBase const& base): StorageBase(base)
		{}

		/** @brief Constructs from a @c StorageBase rvalue. */
		basic_string(StorageBase&& base) noexcept: StorageBase(std::move(base))
		{}

		// ---- Destructor ----

		/**
		 * @brief Destructor — zeroes character data before the base destructor runs.
		 *
		 * For SSO strings, @c data() points to the object's internal buffer; this call
		 * clears those bytes.  For heap strings, @c clearing_allocator::deallocate
		 * already zeroes the allocation; the call here is a belt-and-suspenders zero
		 * of the used portion.
		 */
		~basic_string() noexcept
		{
			// Clear the full SSO buffer (capacity+1), not just size(), to erase stale bytes
			// left from prior assignments that shortened the string (e.g. "Bob" -> "Hi").
			// For heap strings this is a harmless extra zero before deallocate does the same.
			zero_sso(*this);
		}

		// ---- Assignment operators ----

		/** @brief Copy assignment. */
		Spec& operator=(Spec const& other)
		{
			StorageBase::operator=(static_cast<StorageBase const&>(other));
			return *this;
		}

		/**
		 * @brief Move assignment.
		 *
		 * Zeroes this string's current content, performs the base move, then zeroes
		 * the source's SSO buffer.
		 */
		Spec& operator=(Spec&& other) noexcept(
		  noexcept(std::declval<StorageBase&>() = std::declval<StorageBase&&>()))
		{
			if(this != &other)
			{
				alt::clear_memory(
				  const_cast<CharT*>(StorageBase::data()),
				  StorageBase::size() * sizeof(CharT));
				StorageBase::operator=(static_cast<StorageBase&&>(other));
				zero_sso(other);
			}
			return *this;
		}

		/** @brief Assigns from a null-terminated character array. */
		Spec& operator=(CharT const* s)
		{
			StorageBase::operator=(s);
			return *this;
		}

		/** @brief Assigns a single character. */
		Spec& operator=(CharT c)
		{
			StorageBase::operator=(c);
			return *this;
		}

		/** @brief Assigns from an initializer list. */
		Spec& operator=(std::initializer_list<CharT> il)
		{
			StorageBase::operator=(il);
			return *this;
		}

		/** @brief Assigns from a @c basic_string_view. */
		Spec& operator=(std::basic_string_view<CharT, Traits> sv)
		{
			StorageBase::operator=(sv);
			return *this;
		}

		// ---- Accessors that must return the correct allocator type ----

		/**
		 * @brief Returns the allocator associated with this string.
		 * @return @c alt::clearing_allocator<CharT, Alloc> for this object.
		 */
		[[nodiscard]] allocator_type get_allocator() const noexcept
		{
			return StorageBase::get_allocator().to_clearing_allocator();
		}

		// ---- substr ----

		/**
		 * @brief Returns a substring as a new instance of this specialization.
		 *
		 * Delegates bounds checking and construction to @c StorageBase::substr, then
		 * wraps the result in the correct specialization type.
		 *
		 * @param pos   Start position. Throws @c std::out_of_range if @p pos > size().
		 * @param count Length of the substring (clamped to available characters).
		 */
		[[nodiscard]] Spec substr(size_type pos = 0, size_type count = npos) const
		{
			return Spec(StorageBase::substr(pos, count));
		}

		// ---- swap ----

		/** @brief Swaps the contents of two strings of this specialization. */
		void swap(Spec& other) noexcept
		{
			StorageBase::swap(static_cast<StorageBase&>(other));
		}
	};

} // namespace std
