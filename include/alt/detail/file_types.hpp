#pragma once

#include <alt/flags.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

namespace alt
{

	/**
	 * @brief What a path or handle refers to.
	 *
	 * `unknown` is a real answer, not an error: `d_type` is `DT_UNKNOWN` on several
	 * filesystems, and a directory listing reports it rather than paying a hidden stat
	 * per entry. A caller that must know stats the entry.
	 */
	enum class file_kind : std::uint8_t
	{
		regular,
		directory,
		symlink,
		other,
		unknown,
	};

	/**
	 * @brief A portable identity for a file: the same file reports the same value.
	 *
	 * `st_dev`/`st_ino` on POSIX, volume serial number and file index on Windows. It
	 * exists so a caller can build an ETag-style validator from inode, size and
	 * mtime, which is why it has to survive the abstraction.
	 */
	struct file_identity
	{
		std::uint64_t device = 0;
		std::uint64_t serial = 0;

		/** @brief Identities compare by value. */
		[[nodiscard]] friend constexpr bool operator==(const file_identity&, const file_identity&) noexcept = default;
	};

	/** @brief What stat reports: size, modification time, kind and identity. */
	struct file_status
	{
		std::uint64_t size     = 0;
		std::int64_t  mtime_ns = 0; ///< Nanoseconds since the Unix epoch.
		file_kind     kind     = file_kind::unknown;
		file_identity identity;
	};

	/** @brief One entry of a directory listing. `.` and `..` are never reported. */
	struct directory_entry
	{
		std::string name;
		file_kind   kind = file_kind::unknown;
	};

	/**
	 * @brief How an open() should behave, combined through alt::flags<open_flag>.
	 *
	 * `read_write` is `read | write` by value, so naming it and naming both are the
	 * same request. A request with neither access bit is invalid_argument.
	 */
	enum class open_flag : std::uint8_t
	{
		read          = 1U << 0,
		write         = 1U << 1,
		read_write    = (1U << 0) | (1U << 1),
		create        = 1U << 2, ///< Create the file if it does not exist.
		exclusive     = 1U << 3, ///< With create: fail if it exists.
		truncate      = 1U << 4, ///< Discard existing contents.
		append        = 1U << 5, ///< Writes go to the end. Positional writes ignore their offset on POSIX.
		close_on_exec = 1U << 6, ///< Do not leak the descriptor into child processes.
	};

	/**
	 * @brief The permission bits for a file an open() creates.
	 *
	 * Used only with open_flag::create, and ignored on Windows. A struct rather than a
	 * bare integer so a call site cannot pass it where the flags go.
	 */
	struct file_mode
	{
		std::uint32_t permissions = 0644;
	};

	/**
	 * @brief What io_request::in points at for io_op::open.
	 *
	 * Lives in the caller's operation block, never in a coroutine frame.
	 */
	struct io_open_args
	{
		std::filesystem::path path;
		flags<open_flag>      open_flags;
		file_mode             mode;
	};

	/** @brief What io_request::in points at for io_op::rename. */
	struct io_rename_args
	{
		std::filesystem::path from;
		std::filesystem::path to;
	};

} // namespace alt
