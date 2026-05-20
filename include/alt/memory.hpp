#pragma once

#include <cstddef>
#include <memory>

#if defined(_WIN32)
#	include <windows.h>
#elif (defined(__GLIBC__) && __GLIBC_PREREQ(2, 25)) || defined(__APPLE__)
#	include <string.h>
#endif

namespace alt
{

	/**
	 * @brief Zeroes n bytes starting at p in a way the compiler will not optimise away.
	 * @param p  Pointer to memory to zero. Must point to at least n addressable bytes.
	 *           May be null only when n == 0.
	 * @param n  Number of bytes to zero. A value of 0 is a no-op.
	 */
	inline void clear_memory(void* p, std::size_t n) noexcept
	{
#if defined(_WIN32)
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
		Alloc& underlying() noexcept
		{
			return m_alloc;
		}

		/**
		 * @brief Returns a const reference to the underlying allocator.
		 */
		Alloc const& underlying() const noexcept
		{
			return m_alloc;
		}

	private:
		[[no_unique_address]] Alloc m_alloc{};
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
