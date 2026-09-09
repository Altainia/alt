#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "sha_boundary_vectors.hpp"

namespace sha_test
{

	/** Builds the generated vectors' message of length @p n: bytes 0, 1, 2, ... modulo 256. */
	inline std::vector<std::uint8_t> pattern(std::size_t n)
	{
		std::vector<std::uint8_t> message(n);
		for(std::size_t i = 0; i < n; ++i)
		{
			message[i] = static_cast<std::uint8_t>(i % 256);
		}
		return message;
	}

	/** Checks a one-shot function against the OpenSSL-generated padding-boundary digests. */
	template<typename OneShot>
	void check_boundaries(std::span<const sha_vectors::boundary_case> cases, OneShot one_shot)
	{
		for(const auto& expected: cases)
		{
			const auto message = pattern(expected.length);
			EXPECT_EQ(one_shot(message).to_string(), expected.expected)
			  << "message length " << expected.length;
		}
	}

	/**
	 * Checks that splitting a message across two update() calls, at every possible
	 * offset, yields the same digest as hashing it in one call. This is the property
	 * that catches partial-block buffering bugs.
	 */
	template<typename Hasher, typename OneShot>
	void check_streaming_equivalence(std::size_t length, OneShot one_shot)
	{
		const auto message  = pattern(length);
		const auto expected = one_shot(message);
		const auto whole    = std::span<const std::uint8_t>{message};

		for(std::size_t split = 0; split <= length; ++split)
		{
			Hasher hasher;
			hasher.update(whole.first(split));
			hasher.update(whole.subspan(split));
			EXPECT_EQ(hasher.finish(), expected) << "split at " << split << " of " << length;
		}
	}

} // namespace sha_test
