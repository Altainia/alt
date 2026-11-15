#include <alt/task_group.hpp>
#include <cassert>
#include <utility>

namespace alt
{

	task_group::task_group() = default;

	task_group::~task_group()
	{
		assert(m_counter.outstanding() == 0 && "destroying an alt::task_group with live tasks; request_stop() and join() it first");
	}

	cancellation_handle task_group::spawn(cancellable_task t, const start_options& options)
	{
		assert(!options.token.stop_possible() && "alt::task_group supplies its own stop token; register a std::stop_callback on your signal that calls the group's request_stop() instead");
		assert(options.on_completion.invoke == nullptr && "alt::task_group owns start_options::on_completion; use an alt::scope_exit local in the task body for a per-task notification instead");

		start_options owned = options;
		owned.token         = m_source.get_token();
		owned.on_completion = {.invoke = &task_group::on_task_complete, .state = this};

		// DELICATE. Read this before changing it.
		//
		// Two statements, and both their order and which one is last are load-bearing.
		//
		// The count is raised before the task starts. A body with no suspension point
		// runs to completion inside start, which destroys its frame and fires the
		// completion hook there, so a count raised afterwards would be retired before
		// it was ever added.
		//
		// The start call is the last statement, and nothing may be read from *this after
		// it. A hook firing inside start can reach zero and resume a joiner, and that
		// joiner's body may destroy this group. This is the rule alt::async_event::set
		// carries for resume_claimed, in the same form.
		//
		// A release inside spawn itself is unreachable, which saves chasing it: the
		// count is at least one once raised, and a synchronous completion returns it to
		// exactly its value before the spawn. A joiner can only be parked if that value
		// was already at least one, so no zero is reached here.
		m_counter.add();
		return start(std::move(t), owned);
	}

	bool task_group::request_stop() noexcept
	{
		return m_source.request_stop();
	}

	bool task_group::stop_requested() const noexcept
	{
		return m_source.stop_requested();
	}

	std::size_t task_group::outstanding() const noexcept
	{
		return m_counter.outstanding();
	}

	void task_group::on_task_complete(void* state) noexcept
	{
		static_cast<task_group*>(state)->m_counter.complete();
	}

}
