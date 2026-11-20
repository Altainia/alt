#include <gtest/gtest.h>

#include <alt/io_buffer.hpp>
#include <array>
#include <cstddef>
#include <memory>
#include <system_error>
#include <utility>

namespace
{
	constexpr std::byte sentinel{0xA5};
}

TEST(IoBufferTest, AnAllocatedBufferHasTheRequestedSize)
{
	auto buffer = alt::io_buffer::allocate(4096);

	ASSERT_TRUE(buffer.has_value());
	EXPECT_EQ(buffer->size(), 4096U);
	EXPECT_EQ(buffer->bytes().size(), 4096U);
	EXPECT_NE(buffer->bytes().data(), nullptr);
	EXPECT_TRUE(static_cast<bool>(*buffer));
}

TEST(IoBufferTest, ADefaultConstructedBufferIsEmptyAndFalsey)
{
	const alt::io_buffer buffer;

	EXPECT_EQ(buffer.size(), 0U);
	EXPECT_TRUE(buffer.bytes().empty());
	EXPECT_FALSE(static_cast<bool>(buffer));
}

TEST(IoBufferTest, AZeroSizeAllocationSucceedsAndIsEmpty)
{
	auto buffer = alt::io_buffer::allocate(0);

	ASSERT_TRUE(buffer.has_value());
	EXPECT_EQ(buffer->size(), 0U);
	EXPECT_TRUE(buffer->bytes().empty());
	// A zero-size allocation is a real buffer, unlike a default-constructed one.
	EXPECT_TRUE(static_cast<bool>(*buffer));
}

TEST(IoBufferTest, CopiesShareStorageSoAWriteThroughOneIsSeenByTheOther)
{
	auto original = alt::io_buffer::allocate(16);
	ASSERT_TRUE(original.has_value());
	const alt::io_buffer copy = *original;

	original->bytes()[3] = sentinel;

	EXPECT_EQ(copy.bytes().data(), original->bytes().data());
	EXPECT_EQ(copy.bytes()[3], sentinel);
}

TEST(IoBufferTest, AllocatedStorageOutlivesEveryCopyButTheLast)
{
	auto original = alt::io_buffer::allocate(64);
	ASSERT_TRUE(original.has_value());
	const alt::io_buffer survivor = *original;

	{
		const alt::io_buffer writer = *original;
		for(auto& byte: writer.bytes())
		{
			byte = sentinel;
		}
	}
	original = std::unexpected(std::make_error_code(std::errc::io_error));

	ASSERT_EQ(survivor.size(), 64U);
	for(const auto byte: survivor.bytes())
	{
		EXPECT_EQ(byte, sentinel);
	}
}

TEST(IoBufferTest, BorrowHoldsItsKeepaliveForAsLongAsTheBufferLives)
{
	std::array<std::byte, 8> storage{};
	auto                     keepalive = std::make_shared<int>(7);

	{
		const auto buffer = alt::io_buffer::borrow(storage, keepalive);

		EXPECT_EQ(keepalive.use_count(), 2);
		EXPECT_EQ(buffer.bytes().data(), storage.data());
		EXPECT_EQ(buffer.size(), storage.size());
		EXPECT_TRUE(static_cast<bool>(buffer));

		const alt::io_buffer copy = buffer;
		EXPECT_EQ(keepalive.use_count(), 3);
	}

	EXPECT_EQ(keepalive.use_count(), 1);
}

TEST(IoBufferTest, BorrowUnsafeHoldsNothingAndStillExposesTheBytes)
{
	std::array<std::byte, 8> storage{};

	const auto buffer = alt::io_buffer::borrow_unsafe(storage);
	buffer.bytes()[2] = sentinel;

	EXPECT_EQ(buffer.bytes().data(), storage.data());
	EXPECT_EQ(buffer.size(), storage.size());
	EXPECT_EQ(storage[2], sentinel);
	EXPECT_TRUE(static_cast<bool>(buffer));
}
