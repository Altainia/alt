#pragma once

#include <gtest/gtest.h>

#include <string>

// Scaffolding shared by this suite's death and exit tests.
//
// Separate from coroutine_test_support.hpp deliberately: this header depends on
// GoogleTest and that one does not, being included by translation units with no need of
// it. Only the files that actually run a death or exit test include this one.
//
// It replaces three byte-identical copies, one per death-test file. Those copies were
// deliberate, on a recorded argument that the only header the files already shared was
// the GoogleTest-free one. That was true, and it left a third option unconsidered: a
// header of their own, which costs no other translation unit anything. The copies had
// already begun to drift by the time they were replaced -- one carried a consequence
// ("and would fail to compile") that another had dropped, and a cross-reference
// promised a justification the target did not contain.
namespace alt_test
{

	/**
	 * Whether this build still has the asserts a death test observes.
	 *
	 * An assert is what the call under test is expected to die of, so NDEBUG compiles
	 * the guard away and that call returns normally. A property of the build rather
	 * than a defect, so the test skips and says which guard is gone. Each test file
	 * carries its own message for that, naming its own assert, so those stay local.
	 */
#if defined(NDEBUG)
	inline constexpr bool asserts_are_compiled_in = false;
#else
	inline constexpr bool asserts_are_compiled_in = true;
#endif

	/**
	 * Whether ThreadSanitizer is instrumenting this build.
	 *
	 * Nested rather than written as one condition because the two compilers spell it
	 * differently and GCC before 14 does not have __has_feature at all, where a single
	 * `defined(__has_feature) && __has_feature(...)` would not short-circuit in the
	 * preprocessor and would fail to compile.
	 */
#if defined(__SANITIZE_THREAD__)
	inline constexpr bool thread_sanitizer_is_active = true;
#elif defined(__has_feature)
#	if __has_feature(thread_sanitizer)
	inline constexpr bool thread_sanitizer_is_active = true;
#	else
	inline constexpr bool thread_sanitizer_is_active = false;
#	endif
#else
	inline constexpr bool thread_sanitizer_is_active = false;
#endif

	/**
	 * @brief Sets GoogleTest's death test style for one scope and puts back what was
	 *        there.
	 *
	 * GTEST_FLAG_SET writes a process-global flag and GoogleTest offers nothing scoped
	 * of its own, so a death or exit test that sets it leaves it set for everything
	 * after it in the same process. Harmless while every such test sets the same style
	 * and ctest runs each in its own process, and invisible to the next one anyone
	 * adds, which would silently inherit a style it never asked for. Restoring the
	 * previous value is cheaper than that surprise. What must not be written in its
	 * place is a bare GTEST_FLAG_SET.
	 *
	 * Under the "threadsafe" style the child re-executes the binary and runs the test
	 * body again, so the child constructs one of these too and sets the flag for
	 * itself. The restore runs after the expectation either way, which is what makes it
	 * safe to do at all.
	 */
	class scoped_death_test_style
	{
	public:
		explicit scoped_death_test_style(const std::string& style): m_previous(GTEST_FLAG_GET(death_test_style))
		{
			GTEST_FLAG_SET(death_test_style, style);
		}

		~scoped_death_test_style()
		{
			GTEST_FLAG_SET(death_test_style, m_previous);
		}

		scoped_death_test_style(const scoped_death_test_style&)            = delete;
		scoped_death_test_style& operator=(const scoped_death_test_style&) = delete;
		scoped_death_test_style(scoped_death_test_style&&)                 = delete;
		scoped_death_test_style& operator=(scoped_death_test_style&&)      = delete;

	private:
		/** What the flag held before this object changed it. */
		std::string m_previous;
	};

} // namespace alt_test
