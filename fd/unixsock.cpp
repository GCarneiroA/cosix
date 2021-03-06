#include <fd/unixsock.hpp>
#include <fd/scheduler.hpp>
#include <oslibc/iovec.hpp>

using namespace cloudos;

static bool unixsock_is_readable(void *r, thread_condition*, thread_condition_data **conditiondata) {
	auto *unixsock = reinterpret_cast<struct unixsock*>(r);
	bool readable = unixsock->is_readable();
	if(readable && conditiondata) {
		*conditiondata = unixsock->allocate_current_condition_data();
	}
	return readable;
}

static bool unixsock_is_writeable(void *r, thread_condition*, thread_condition_data **) {
	auto *unixsock = reinterpret_cast<struct unixsock*>(r);
	return unixsock->is_writeable();
}

unixsock::unixsock(cloudabi_filetype_t sockettype, cloudabi_fdflags_t flags, const char *n)
: sock_t(sockettype, flags, n)
{
	recv_signaler.set_already_satisfied_function(unixsock_is_readable, this);
	send_signaler.set_already_satisfied_function(unixsock_is_writeable, this);
}

unixsock::~unixsock()
{
	assert(status == sockstatus_t::CONNECTED || status == sockstatus_t::SHUTDOWN);

	sock_shutdown(CLOUDABI_SHUT_RD | CLOUDABI_SHUT_WR);
	auto o = othersock.lock();
	if(o) {
		o->error = ECONNRESET;
	}
	remove_all(&recv_messages, [&](unixsock_message_list *) {
		return true;
	}, [&](unixsock_message_list *item) {
		remove_all(&item->data->fd_list, [&](linked_list<fd_mapping_t> *) {
			return true;
		});
		deallocate(item->data->buf);
		deallocate(item->data);
		deallocate(item);
	});

	assert(recv_messages == nullptr);
}

size_t unixsock::bytes_readable() const
{
	return num_recv_bytes;
}

bool unixsock::is_readable()
{
	if(recv_messages != nullptr) {
		return true;
	}
	return is_shutdown();
}

bool unixsock::is_shutdown()
{
	auto other = othersock.lock();
	return !other || other->status == sockstatus_t::SHUTDOWN;
}

bool unixsock::is_writeable()
{
	if(status != sockstatus_t::CONNECTED) {
		return false;
	}
	auto other = othersock.lock();
	assert(other);
	assert(other->num_recv_bytes <= MAX_SIZE_BUFFERS);
	return other->num_recv_bytes != MAX_SIZE_BUFFERS;
}

thread_condition_data *unixsock::allocate_current_condition_data() {
	auto *cd = allocate<thread_condition_data_fd_readwrite>();
	cd->nbytes = bytes_readable();
	cd->flags = is_shutdown() ? CLOUDABI_EVENT_FD_READWRITE_HANGUP : 0;
	return cd;
}

cloudabi_errno_t unixsock::get_read_signaler(thread_condition_signaler **s)
{
	*s = &recv_signaler;
	return 0;
}

cloudabi_errno_t unixsock::get_write_signaler(thread_condition_signaler **s)
{
	if(status != sockstatus_t::CONNECTED) {
		return EPIPE;
	}
	*s = &send_signaler;
	return 0;
}

void unixsock::socketpair(shared_ptr<unixsock> other)
{
	assert(status == sockstatus_t::IDLE);
	assert(other->status == sockstatus_t::IDLE);
	assert(type == other->type);

	status = other->status = sockstatus_t::CONNECTED;
	othersock = other;
	other->othersock = weak_from_this();
}

