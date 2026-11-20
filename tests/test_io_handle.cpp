#include <gtest/gtest.h>

#include <alt/detail/io_handle.hpp>
#include <alt/io_backend.hpp>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#	include <windows.h>
#else
#	include <fcntl.h>
#	include <unistd.h>
#endif

namespace
{
	/// A temp file opened for reading, removed from the filesystem on destruction.
	/// The descriptor is handed to the caller, which owns closing it.
	class temp_file
	{
	public:
		temp_file(): m_path(unique_path()), m_handle(open_new(m_path))
		{}

		temp_file(const temp_file&)            = delete;
		temp_file& operator=(const temp_file&) = delete;

		~temp_file()
		{
			std::error_code ignored;
			std::filesystem::remove(m_path, ignored);
		}

		alt::native_handle release() noexcept
		{
			return std::exchange(m_handle, alt::invalid_native_handle);
		}

	private:
		static std::filesystem::path unique_path()
		{
			static std::atomic<int> counter{0};
			return std::filesystem::temp_directory_path() / ("alt_io_handle_test_" + std::to_string(++counter) + "_" + std::to_string(process_id()));
		}

		static unsigned long process_id()
		{
#ifdef _WIN32
			return ::GetCurrentProcessId();
#else
			return static_cast<unsigned long>(::getpid());
#endif
		}

		static alt::native_handle open_new(const std::filesystem::path& path)
		{
#ifdef _WIN32
			return reinterpret_cast<alt::native_handle>(::CreateFileA(path.string().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
#else
			return ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
#endif
		}

		std::filesystem::path m_path;
		alt::native_handle    m_handle;
	};

	/// True while the operating system still recognizes the handle. Probes liveness without doing I/O.
	bool is_live(alt::native_handle handle)
	{
#ifdef _WIN32
		DWORD flags = 0;
		return ::GetHandleInformation(reinterpret_cast<HANDLE>(handle), &flags) != 0;
#else
		return ::fcntl(static_cast<int>(handle), F_GETFD) != -1;
#endif
	}
}

TEST(IoHandleTest, TheDescriptorIsClosedWhenTheLastReferenceGoes)
{
	temp_file  file;
	const auto fd     = file.release();
	auto       handle = alt::detail::io_handle::adopt(fd);
	ASSERT_TRUE(is_live(fd));
	EXPECT_TRUE(handle->is_open());
	handle.reset();
	EXPECT_FALSE(is_live(fd));
}

TEST(IoHandleTest, TheDescriptorStaysOpenWhileASecondReferenceIsHeld)
{
	temp_file  file;
	const auto fd     = file.release();
	auto       first  = alt::detail::io_handle::adopt(fd);
	auto       second = first;
	first.reset();
	EXPECT_TRUE(is_live(fd));
	EXPECT_TRUE(second->is_open());
	second.reset();
	EXPECT_FALSE(is_live(fd));
}

TEST(IoHandleTest, RequestCloseMarksTheHandleWithoutClosingTheDescriptor)
{
	temp_file  file;
	const auto fd     = file.release();
	auto       handle = alt::detail::io_handle::adopt(fd);
	EXPECT_FALSE(handle->close_requested());
	handle->request_close();
	EXPECT_TRUE(handle->close_requested());
	EXPECT_TRUE(handle->is_open());
	EXPECT_EQ(handle->native(), fd);
	EXPECT_TRUE(is_live(fd));
	handle.reset();
	EXPECT_FALSE(is_live(fd));
}

TEST(IoHandleTest, ADescriptorIsNotReusedUnderAHeldReference)
{
#ifdef _WIN32
	GTEST_SKIP() << "Descriptor numbers are a POSIX notion; a Windows HANDLE is not the lowest free integer, so there is no reuse to guard against.";
#else
	// A merely-closed descriptor reports EBADF, which is benign. The dangerous case is the number being
	// handed to the next open(), so a stale read succeeds against someone else's file. POSIX gives out
	// the lowest free number, so if the held reference had let go, the second open would receive it.
	temp_file  first_file;
	const auto held_fd = first_file.release();
	auto       held    = alt::detail::io_handle::adopt(held_fd);
	held->request_close();

	temp_file  second_file;
	const auto second_fd = second_file.release();
	ASSERT_NE(second_fd, alt::invalid_native_handle);
	EXPECT_NE(second_fd, held_fd);
	EXPECT_EQ(held->native(), held_fd);
	EXPECT_TRUE(is_live(held_fd));

	::close(static_cast<int>(second_fd));
#endif
}
