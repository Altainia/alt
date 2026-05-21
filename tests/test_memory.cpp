#include <gtest/gtest.h>

#include <algorithm>
#include <alt/memory.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <list>
#include <memory>
#include <vector>

namespace
{

	template<typename T>
	struct spy_allocator
	{
		using value_type                 = T;
		std::vector<std::byte>* captured = nullptr;

		explicit spy_allocator(std::vector<std::byte>& buf): captured(&buf)
		{}

		template<typename U>
		spy_allocator(spy_allocator<U> const& other): captured(other.captured)
		{}

		T* allocate(std::size_t n)
		{
			return std::allocator<T>{}.allocate(n);
		}

		void deallocate(T* p, std::size_t n)
		{
			auto* raw = reinterpret_cast<std::byte*>(p);
			// clearing_allocator zeroes before delegating here, so captured bytes should be all zero
			captured->assign(raw, raw + (n * sizeof(T)));
			std::allocator<T>{}.deallocate(p, n);
		}
	};

	template<typename T, typename U>
	bool operator==(spy_allocator<T> const& a, spy_allocator<U> const& b) noexcept
	{
		return a.captured == b.captured;
	}

	// --- ClearMemory ---

	TEST(ClearMemory, ZeroesSingleByte)
	{
		unsigned char byte = 0xFF;
		alt::clear_memory(&byte, 1);
		EXPECT_EQ(byte, 0x00u);
	}

	TEST(ClearMemory, ZeroesMultiByteBuffer)
	{
		std::array<unsigned char, 64> buf{};
		buf.fill(0xAB);
		alt::clear_memory(buf.data(), buf.size());
		EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [](unsigned char b) { return b == 0x00u; }));
	}

	TEST(ClearMemory, LeavesNeighborsUntouched)
	{
		unsigned char buf[10];
		buf[0] = 0xDE;
		buf[9] = 0xDE;
		std::memset(buf + 1, 0xFF, 8);

		alt::clear_memory(buf + 1, 8);

		for(std::size_t i = 1; i <= 8u; ++i)
		{
			EXPECT_EQ(buf[i], 0x00u) << "index " << i;
		}
		EXPECT_EQ(buf[0], 0xDEu);
		EXPECT_EQ(buf[9], 0xDEu);
	}

	TEST(ClearMemory, NIsZeroIsNoop)
	{
		unsigned char byte = 0xAB;
		alt::clear_memory(&byte, 0);
		EXPECT_EQ(byte, 0xABu);
	}

	// --- ClearingAllocator ---

	TEST(ClearingAllocator, SingleElementZeroed)
	{
		std::vector<std::byte>                           buf;
		spy_allocator<int>                               spy{buf};
		alt::clearing_allocator<int, spy_allocator<int>> alloc{spy};

		int* p = alloc.allocate(1);
		std::memset(p, 0xFF, sizeof(int));
		alloc.deallocate(p, 1);

		ASSERT_EQ(buf.size(), sizeof(int));
		EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [](std::byte b) { return b == std::byte{0}; }));
	}

	TEST(ClearingAllocator, DeallocateZeroSizeIsNoop)
	{
		std::vector<std::byte>                           buf;
		spy_allocator<int>                               spy{buf};
		alt::clearing_allocator<int, spy_allocator<int>> alloc{spy};

		int* p = alloc.allocate(1);
		alloc.deallocate(p, 0); // n=0: clear_memory is a no-op, underlying deallocate still called
	}

	TEST(ClearingAllocator, ZeroesBeforeDeallocation)
	{
		std::vector<std::byte>                           buf;
		spy_allocator<int>                               spy{buf};
		alt::clearing_allocator<int, spy_allocator<int>> alloc{spy};

		int* p = alloc.allocate(4);
		std::memset(p, 0xFF, 4 * sizeof(int));
		alloc.deallocate(p, 4);

		ASSERT_EQ(buf.size(), 4 * sizeof(int));
		EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [](std::byte b) { return b == std::byte{0}; }));
	}

	TEST(ClearingAllocator, ZeroesCorrectByteCount)
	{
		std::vector<std::byte>                                 buf;
		spy_allocator<double>                                  spy{buf};
		alt::clearing_allocator<double, spy_allocator<double>> alloc{spy};

		double* p = alloc.allocate(4);
		std::memset(p, 0xFF, 4 * sizeof(double));
		alloc.deallocate(p, 4);

		ASSERT_EQ(buf.size(), 4 * sizeof(double));
		EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [](std::byte b) { return b == std::byte{0}; }));
	}

	TEST(ClearingAllocator, WorksWithVector)
	{
		std::vector<std::byte>                           buf;
		spy_allocator<int>                               spy{buf};
		alt::clearing_allocator<int, spy_allocator<int>> alloc{spy};

		std::vector<int, alt::clearing_allocator<int, spy_allocator<int>>> v{alloc};
		for(int i = 0; i < 100; ++i)
		{
			v.push_back(i);
		}

		// Repeated push_back triggers reallocations; each reallocation deallocates
		// the old buffer through clearing_allocator, which zeroes it first.
		// The spy captures the last such deallocation; verify it was zeroed.
		EXPECT_FALSE(buf.empty());
		EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [](std::byte b) { return b == std::byte{0}; }));
	}

	TEST(ClearingAllocator, EqualityDefaultConstructed)
	{
		alt::clearing_allocator<int> a;
		alt::clearing_allocator<int> b;
		EXPECT_EQ(a, b);
	}

	TEST(ClearingAllocator, EqualityStatefulAllocators)
	{
		std::vector<std::byte>                           buf_a;
		std::vector<std::byte>                           buf_b;
		spy_allocator<int>                               spy_a{buf_a};
		spy_allocator<int>                               spy_b{buf_b};
		alt::clearing_allocator<int, spy_allocator<int>> alloc_a{spy_a};
		alt::clearing_allocator<int, spy_allocator<int>> alloc_b{spy_b};
		EXPECT_NE(alloc_a, alloc_b);
	}

	TEST(ClearingAllocator, SelectOnCopyConstruction)
	{
		alt::clearing_allocator<int> alloc;
		const auto                   copy =
		  std::allocator_traits<alt::clearing_allocator<int>>::select_on_container_copy_construction(alloc);
		EXPECT_EQ(alloc, copy);
	}

	TEST(ClearingAllocator, RebindViaList)
	{
		std::list<int, alt::clearing_allocator<int>> lst;
		lst.push_back(1);
		lst.push_back(2);
		lst.push_back(3);
		lst.push_back(4);
		lst.push_back(5);
		lst.pop_front();
		lst.pop_front();
		EXPECT_EQ(lst.size(), 3u);
	}

} // namespace