size_t unixsock::read(void *dest, size_t count)
{
	cloudabi_iovec_t iovec[1];
	iovec[0].buf = dest;
	iovec[0].buf_len = count;

	cloudabi_recv_in_t recv_in[1];
	recv_in[0].ri_data = &iovec[0];
	recv_in[0].ri_data_len = 1;
	recv_in[0].ri_fds = nullptr;
	recv_in[0].ri_fds_len = 0;
	recv_in[0].ri_flags = 0;

	cloudabi_recv_out_t recv_out[1];
	recv_out[0].ro_datalen = 0;
	recv_out[0].ro_fdslen = 0;

	sock_recv(recv_in, recv_out);
	assert(recv_out[0].ro_fdslen == 0);
	return recv_out[0].ro_datalen;
}

size_t unixsock::write(const char *str, size_t count)
{
	cloudabi_ciovec_t iovec[1];
	iovec[0].buf = str;
	iovec[0].buf_len = count;

	cloudabi_send_in_t send_in[1];
	send_in[0].si_data = &iovec[0];
	send_in[0].si_data_len = 1;
	send_in[0].si_fds = nullptr;
	send_in[0].si_fds_len = 0;
	send_in[0].si_flags = 0;

	cloudabi_send_out_t send_out[1];
	send_out[0].so_datalen = 0;

	sock_send(send_in, send_out);
	return send_out[0].so_datalen;
}

void unixsock::sock_shutdown(cloudabi_sdflags_t how)
{
	if(status != sockstatus_t::CONNECTED) {
		error = ENOTCONN;
		return;
	}
	auto other = othersock.lock();
	if(other && (how & CLOUDABI_SHUT_RD)) {
		other->sock_shutdown(CLOUDABI_SHUT_WR);
	}
	if(how & CLOUDABI_SHUT_WR) {
		status = sockstatus_t::SHUTDOWN;
		if(other) {
			// Let a potential reader know that we shut down so it can
			// trigger EOF
			other->recv_messages_cv.broadcast();
		}
	}
	error = 0;
}

