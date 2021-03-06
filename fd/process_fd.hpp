#pragma once

#include "fd.hpp"
#include "mem_mapping.hpp"
#include "thread.hpp"
#include <oslibc/list.hpp>
#include <cloudabi/headers/cloudabi_types.h>
#include <concur/condition.hpp>
#include <concur/cv.hpp>

namespace cloudos {

struct process_fd;
typedef linked_list<process_fd*> process_list;

struct vga_stream;
struct cv_t;

struct fd_mapping_t {
	shared_ptr<fd_t> fd; /* can be empty, in this case, the mapping is unused and can be reused for another fd */
	cloudabi_rights_t rights_base;
	cloudabi_rights_t rights_inheriting;
};

typedef linked_list<pair<cloudabi_tid_t, thread_condition_signaler>> thread_wakelist;

struct userland_lock_waiters_t {
	_Atomic(cloudabi_lock_t) *lock = nullptr;
	thread_condition_signaler readlock_obtained_signaler;
	size_t number_of_readers = 0;
	thread_wakelist *waiting_writers = nullptr;
};

typedef linked_list<userland_lock_waiters_t*> userland_lock_waiters_list;

struct userland_condvar_waiters_t {
	_Atomic(cloudabi_condvar_t) *condvar = nullptr;
	thread_wakelist *waiting_threads = nullptr;
	_Atomic(cloudabi_lock_t) *lock = nullptr;
};

typedef linked_list<userland_condvar_waiters_t*> userland_condvar_waiters_list;

/** Process file descriptor
 *
 * This file descriptor contains all information necessary for running a
 * process.
 *
 * A process_fd holds its file descriptors, but does not own them, because they
 * may be shared by multiple processes.
 *
 * A process_fd holds and owns its own page directory and the lower 0x300 page
 * tables. The upper 0x100 page tables are in the page directory, but owned by
 * the page_allocator.
 *
 * The process_fd also holds and owns its threads. A process_fd starts without
 * threads, but creates one upon exec(), and copies the calling thread upon
 * fork().
 *
 * When a process FD refcount becomes 0, the process must be exited. This means
 * all FDs in the file descriptor list are de-refcounted (and possibly cleaned
 * up). Also, we must ensure that the process FD does not end up in the
 * ready/blocked list again.
 */
struct process_fd : public fd_t {
	process_fd(const char *n);
	~process_fd() override;

	void get_pid(uint8_t *pid, size_t size);

	void add_initial_fds();

	void install_page_directory();
	uint32_t *get_page_table(int i);
	uint32_t *ensure_get_page_table(int i);

	// Read an ELF from this fd, map it, and prepare it for execution. This
	// function will not remove previous process contents, use unexec() for
	// that.
	cloudabi_errno_t exec(shared_ptr<fd_t>, size_t fdslen, fd_mapping_t **new_fds, void const *argdata, size_t argdatalen);
	// TODO: make this private
	cloudabi_errno_t exec(uint8_t *elf_buffer, size_t size, uint8_t *argdata, size_t argdatalen);

	// create a main thread from the given calling thread, belonging to
	// another process (this function assumes its own page directory is
	// loaded)
	void fork(shared_ptr<thread> t);

	cloudabi_fd_t add_fd(shared_ptr<fd_t>, cloudabi_rights_t rights_base, cloudabi_rights_t rights_inheriting = 0);
	cloudabi_errno_t replace_fd(cloudabi_fd_t fdnum, shared_ptr<fd_t>, cloudabi_rights_t rights_base, cloudabi_rights_t rights_inheriting = 0);
	cloudabi_errno_t get_fd(fd_mapping_t **mapping, cloudabi_fd_t num, cloudabi_rights_t has_rights);
	cloudabi_errno_t close_fd(cloudabi_fd_t num);

	inline bool is_running() { return running; }
	void exit(cloudabi_exitcode_t exitcode, cloudabi_signal_t exitsignal = 0);
	void signal(cloudabi_signal_t exitsignal);

