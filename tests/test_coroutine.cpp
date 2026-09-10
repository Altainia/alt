#include <gtest/gtest.h>

#include <alt/detail/coroutine.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

TEST(ResultStore, StartsEmpty)
{
	const alt::detail::result_store<int> store;
	EXPECT_FALSE(store.is_set());
}

TEST(ResultStore, HoldsAValue)
{
	alt::detail::result_store<int> store;
	store.set_value(7);
	EXPECT_TRUE(store.is_set());
	EXPECT_EQ(store.value(), 7);
}

TEST(ResultStore, RethrowsAStoredException)
{
	alt::detail::result_store<int> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_TRUE(store.is_set());
	EXPECT_THROW(store.value(), std::runtime_error);
}

TEST(ResultStore, MovesAMoveOnlyValueOut)
{
	alt::detail::result_store<std::unique_ptr<int>> store;
	store.set_value(std::make_unique<int>(3));
	const auto taken = std::move(store).value();
	ASSERT_NE(taken, nullptr);
	EXPECT_EQ(*taken, 3);
}

TEST(ResultStore, StoresExceptionPtrAsAValueWithoutAmbiguity)
{
	// T is itself std::exception_ptr: the value and error channels must stay distinct.
	alt::detail::result_store<std::exception_ptr> store;
	store.set_value(std::make_exception_ptr(std::runtime_error{"as a value"}));
	EXPECT_TRUE(store.is_set());
	EXPECT_NO_THROW(store.value());
	EXPECT_NE(store.value(), nullptr);
}

TEST(ResultStoreVoid, HoldsCompletion)
{
	alt::detail::result_store<void> store;
	EXPECT_FALSE(store.is_set());
	store.set_value();
	EXPECT_TRUE(store.is_set());
	EXPECT_NO_THROW(store.value());
}

TEST(ResultStoreVoid, RethrowsAStoredException)
{
	alt::detail::result_store<void> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_THROW(store.value(), std::runtime_error);
}

TEST(ResultStoreReference, RefersToTheOriginalObject)
{
	std::string                             subject{"hello"};
	alt::detail::result_store<std::string&> store;
	store.set_value(subject);
	EXPECT_EQ(&store.value(), &subject);
	store.value() += " there";
	EXPECT_EQ(subject, "hello there");
}

TEST(ResultStoreReference, RethrowsAStoredException)
{
	alt::detail::result_store<std::string&> store;
	store.set_exception(std::make_exception_ptr(std::runtime_error{"boom"}));
	EXPECT_THROW(store.value(), std::runtime_error);
}