void unixsock::sock_recv(const cloudabi_recv_in_t* in, cloudabi_recv_out_t *out)
{
	out->ro_flags = 0;
	out->ro_datalen = 0;
	out->ro_fdslen = 0;

	if(status != sockstatus_t::CONNECTED && status != sockstatus_t::SHUTDOWN) {
		error = ENOTCONN;
		return;
	}

	assert(type == CLOUDABI_FILETYPE_SOCKET_DGRAM
	    || type == CLOUDABI_FILETYPE_SOCKET_STREAM);

	bool waitall = type == CLOUDABI_FILETYPE_SOCKET_STREAM && in->ri_flags & CLOUDABI_SOCK_RECV_WAITALL;
	bool peek = in->ri_flags & CLOUDABI_SOCK_RECV_PEEK;

	size_t wanted_data = 0;
	if(waitall) {
		for(size_t i = 0; i < in->ri_data_len; ++i) {
			wanted_data += in->ri_data[i].buf_len;
		}
	}

	while(true) {
		// see if condition is already satisfied
		if(waitall) {
			// count number of bytes present
			size_t bytes_present = 0;
			for(auto *message = recv_messages; message; message = message->next) {
				bytes_present += message->data->buf.size - message->data->stream_data_recv;
			}
			if(bytes_present >= wanted_data) {
				break;
			}
		} else {
			if(recv_messages != nullptr) {
				break;
			}
		}

		auto other = othersock.lock();
		if(!other || other->status == sockstatus_t::SHUTDOWN) {
			// there is not enough data and othersock is already destroyed or shut down, so signal EOF
			error = 0;
			return;
		}
		assert(other->othersock.lock().get() == this);
		assert(other->status == sockstatus_t::CONNECTED || other->status == sockstatus_t::SHUTDOWN);

		if(flags & CLOUDABI_FDFLAG_NONBLOCK) {
			error = EAGAIN;
			return;
		}

		// wait until there is at least one more message
		// release other, so it can be destructed during this wait() if needed
		other.reset();
		recv_messages_cv.wait();
		other = othersock.lock();
	}
	assert(recv_messages);

	if(type == CLOUDABI_FILETYPE_SOCKET_DGRAM) {
		// Datagram receiving: take next message; fill current buffers
		// with only it
		auto item = recv_messages;
		auto message = item->data;
		if(!peek) {
			recv_messages = item->next;
			deallocate(item);
		}

		size_t bytes_copied = veccpy(in->ri_data, in->ri_data_len, message->buf, 0);
		out->ro_flags = bytes_copied < message->buf.size ? CLOUDABI_SOCK_RECV_DATA_TRUNCATED : 0;

		auto *fd_item = message->fd_list;
		size_t fds_set = 0;
		auto process = get_scheduler()->get_running_thread()->get_process();
		while(fd_item) {
			if(fds_set < in->ri_fds_len) {
				fd_mapping_t &fd_map = fd_item->data;
				in->ri_fds[fds_set] = process->add_fd(fd_map.fd, fd_map.rights_base, fd_map.rights_inheriting);
				fds_set++;
			} else if(in->ri_fds_len > 0) {
				// if userland did not ask for FDs at all, we shouldn't set FDS_TRUNCATED
				out->ro_flags |= CLOUDABI_SOCK_RECV_FDS_TRUNCATED;
			}
			auto d = fd_item;
			fd_item = fd_item->next;
			if(!peek) {
				deallocate(d);
			}
		}

		num_recv_bytes -= bytes_copied;
		out->ro_datalen = bytes_copied;
		out->ro_fdslen = fds_set;
		error = 0;

		if(!peek) {
			deallocate(message->buf);
			deallocate(message);
			send_signaler.condition_broadcast();
		}
	} else if(type == CLOUDABI_FILETYPE_SOCKET_STREAM) {
		// Stream receiving: while the current buffers aren't full,
		// fill them with parts of the next message, taking them off
		// the list when they are fully consumed.
		// However, if a new buffer contains file descriptors, these
		// act as read boundaries, i.e. the read stops before the data.

		auto *recv_message = recv_messages;
		size_t total_written = 0;
		size_t messages_consumed = 0;
		bool next_message_read = false;
		bool fd_boundary_encountered = false;
		size_t stream_data_recv = recv_message->data->stream_data_recv;
		for(size_t i = 0; i < in->ri_data_len && recv_message && !fd_boundary_encountered; ++i) {
			auto &iovec = in->ri_data[i];
			size_t written = 0;
			while(written < iovec.buf_len && recv_message && !fd_boundary_encountered) {
				auto *body = recv_message->data;
				assert(body->buf.size >= stream_data_recv);
				size_t message_remaining = body->buf.size - stream_data_recv;
				if(message_remaining > 0) {
					size_t iovec_remaining = iovec.buf_len - written;
					size_t copy = iovec_remaining < message_remaining ? iovec_remaining : message_remaining;
					memcpy(reinterpret_cast<char*>(iovec.buf) + written,
						reinterpret_cast<char*>(body->buf.ptr) + stream_data_recv, copy);
					stream_data_recv += copy;
					if(!peek) {
						body->stream_data_recv = stream_data_recv;
					}
					written += copy;
					message_remaining -= copy;
					assert(copy > 0);
					next_message_read = true;
				}
				if(message_remaining == 0) {
					// no data left in this buffer
					recv_message = recv_message->next;
					if(recv_message) {
						stream_data_recv = recv_message->data->stream_data_recv;
					}
					++messages_consumed;
					next_message_read = false;

					// move on to the next buffer only if it has no file descriptors
					// attached to it; these act as a read boundary
					fd_boundary_encountered = recv_message && recv_message->data->fd_list != nullptr;
				}
			}
			total_written += written;
		}

		recv_message = recv_messages;
		out->ro_flags = 0;

		size_t fd_messages = messages_consumed + (next_message_read ? 1 : 0);
		size_t fds_set = 0;
		auto process = get_scheduler()->get_running_thread()->get_process();
		for(; fd_messages > 0; --fd_messages) {
			assert(recv_message);

			// FDs from this message were consumed in the stream; take or destroy them
			auto *fd_item = recv_messages->data->fd_list;
			while(fd_item) {
				if(fds_set < in->ri_fds_len) {
					fd_mapping_t &fd_map = fd_item->data;
					in->ri_fds[fds_set] = process->add_fd(fd_map.fd, fd_map.rights_base, fd_map.rights_inheriting);
					fds_set++;
				} else if(in->ri_fds_len > 0) {
					// if userland did not ask for FDs at all, we shouldn't set FDS_TRUNCATED
					out->ro_flags |= CLOUDABI_SOCK_RECV_FDS_TRUNCATED;
				}
				auto d = fd_item;
				fd_item = fd_item->next;
				if(!peek) {
					deallocate(d);
				}
			}
			if(!peek) {
				recv_messages->data->fd_list = nullptr;
			}

			recv_message = recv_message->next;
		}

		if(!peek) {
			for(; messages_consumed > 0; --messages_consumed) {
				assert(recv_messages);

				auto *d = recv_messages;
				recv_messages = recv_messages->next;
				deallocate(d->data->buf);
				deallocate(d->data);
				deallocate(d);
			}
		}

		num_recv_bytes -= total_written;
		out->ro_datalen = total_written;
		out->ro_fdslen = fds_set;
		error = 0;
		if(!peek && total_written > 0) {
			send_signaler.condition_broadcast();
		}
	}
}

