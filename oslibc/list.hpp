#pragma once

#include <stdint.h>
#include <stddef.h>

#include <memory/allocation.hpp>

/* A linked list item. Holds a copy of the input data, so use raw, shared
 * or weak pointers if you need to. When destructed, will only destruct its
 * copy, it won't dereference.
 */
template <typename T>
struct linked_list {
	linked_list() : data() {}
	linked_list(T &d) : data(d) {}
	linked_list(T &&d) : data(move(d)) {}

	T data;
	linked_list<T> *next = nullptr;
};

template <typename T>
inline bool empty(linked_list<T> const *list) {
	return list == nullptr;
}

template <typename T>
inline size_t size(linked_list<T> const *list) {
	size_t s = 0;
	for(; list; list = list->next, ++s) {}
	return s;
}

template <typename T, typename Functor>
inline void iterate(linked_list<T> *list, Functor f) {
	for(; list; list = list->next) {
		f(list);
	}
}

template <typename T, typename Functor>
inline linked_list<T> *find(linked_list<T> *list, Functor f) {
	for(; list; list = list->next) {
		if(f(list)) {
			return list;
		}
	}
	return nullptr;
}

template <typename T, typename Functor>
inline linked_list<T> const *find(linked_list<T> const *list, Functor f) {
	for(; list; list = list->next) {
		if(f(list)) {
			return list;
		}
	}
	return nullptr;
}

template <typename T, typename Functor>
inline bool contains(linked_list<T> const *list, Functor f) {
	return find(list, f) != nullptr;
}

template <typename T>
inline bool contains_object(linked_list<T*> const *list, T const *data) {
	return contains(list, [data](linked_list<T*> const *item) {
		return item->data == data;
	});
}

template <typename T>
inline void append(linked_list<T> **list, linked_list<T> *entry) {
	if(*list == nullptr) {
		*list = entry;
		return;
	}

	for(linked_list<T> *i = *list; i; i = i->next) {
		if(i->next == nullptr) {
			i->next = entry;
			return;
		}
	}
}

template <typename T>
inline void prepend(linked_list<T> **list, linked_list<T> *entry) {
	linked_list<T> *tail = entry;
	for(; tail->next; tail = tail->next) {}

	tail->next = *list;
	*list = entry;
}

/**
 * NOTE: Only use the default_list_deallocator if all linked_list<T> items are
 * allocated with allocate()! It will only deallocate the outer item, not the
 * inner one.
 */
template <typename T>
struct default_list_deallocator {
	void operator()(linked_list<T> *item) {
		cloudos::deallocate(item);
	}
};

template <typename T, typename Functor, typename Deallocator = default_list_deallocator<T>>
inline bool remove_one(linked_list<T> **list, Functor f, Deallocator d = {}) {
	if(*list == nullptr) {
		return false;
	}

	linked_list<T> *prev = nullptr;
	for(linked_list<T> *i = *list; i; prev = i, i = i->next) {
		if(f(i)) {
			if(prev == nullptr) {
				*list = i->next;
			} else {
				prev->next = i->next;
			}
			d(i);
			return true;
		}
	}

	return false;
}

template <typename T, typename Deallocator = default_list_deallocator<T>>
inline bool remove_object(linked_list<T> **list, T const &data, Deallocator d = {}) {
	return remove_one(list, [data](linked_list<T> const *item) {
		return item->data == data;
	}, d);
}

template <typename T, typename Functor, typename Deallocator = default_list_deallocator<T>>
inline size_t remove_all(linked_list<T> **list, Functor f, Deallocator d = {}) {
	if(*list == nullptr) {
		return 0;
	}

	linked_list<T> *prev = nullptr;
	size_t removed = 0;
	for(linked_list<T> *i = *list; i;) {
		if(f(i)) {
			if(prev == nullptr) {
				*list = i->next;
			} else {
				prev->next = i->next;
			}
			linked_list<T> *next = i->next;
			i->next = nullptr;
			d(i);
			i = next;
			++removed;
		} else {
			prev = i;
			i = i->next;
		}
	}

	return removed;
}

template <typename T, typename Deallocator = default_list_deallocator<T>>
inline size_t clear(linked_list<T> **list, Deallocator d = {}) {
	if(*list == nullptr) {
		return 0;
	}

	size_t removed = 0;
	for(linked_list<T> *i = *list; i;) {
		linked_list<T> *next = i->next;
		d(i);
		i = next;
		++removed;
	}

	*list = nullptr;

	return removed;
}

template <typename T>
struct linked_iterator {
	linked_list<T> *item;
};

template <typename T>
inline linked_iterator<T> begin(linked_list<T> *list) {
	return {list};
}

template <typename T>
inline linked_iterator<T> end(linked_list<T> *) {
	return {nullptr};
}

template <typename T>
inline T &operator*(linked_iterator<T> &it) {
	assert(it.item != nullptr);
	return it.item->data;
}

template <typename T>
inline void operator++(linked_iterator<T> &it) {
	assert(it.item != nullptr);
	it.item = it.item->next;
}

template <typename T>
inline bool operator!=(linked_iterator<T> const &lh, linked_iterator<T> const &rh) {
	return lh.item != rh.item;
}

template <typename T>
inline bool operator==(linked_iterator<T> const &lh, linked_iterator<T> const &rh) {
	return lh.item == rh.item;
}