	// Add the given mem_mapping_t to the page directory and tables, and add
	// it to the list of mappings. If overwrite is false, will kernel_panic()
	// on existing mappings.
	cloudabi_errno_t add_mem_mapping(mem_mapping_t *mapping, bool overwrite = false);
	// Unmap the given address range
	void mem_unmap(void *addr, size_t num_pages);
	// Change protection on the given address range
	void mem_protect(void *addr, size_t num_pages, cloudabi_mprot_t prot);
	// Sync this memory range
	cloudabi_errno_t mem_sync(void *addr, size_t num_pages, cloudabi_msflags_t flags);
	// Handle a pagefault; if the access should have been fine, fix memory to allow it and return 0
	bool handle_pagefault(void *addr, bool for_writing, bool for_exec);

	// Find a piece of the address space that's free to be mapped.
	void *find_free_virtual_range(size_t num_pages);

	/* Add a thread to this process.
	 * auxv_address and entrypoint must already point to valid memory in
	 * this process; stack_bottom until stack_bottom + stack_len must point to
	 * a valid memory region for the thread's stack.
	 */
	shared_ptr<thread> add_thread(void *stack_bottom, size_t stack_len, void *auxv_address, void *entrypoint);

	static const int PAGE_SIZE = 4096 /* bytes */;

	/* If given lock is known to the kernel, return its info. Otherwise, return nullptr. */
	userland_lock_waiters_t *get_userland_lock_info(_Atomic(cloudabi_lock_t) *lock);
	/* If given lock is known to the kernel, return its info. Otherwise, make given lock
	 * known to the kernel, and return a new info object. */
	userland_lock_waiters_t *get_or_create_userland_lock_info(_Atomic(cloudabi_lock_t) *lock);
	/* Forget about the given lock: it just became unmanaged. */
	void forget_userland_lock_info(_Atomic(cloudabi_lock_t) *lock);

	/* Likewise, but for userland condition variables. */
	userland_condvar_waiters_t *get_userland_condvar_cv(_Atomic(cloudabi_condvar_t) *condvar);
	userland_condvar_waiters_t *get_or_create_userland_condvar_cv(_Atomic(cloudabi_condvar_t) *condvar, _Atomic(cloudabi_condvar_t) *lock);
	void forget_userland_condvar_cv(_Atomic(cloudabi_condvar_t) *condvar);

	inline thread_condition_signaler *get_termination_signaler() {
		return &termination_signaler;
	}

	inline bool is_terminated() {
		return !running;
	}

	thread_condition_data *allocate_current_condition_data();

	inline bool is_terminated(cloudabi_exitcode_t &c, cloudabi_signal_t &s) {
		if(running) {
			return false;
		} else {
			c = exitcode;
			s = exitsignal;
			return true;
		}
	}

	// Called by a thread when it is exiting.
	void remove_thread(shared_ptr<thread> t);

private:
	thread_list *threads = nullptr;
	void add_thread(shared_ptr<thread> thr);
	void exit_all_threads();

	// TODO: for shared mutexes, all cloudabi_tid_t's should be globally
	// unique; we don't have shared mutexes yet
	cloudabi_tid_t last_thread = MAIN_THREAD - 1;
	uint8_t pid[16] = {0};

	static const int PAGE_DIRECTORY_SIZE = 1024 /* entries */;

	size_t fd_capacity = 0;
	fd_mapping_t **fds = nullptr;

	// Page directory, filled with physical addresses to page tables
	uint32_t *page_directory = nullptr;
	// The actual backing table virtual addresses; only the first 0x300
	// entries are valid, the others are in page_allocator.kernel_page_tables
	uint32_t **page_tables = nullptr;

	// The memory mappings used by this process.
	mem_mapping_list *mappings = nullptr;

	// The kernel managed lock & condvar information for this process.
	userland_lock_waiters_list *userland_locks = nullptr;
	userland_condvar_waiters_list *userland_condvars = nullptr;

	bool running = false;
	cloudabi_exitcode_t exitcode = 0;
	cloudabi_signal_t exitsignal = 0;

	thread_condition_signaler termination_signaler;
};

}