void unixsock::sock_send(const cloudabi_send_in_t* in, cloudabi_send_out_t *out)
{
	if(status == sockstatus_t::SHUTDOWN) {
		error = EPIPE;
		return;
	}
	if(status != sockstatus_t::CONNECTED) {
		error = ENOTCONN;
		return;
	}
	auto other = othersock.lock();
	assert(other);
	assert(other->othersock.lock());

	assert(type == CLOUDABI_FILETYPE_SOCKET_DGRAM
	    || type == CLOUDABI_FILETYPE_SOCKET_STREAM);

	size_t total_message_size = 0;
	for(size_t i = 0; i < in->si_data_len; ++i) {
		const cloudabi_ciovec_t &data = in->si_data[i];
		// TODO: guard against overflow
		total_message_size += data.buf_len;
	}

	if(!is_writeable()) {
		// TODO: if not O_NONBLOCK, block for the messages to be read
		// off the socket?
		error = EAGAIN;
		return;
	}

	if(total_message_size + other->num_recv_bytes > MAX_SIZE_BUFFERS) {
		// limit the number of bytes written
		total_message_size = MAX_SIZE_BUFFERS - other->num_recv_bytes;
	}

	auto *message = allocate<unixsock_message>();
	message->buf = allocate(total_message_size);
	size_t bytes_copied = veccpy(message->buf, in->si_data, in->si_data_len, 0);
	assert(bytes_copied == total_message_size);

	auto process = get_scheduler()->get_running_thread()->get_process();
	for(size_t i = 0; i < in->si_fds_len; ++i) {
		cloudabi_fd_t fdnum = in->si_fds[i];
		fd_mapping_t *fd_mapping;
		error = process->get_fd(&fd_mapping, fdnum, 0);
		if(error != 0) {
			deallocate(message->buf);
			deallocate(message);
			return;
		}
		fd_mapping_t fd_mapping_copy = *fd_mapping;
		auto *fd_item = allocate<linked_list<fd_mapping_t>>(fd_mapping_copy);
		append(&message->fd_list, fd_item);
	}

	auto *message_item = allocate<unixsock_message_list>(message);
	append(&other->recv_messages, message_item);
	other->num_recv_bytes += total_message_size;
	assert(other->num_recv_bytes <= MAX_SIZE_BUFFERS);
	other->recv_signaler.condition_broadcast([&other]() { return other->allocate_current_condition_data(); });
	other->recv_messages_cv.notify();
	other->have_bytes_received();
	out->so_datalen = total_message_size;
	error = 0;
}

void unixsock::have_bytes_received() {
}
