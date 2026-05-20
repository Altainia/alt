#include <gtest/gtest.h>

#include <algorithm>
#include <alt/memory.hpp>
#include <array>
#include <cstring>
#include <iterator>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{

	using ClearingStr = std::basic_string<char, std::char_traits<char>, alt::clearing_allocator<char>>;

	// ---- spy allocator (same pattern as test_memory.cpp) ----

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
			captured->assign(raw, raw + n * sizeof(T));
			std::allocator<T>{}.deallocate(p, n);
		}
	};

	template<typename T, typename U>
	bool operator==(spy_allocator<T> const& a, spy_allocator<U> const& b) noexcept
	{
		return a.captured == b.captured;
	}

	using SpyClearingStr =
	  std::basic_string<char, std::char_traits<char>, alt::clearing_allocator<char, spy_allocator<char>>>;

	// Unique byte sequences used to identify specific string content in raw memory.
	// These byte values are chosen to avoid being plausible string metadata (length, capacity, etc.)
	constexpr std::array<char, 8> k_sso_secret{'\x01', '\x02', '\x03', '\x04', '\x05', '\x06', '\x07', '\x08'};

	bool contains_sequence(void const* storage, std::size_t storage_size, char const* seq, std::size_t seq_len)
	{
		auto const* begin = static_cast<char const*>(storage);
		auto const* end   = begin + storage_size;
		return std::search(begin, end, seq, seq + seq_len) != end;
	}

	// ============================================================
	// SSO Clearing
	// ============================================================

	TEST(SSOClearing, SSOCharDataClearedOnDestruction)
	{
		alignas(ClearingStr) std::array<unsigned char, sizeof(ClearingStr)> storage;
		storage.fill(0xCC);

		// Placement-new a short string (well within SSO threshold)
		std::string_view secret(k_sso_secret.data(), k_sso_secret.size());

		auto* sp = ::new(storage.data()) ClearingStr(secret);
		ASSERT_EQ(sp->size(), secret.size());

		// The character bytes must be somewhere in the object's raw storage
		ASSERT_TRUE(contains_sequence(storage.data(), storage.size(), secret.data(), secret.size()))
		  << "Test pre-condition: SSO string content must reside in object storage";

		sp->~ClearingStr();

		EXPECT_FALSE(contains_sequence(storage.data(), storage.size(), secret.data(), secret.size()))
		  << "SSO character bytes should be zeroed after destruction";
	}

	TEST(SSOClearing, EmptyStringDestructionIsSafe)
	{
		// Should not crash
		{
			ClearingStr s;
			(void)s;
		}
	}

	TEST(SSOClearing, LongStringHeapNotInObjectStorage)
	{
		// A string longer than SSO threshold stores data on the heap.
		// Verify the heap data is NOT present in the object's raw storage
		// (confirming this test path exercises heap, not SSO).
		std::string                                                         heap_content(64, 'X');
		alignas(ClearingStr) std::array<unsigned char, sizeof(ClearingStr)> storage;
		storage.fill(0xCC);

		auto* sp = ::new(storage.data()) ClearingStr(heap_content);
		bool  in_storage =
		  contains_sequence(storage.data(), storage.size(), heap_content.data(), heap_content.size());
		sp->~ClearingStr();

		// The data should be on the heap, not in the object itself
		EXPECT_FALSE(in_storage) << "Heap-allocated string should not store chars in object storage";
	}

	TEST(SSOClearing, MoveSourceSSOClearedAfterMoveConstruct)
	{
		alignas(ClearingStr) std::array<unsigned char, sizeof(ClearingStr)> src_storage;
		src_storage.fill(0xCC);

		std::string_view secret(k_sso_secret.data(), k_sso_secret.size());
		auto*            src = ::new(src_storage.data()) ClearingStr(secret);

		ASSERT_TRUE(contains_sequence(src_storage.data(), src_storage.size(), secret.data(), secret.size()))
		  << "Pre-condition: SSO content must be in source storage before move";

		// Move-construct into a regular stack object
		ClearingStr dst(std::move(*src));
		EXPECT_EQ(dst, std::string_view(secret));

		// The source's SSO buffer should be zeroed by our move constructor
		EXPECT_FALSE(
		  contains_sequence(src_storage.data(), src_storage.size(), secret.data(), secret.size()))
		  << "Move source's SSO buffer should be cleared after move construction";

		src->~ClearingStr();
	}

	TEST(SSOClearing, MoveSourceSSOClearedAfterMoveAssign)
	{
		alignas(ClearingStr) std::array<unsigned char, sizeof(ClearingStr)> src_storage;
		src_storage.fill(0xCC);

		std::string_view secret(k_sso_secret.data(), k_sso_secret.size());
		auto*            src = ::new(src_storage.data()) ClearingStr(secret);

		ASSERT_TRUE(contains_sequence(src_storage.data(), src_storage.size(), secret.data(), secret.size()))
		  << "Pre-condition: SSO content must be in source storage before move";

		ClearingStr dst;
		dst = std::move(*src);
		EXPECT_EQ(dst, std::string_view(secret));

		EXPECT_FALSE(
		  contains_sequence(src_storage.data(), src_storage.size(), secret.data(), secret.size()))
		  << "Move source's SSO buffer should be cleared after move assignment";

		src->~ClearingStr();
	}

	TEST(SSOClearing, MoveAssignSelfIsNoop)
	{
		ClearingStr s("hello");
#if defined(__GNUC__) && !defined(__clang__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wself-move"
#endif
		s = std::move(s); // must not crash or corrupt
#if defined(__GNUC__) && !defined(__clang__)
#	pragma GCC diagnostic pop
#endif
		// After self-move, `s` is still in a valid state (our guard: this != &other).
		// The content is unspecified per the standard, but we require no crash.
	}

	// ============================================================
	// Heap Clearing
	// ============================================================

	TEST(HeapClearing, HeapDataClearedOnDestruction)
	{
		std::vector<std::byte>                             captured;
		spy_allocator<char>                                spy{captured};
		alt::clearing_allocator<char, spy_allocator<char>> alloc{spy};

		{
			SpyClearingStr s(alloc);
			// 64 chars forces heap allocation beyond SSO
			s.assign(64, '\xAB');
		}

		// The spy captures the last deallocated block; it should be all-zero
		ASSERT_FALSE(captured.empty()) << "Expected at least one heap deallocation";
		EXPECT_TRUE(std::all_of(
		  captured.begin(), captured.end(), [](std::byte b) { return b == std::byte{0}; }))
		  << "Heap buffer must be zeroed before deallocation";
	}

	// ============================================================
	// Constructors
	// ============================================================

	TEST(Constructors, DefaultConstruct)
	{
		ClearingStr s;
		EXPECT_TRUE(s.empty());
		EXPECT_EQ(s.size(), 0u);
	}

	TEST(Constructors, FromCStringShort)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s, "hello");
		EXPECT_EQ(s.size(), 5u);
	}

	TEST(Constructors, FromCStringLong)
	{
		std::string long_str(64, 'x');
		ClearingStr s(long_str.c_str());
		EXPECT_EQ(s, long_str.c_str());
	}

	TEST(Constructors, FromCStringWithCount)
	{
		ClearingStr s("hello world", 5);
		EXPECT_EQ(s, "hello");
	}

	TEST(Constructors, Fill)
	{
		ClearingStr s(5, 'a');
		EXPECT_EQ(s, "aaaaa");
	}

	TEST(Constructors, CopyConstruct)
	{
		ClearingStr a("test");
		ClearingStr b(a);
		EXPECT_EQ(a, b);
		EXPECT_EQ(b, "test");
	}

	TEST(Constructors, MoveConstruct)
	{
		ClearingStr a("test");
		ClearingStr b(std::move(a));
		EXPECT_EQ(b, "test");
	}

	TEST(Constructors, CopyConstructWithAllocator)
	{
		ClearingStr                   a("test");
		alt::clearing_allocator<char> alloc;
		ClearingStr                   b(a, alloc);
		EXPECT_EQ(b, "test");
	}

	TEST(Constructors, MoveConstructWithAllocator)
	{
		ClearingStr                   a("test");
		alt::clearing_allocator<char> alloc;
		ClearingStr                   b(std::move(a), alloc);
		EXPECT_EQ(b, "test");
	}

	TEST(Constructors, FromStringView)
	{
		std::string_view sv = "view";
		ClearingStr      s(sv);
		EXPECT_EQ(s, "view");
	}

	TEST(Constructors, FromStringViewWithAllocator)
	{
		std::string_view              sv = "view";
		alt::clearing_allocator<char> alloc;
		ClearingStr                   s(sv, alloc);
		EXPECT_EQ(s, "view");
	}

	TEST(Constructors, FromInitializerList)
	{
		ClearingStr s{'a', 'b', 'c'};
		EXPECT_EQ(s, "abc");
	}

	TEST(Constructors, FromRange)
	{
		char const  arr[] = {'h', 'i'};
		ClearingStr s(std::begin(arr), std::end(arr));
		EXPECT_EQ(s, "hi");
	}

	TEST(Constructors, SubstringByPosOnly)
	{
		ClearingStr src("hello world");
		ClearingStr s(src, 6u);
		EXPECT_EQ(s, "world");
	}

	TEST(Constructors, SubstringByPosAndCount)
	{
		ClearingStr src("hello world");
		ClearingStr s(src, 0u, 5u);
		EXPECT_EQ(s, "hello");
	}

	TEST(Constructors, ExplicitAllocatorOnly)
	{
		alt::clearing_allocator<char> alloc;
		ClearingStr                   s(alloc);
		EXPECT_TRUE(s.empty());
	}

	TEST(Constructors, FillWithAllocator)
	{
		alt::clearing_allocator<char> alloc;
		ClearingStr                   s(3u, 'z', alloc);
		EXPECT_EQ(s, "zzz");
	}

	// ============================================================
	// Assignment Operators
	// ============================================================

	TEST(Assignment, CopyAssign)
	{
		ClearingStr a("source");
		ClearingStr b;
		b = a;
		EXPECT_EQ(b, "source");
		EXPECT_EQ(a, "source"); // source unchanged
	}

	TEST(Assignment, MoveAssign)
	{
		ClearingStr a("source");
		ClearingStr b;
		b = std::move(a);
		EXPECT_EQ(b, "source");
	}

	TEST(Assignment, AssignFromCString)
	{
		ClearingStr s;
		s = "hello";
		EXPECT_EQ(s, "hello");
	}

	TEST(Assignment, AssignFromChar)
	{
		ClearingStr s("abc");
		s = 'X';
		EXPECT_EQ(s, "X");
	}

	TEST(Assignment, AssignFromInitializerList)
	{
		ClearingStr s;
		s = {'a', 'b', 'c'};
		EXPECT_EQ(s, "abc");
	}

	TEST(Assignment, AssignFromStringView)
	{
		ClearingStr      s;
		std::string_view sv = "view";
		s                   = sv;
		EXPECT_EQ(s, "view");
	}

	// ============================================================
	// Type Correctness
	// ============================================================

	TEST(TypeCorrectness, GetAllocatorReturnsCorrectType)
	{
		ClearingStr s("hello");
		static_assert(
		  std::is_same_v<decltype(s.get_allocator()), alt::clearing_allocator<char>>,
		  "get_allocator() must return alt::clearing_allocator<char>");
	}

	TEST(TypeCorrectness, SubstrReturnsCorrectType)
	{
		ClearingStr s("hello");
		static_assert(
		  std::is_same_v<decltype(s.substr(0, 3)), ClearingStr>,
		  "substr() must return ClearingStr (not the StorageBase type)");
		EXPECT_EQ(s.substr(0, 3), "hel");
	}

	TEST(TypeCorrectness, OperatorPlusReturnsCorrectType)
	{
		ClearingStr a("hello ");
		ClearingStr b("world");
		static_assert(
		  std::is_same_v<decltype(a + b), ClearingStr>,
		  "operator+ must return ClearingStr");
		EXPECT_EQ(a + b, "hello world");
	}

	TEST(TypeCorrectness, OperatorPlusWithCString)
	{
		ClearingStr a("hello ");
		static_assert(std::is_same_v<decltype(a + "world"), ClearingStr>);
		static_assert(std::is_same_v<decltype("hello " + ClearingStr("world")), ClearingStr>);
		EXPECT_EQ(a + "world", "hello world");
		EXPECT_EQ("hello " + ClearingStr("world"), "hello world");
	}

	TEST(TypeCorrectness, OperatorPlusWithChar)
	{
		ClearingStr a("hello");
		static_assert(std::is_same_v<decltype(a + '!'), ClearingStr>);
		static_assert(std::is_same_v<decltype('X' + ClearingStr("y")), ClearingStr>);
		EXPECT_EQ(a + '!', "hello!");
		EXPECT_EQ('H' + ClearingStr("i"), "Hi");
	}

	// ============================================================
	// String Operations
	// ============================================================

	TEST(StringOps, Append)
	{
		ClearingStr s("hello");
		s.append(" world");
		EXPECT_EQ(s, "hello world");
	}

	TEST(StringOps, OperatorPlusEquals)
	{
		ClearingStr s("hello");
		s += " world";
		EXPECT_EQ(s, "hello world");
		s += '!';
		EXPECT_EQ(s, "hello world!");
	}

	TEST(StringOps, Insert)
	{
		ClearingStr s("helloworld");
		s.insert(5, " ");
		EXPECT_EQ(s, "hello world");
	}

	TEST(StringOps, Erase)
	{
		ClearingStr s("hello world");
		s.erase(5, 6);
		EXPECT_EQ(s, "hello");
	}

	TEST(StringOps, Replace)
	{
		ClearingStr s("hello world");
		s.replace(6, 5, "there");
		EXPECT_EQ(s, "hello there");
	}

	TEST(StringOps, Find)
	{
		ClearingStr s("hello world");
		EXPECT_EQ(s.find("world"), 6u);
		EXPECT_EQ(s.find("xyz"), ClearingStr::npos);
	}

	TEST(StringOps, RFind)
	{
		ClearingStr s("abcabc");
		EXPECT_EQ(s.rfind("bc"), 4u);
	}

	TEST(StringOps, FindFirstOf)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s.find_first_of("aeiou"), 1u); // 'e'
	}

	TEST(StringOps, FindLastOf)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s.find_last_of("aeiou"), 4u); // 'o'
	}

	TEST(StringOps, FindFirstNotOf)
	{
		ClearingStr s("aabbcc");
		EXPECT_EQ(s.find_first_not_of("ab"), 4u);
	}

	TEST(StringOps, FindLastNotOf)
	{
		ClearingStr s("aabbcc");
		EXPECT_EQ(s.find_last_not_of("c"), 3u);
	}

	TEST(StringOps, Compare)
	{
		ClearingStr a("abc");
		ClearingStr b("abc");
		ClearingStr c("abd");
		EXPECT_EQ(a.compare(b), 0);
		EXPECT_LT(a.compare(c), 0);
		EXPECT_GT(c.compare(a), 0);
	}

	TEST(StringOps, Comparison)
	{
		ClearingStr a("abc");
		ClearingStr b("abc");
		ClearingStr c("abd");
		EXPECT_EQ(a, b);
		EXPECT_NE(a, c);
		EXPECT_LT(a, c);
		EXPECT_GT(c, a);
	}

	TEST(StringOps, ThreeWayComparison)
	{
		ClearingStr a("abc");
		ClearingStr b("abc");
		ClearingStr c("abd");
		EXPECT_EQ(a <=> b, std::strong_ordering::equal);
		EXPECT_EQ(a <=> c, std::strong_ordering::less);
	}

	TEST(StringOps, StartsWith)
	{
		ClearingStr s("hello world");
		EXPECT_TRUE(s.starts_with("hello"));
		EXPECT_FALSE(s.starts_with("world"));
	}

	TEST(StringOps, EndsWith)
	{
		ClearingStr s("hello world");
		EXPECT_TRUE(s.ends_with("world"));
		EXPECT_FALSE(s.ends_with("hello"));
	}

	TEST(StringOps, Contains)
	{
		ClearingStr s("hello world");
		EXPECT_TRUE(s.contains("lo wo"));
		EXPECT_FALSE(s.contains("xyz"));
	}

	TEST(StringOps, SizeAndLength)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s.size(), 5u);
		EXPECT_EQ(s.length(), 5u);
		EXPECT_FALSE(s.empty());

		ClearingStr empty;
		EXPECT_EQ(empty.size(), 0u);
		EXPECT_TRUE(empty.empty());
	}

	TEST(StringOps, Reserve)
	{
		ClearingStr s;
		s.reserve(100);
		EXPECT_GE(s.capacity(), 100u);
	}

	TEST(StringOps, Resize)
	{
		ClearingStr s("hello");
		s.resize(3);
		EXPECT_EQ(s, "hel");
		s.resize(6, 'x');
		EXPECT_EQ(s, "helxxx");
	}

	TEST(StringOps, ShrinkToFit)
	{
		ClearingStr s;
		s.reserve(1000);
		s = "small";
		s.shrink_to_fit();
		// Just check it doesn't crash and the content is preserved
		EXPECT_EQ(s, "small");
	}

	TEST(StringOps, FrontAndBack)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s.front(), 'h');
		EXPECT_EQ(s.back(), 'o');
	}

	TEST(StringOps, SubscriptAndAt)
	{
		ClearingStr s("hello");
		EXPECT_EQ(s[0], 'h');
		EXPECT_EQ(s.at(4), 'o');
		EXPECT_THROW([[maybe_unused]] auto _ = s.at(10), std::out_of_range);
	}

	TEST(StringOps, CStrAndData)
	{
		ClearingStr s("hello");
		EXPECT_STREQ(s.c_str(), "hello");
		EXPECT_STREQ(s.data(), "hello");
	}

	TEST(StringOps, Substr)
	{
		ClearingStr s("hello world");
		EXPECT_EQ(s.substr(6), "world");
		EXPECT_EQ(s.substr(0, 5), "hello");
		EXPECT_THROW(s.substr(100), std::out_of_range);
	}

	TEST(StringOps, Copy)
	{
		ClearingStr s("hello");
		char        buf[6]{};
		auto        n = s.copy(buf, 5);
		EXPECT_EQ(n, 5u);
		EXPECT_STREQ(buf, "hello");
	}

	TEST(StringOps, Swap)
	{
		ClearingStr a("hello");
		ClearingStr b("world");
		a.swap(b);
		EXPECT_EQ(a, "world");
		EXPECT_EQ(b, "hello");
	}

	TEST(StringOps, Iterators)
	{
		ClearingStr s("hello");
		std::string collected(s.begin(), s.end());
		EXPECT_EQ(collected, "hello");

		std::string reversed(s.rbegin(), s.rend());
		EXPECT_EQ(reversed, "olleh");
	}

	TEST(StringOps, PushBackAndPopBack)
	{
		ClearingStr s("hell");
		s.push_back('o');
		EXPECT_EQ(s, "hello");
		s.pop_back();
		EXPECT_EQ(s, "hell");
	}

	TEST(StringOps, Clear)
	{
		ClearingStr s("hello");
		s.clear();
		EXPECT_TRUE(s.empty());
	}

	// ============================================================
	// Interoperability
	// ============================================================

	TEST(Interop, ImplicitConversionToStringView)
	{
		ClearingStr      s("hello");
		std::string_view sv = s;
		EXPECT_EQ(sv, "hello");
	}

	TEST(Interop, OutputStreamOperator)
	{
		ClearingStr        s("hello");
		std::ostringstream oss;
		oss << s;
		EXPECT_EQ(oss.str(), "hello");
	}

	TEST(Interop, GetLine)
	{
		std::istringstream iss("hello world\ngoodbye");
		ClearingStr        line;
		ASSERT_TRUE(static_cast<bool>(std::getline(iss, line)));
		EXPECT_EQ(line, "hello world");
	}

	TEST(Interop, CompareWithStdString)
	{
		ClearingStr      cs("hello");
		std::string_view sv = cs;
		std::string      s  = "hello";
		EXPECT_EQ(sv, s);
	}

	TEST(Interop, WorksWithStdSort)
	{
		std::vector<ClearingStr> v{"banana", "apple", "cherry"};
		std::sort(v.begin(), v.end());
		EXPECT_EQ(v[0], "apple");
		EXPECT_EQ(v[1], "banana");
		EXPECT_EQ(v[2], "cherry");
	}

	TEST(Interop, WorksWithStdHash)
	{
		ClearingStr            s("hello");
		std::hash<ClearingStr> h;
		EXPECT_NE(h(s), 0u); // Just verify it compiles and produces a value
	}

	// ============================================================
	// Operator+ / Concatenation
	// ============================================================

	TEST(Concat, TwoLvalues)
	{
		ClearingStr a("hello ");
		ClearingStr b("world");
		ClearingStr c = a + b;
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, RvalueLeft)
	{
		ClearingStr b("world");
		ClearingStr c = ClearingStr("hello ") + b;
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, RvalueRight)
	{
		ClearingStr a("hello ");
		ClearingStr c = a + ClearingStr("world");
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, BothRvalues)
	{
		ClearingStr c = ClearingStr("hello ") + ClearingStr("world");
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, CStringPrefix)
	{
		ClearingStr b("world");
		ClearingStr c = "hello " + b;
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, CStringSuffix)
	{
		ClearingStr a("hello");
		ClearingStr c = a + " world";
		EXPECT_EQ(c, "hello world");
	}

	TEST(Concat, CharPrefix)
	{
		ClearingStr b("ello");
		ClearingStr c = 'h' + b;
		EXPECT_EQ(c, "hello");
	}

	TEST(Concat, CharSuffix)
	{
		ClearingStr a("hell");
		ClearingStr c = a + 'o';
		EXPECT_EQ(c, "hello");
	}

	// ============================================================
	// Wchar and other character types (compile-time check)
	// ============================================================

	TEST(WideChar, BasicUsage)
	{
		using WClearingStr =
		  std::basic_string<wchar_t, std::char_traits<wchar_t>, alt::clearing_allocator<wchar_t>>;
		WClearingStr s(L"hello");
		EXPECT_EQ(s.size(), 5u);
		EXPECT_EQ(s, L"hello");
	}

} // namespace
