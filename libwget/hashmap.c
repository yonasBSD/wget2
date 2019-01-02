/*
 * Copyright(c) 2012 Tim Ruehsen
 * Copyright(c) 2015-2019 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <https://www.gnu.org/licenses/>.
 *
 *
 * hashmap routines
 *
 * Changelog
 * 06.11.2012  Tim Ruehsen  created
 *
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <wget.h>
#include "private.h"

typedef struct _entry_st _entry_t;

struct _entry_st {
	void
		*key,
		*value;
	_entry_t
		*next;
	unsigned int
		hash;
};

struct _wget_hashmap_st {
	wget_hashmap_hash_t
		hash; // hash function
	wget_hashmap_compare_t
		cmp; // compare function
	wget_hashmap_key_destructor_t
		key_destructor; // key destructor function
	wget_hashmap_value_destructor_t
		value_destructor; // value destructor function
	_entry_t
		**entry;   // pointer to array of pointers to entries
	int
		max,       // allocated entries
		cur,       // number of entries in use
		threshold; // resize when max reaches threshold
	float
		off,       // resize strategy: >0: resize = off * max, <0: resize = max + (-off)
		factor;
};

/**
 * \file
 * \brief Hashmap functions
 * \defgroup libwget-hashmap Hashmap functions
 * @{
 *
 * Hashmaps are key/value stores that perform at O(1) for insertion, searching and removing.
 */

/**
 * \param[in] max Initial number of pre-allocated entries
 * \param[in] hash Hash function to build hashes from elements
 * \param[in] cmp Comparison function used to find elements
 * \return New hashmap instance
 *
 * Create a new hashmap instance with initial size \p max.
 * It should be free'd after use with wget_hashmap_free().
 *
 * Before the first insertion of an element, \p hash and \p cmp must be set.
 * So if you use %NULL values here, you have to call wget_hashmap_setcmpfunc() and/or
 * wget_hashmap_hashcmpfunc() with appropriate function pointers. No doing so will result
 * in undefined behavior (likely you'll see a segmentation fault).
 */
wget_hashmap_t *wget_hashmap_create(int max, wget_hashmap_hash_t hash, wget_hashmap_compare_t cmp)
{
	wget_hashmap_t *h = xmalloc(sizeof(wget_hashmap_t));

	h->entry = xcalloc(max, sizeof(_entry_t *));
	h->max = max;
	h->cur = 0;
	h->off = 2;
	h->hash = hash;
	h->cmp = cmp;
	h->key_destructor = free;
	h->value_destructor = free;
	h->factor = 0.75;
	h->threshold = (int)(max * h->factor);

	return h;
}

G_GNUC_WGET_NONNULL_ALL
static _entry_t * hashmap_find_entry(const wget_hashmap_t *h, const char *key, unsigned int hash)
{
	for (_entry_t * e = h->entry[hash % h->max]; e; e = e->next) {
		if (hash == e->hash && (key == e->key || !h->cmp(key, e->key))) {
			return e;
		}
	}

	return NULL;
}

G_GNUC_WGET_NONNULL_ALL
static void hashmap_rehash(wget_hashmap_t *h, int newmax, int recalc_hash)
{
	_entry_t **new_entry, *entry, *next;
	int cur = h->cur;

	if (cur) {
		int pos;
		new_entry = xcalloc(newmax, sizeof(_entry_t *));

		for (int it = 0; it < h->max && cur; it++) {
			for (entry = h->entry[it]; entry; entry = next) {
				next = entry->next;

				// now move entry from 'h' to 'new_hashmap'
				if (recalc_hash)
					entry->hash = h->hash(entry->key);
				pos = entry->hash % newmax;
				entry->next = new_entry[pos];
				new_entry[pos] = entry;

				cur--;
			}
		}

		xfree(h->entry);
		h->entry = new_entry;
		h->max = newmax;
		h->threshold = (int)(newmax * h->factor);
	}
}

G_GNUC_WGET_NONNULL((1,3))
static void hashmap_new_entry(wget_hashmap_t *h, unsigned int hash, const char *key, const char *value)
{
	_entry_t *entry;
	int pos = hash % h->max;

	entry = xmalloc(sizeof(_entry_t));
	entry->key = (void *)key;
	entry->value = (void *)value;
	entry->hash = hash;
	entry->next = h->entry[pos];
	h->entry[pos] = entry;

	if (++h->cur >= h->threshold) {
		int newsize;

		if (h->off > 0) {
			newsize = (int) (h->max * h->off);
		} else if (h->off < 0) {
			newsize = (int) (h->max - h->off);
		} else {
			newsize = 0; // resizing switched off
		}

		if (newsize > 0)
			hashmap_rehash(h, newsize, 0);
	}
}

/**
 * \param[in] h Hashmap to put data into
 * \param[in] key Key to insert into \p h
 * \param[in] value Value to insert into \p h
 * \return 0 if inserted a new entry, 1 if entry existed
 *
 * Insert a key/value pair into hashmap \p h.
 *
 * \p key and \p value are *not* cloned, the hashmap takes 'ownership' of both.
 *
 * If \p key already exists and the pointer values the old and the new key differ,
 * the old key will be destroyed by calling the key destructor function (default is free()).
 *
 * To realize a hashset (just keys without values), \p value may be %NULL.
 *
 * Neither \p h nor \p key must be %NULL.
 */
int wget_hashmap_put_noalloc(wget_hashmap_t *h, const void *key, const void *value)
{
	if (h && key) {
		_entry_t *entry;
		unsigned int hash = h->hash(key);

		if ((entry = hashmap_find_entry(h, key, hash))) {
			if (entry->key != key && entry->key != value) {
				if (h->key_destructor)
					h->key_destructor(entry->key);
				if (entry->key == entry->value)
					entry->value = NULL;
			}
			if (entry->value != value && entry->value != key) {
				if (h->value_destructor)
					h->value_destructor(entry->value);
			}

			entry->key = (void *) key;
			entry->value = (void *) value;

			return 1;
		}

		// a new entry
		hashmap_new_entry(h, hash, key, value);
	}

	return 0;
}

/**
 * \param[in] h Hashmap to put data into
 * \param[in] key Key to insert into \p h
 * \param[in] keysize Size of \p key
 * \param[in] value Value to insert into \p h
 * \param[in] valuesize Size of \p value
 * \return 0 if inserted a new entry, 1 if entry existed
 *
 * Insert a key/value pair into hashmap \p h.
 *
 * If \p key already exists it will not be cloned. In this case the value destructor function
 * will be called with the old value and the new value will be shallow cloned.
 *
 * If \p doesn't exist, both \p key and \p value will be shallow cloned.
 *
 * To realize a hashset (just keys without values), \p value may be %NULL.
 *
 * Neither \p h nor \p key must be %NULL.
 */
int wget_hashmap_put(wget_hashmap_t *h, const void *key, size_t keysize, const void *value, size_t valuesize)
{
	if (h && key) {
		_entry_t *entry;
		unsigned int hash = h->hash(key);

		if ((entry = hashmap_find_entry(h, key, hash))) {
			if (h->value_destructor)
				h->value_destructor(entry->value);

			entry->value = wget_memdup(value, valuesize);

			return 1;
		}

		// a new entry
		hashmap_new_entry(h, hash, wget_memdup(key, keysize), wget_memdup(value, valuesize));
	}

	return 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] key Key to search for
 * \param[out] value Value to be returned
 * \return 1 if \p key has been found, 0 if not found
 *
 * Get the value for a given key.
 *
 * Neither \p h nor \p key must be %NULL.
 */
int wget_hashmap_get(const wget_hashmap_t *h, const void *key, void **value)
{
	if (h && key) {
		_entry_t *entry;

		if ((entry = hashmap_find_entry(h, key, h->hash(key)))) {
			if (value)
				*value = entry->value;
			return 1;
		}
	}

	return 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] key Key to search for
 * \return 1 if \p key has been found, 0 if not found
 *
 * Check if \p key exists in \p h.
 */
int wget_hashmap_contains(const wget_hashmap_t *h, const void *key)
{
	return wget_hashmap_get(h, key, NULL);
}

G_GNUC_WGET_NONNULL_ALL
static int hashmap_remove_entry(wget_hashmap_t *h, const char *key, int free_kv)
{
	_entry_t *entry, *next, *prev = NULL;
	unsigned int hash = h->hash(key);
	int pos = hash % h->max;

	for (entry = h->entry[pos]; entry; prev = entry, entry = next) {
		next = entry->next;

		if (hash == entry->hash && (key == entry->key || !h->cmp(key, entry->key))) {
			if (prev)
				prev->next = next;
			else
				h->entry[pos] = next;

			if (free_kv) {
				if (h->key_destructor)
					h->key_destructor(entry->key);
				if (entry->value != entry->key) {
					if (h->value_destructor)
						h->value_destructor(entry->value);
				}
				entry->key = NULL;
				entry->value = NULL;
			}
			xfree(entry);

			h->cur--;
			return 1;
		}
	}

	return 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] key Key to be removed
 * \return 1 if \p key has been removed, 0 if not found
 *
 * Remove \p key from hashmap \p h.
 *
 * If \p key is found, the key and value destructor functions are called
 * when removing the entry from the hashmap.
 */
int wget_hashmap_remove(wget_hashmap_t *h, const void *key)
{
	if (h && key)
		return hashmap_remove_entry(h, key, 1);
	else
		return 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] key Key to be removed
 * \return 1 if \p key has been removed, 0 if not found
 *
 * Remove \p key from hashmap \p h.
 *
 * Key and value destructor functions are *not* called when removing the entry from the hashmap.
 */
int wget_hashmap_remove_nofree(wget_hashmap_t *h, const void *key)
{
	if (h && key)
		return hashmap_remove_entry(h, key, 0);
	else
		return 0;
}

/**
 * \param[in] h Hashmap to be free'd
 *
 * Remove all entries from hashmap \p h and free the hashmap instance.
 *
 * Key and value destructor functions are called for each entry in the hashmap.
 */
void wget_hashmap_free(wget_hashmap_t **h)
{
	if (h && *h) {
		wget_hashmap_clear(*h);
		xfree((*h)->entry);
		xfree(*h);
	}
}

/**
 * \param[in] h Hashmap to be cleared
 *
 * Remove all entries from hashmap \p h.
 *
 * Key and value destructor functions are called for each entry in the hashmap.
 */
void wget_hashmap_clear(wget_hashmap_t *h)
{
	if (h) {
		_entry_t *entry, *next;
		int it, cur = h->cur;

		for (it = 0; it < h->max && cur; it++) {
			for (entry = h->entry[it]; entry; entry = next) {
				next = entry->next;

				if (h->key_destructor)
					h->key_destructor(entry->key);

				// free value if different from key
				if (entry->value != entry->key && h->value_destructor)
					h->value_destructor(entry->value);

				entry->key = NULL;
				entry->value = NULL;

				xfree(entry);
				cur--;
			}
			h->entry[it] = NULL;
		}
		h->cur = 0;
	}
}

/**
 * \param[in] h Hashmap
 * \return Number of entries in hashmap \p h
 *
 * Return the number of entries in the hashmap \p h.
 */
int wget_hashmap_size(const wget_hashmap_t *h)
{
	return h ? h->cur : 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] browse Function to be called for each element of \p h
 * \param[in] ctx Context variable use as param to \p browse
 * \return Return value of the last call to \p browse
 *
 * Call function \p browse for each element of hashmap \p h or until \p browse
 * returns a value not equal to zero.
 *
 * \p browse is called with \p ctx and the pointer to the current element.
 *
 * The return value of the last call to \p browse is returned or 0 if either \p h or \p browse is %NULL.
 */
int wget_hashmap_browse(const wget_hashmap_t *h, wget_hashmap_browse_t browse, void *ctx)
{
	if (h && browse) {
		_entry_t *entry;
		int it, ret, cur = h->cur;

		for (it = 0; it < h->max && cur; it++) {
			for (entry = h->entry[it]; entry; entry = entry->next) {
				if ((ret = browse(ctx, entry->key, entry->value)) != 0)
					return ret;
				cur--;
			}
		}
	}

	return 0;
}

/**
 * \param[in] h Hashmap
 * \param[in] cmp Comparison function used to find keys
 *
 * Set the comparison function.
 */
void wget_hashmap_setcmpfunc(wget_hashmap_t *h, wget_hashmap_compare_t cmp)
{
	if (h)
		h->cmp = cmp;
}

/**
 * \param[in] h Hashmap
 * \param[in] hash Hash function used to hash keys
 *
 * Set the key hash function.
 *
 * The keys of all entries in the hashmap will be hashed again.
 */
void wget_hashmap_sethashfunc(wget_hashmap_t *h, wget_hashmap_hash_t hash)
{
	if (h) {
		h->hash = hash;

		hashmap_rehash(h, h->max, 1);
	}
}

/**
 * \param[in] h Hashmap
 * \param[in] destructor Destructor function for keys
 *
 * Set the key destructor function.
 *
 * Default is free().
 */
void wget_hashmap_set_key_destructor(wget_hashmap_t *h, wget_hashmap_key_destructor_t destructor)
{
	if (h)
		h->key_destructor = destructor;
}

/**
 * \param[in] h Hashmap
 * \param[in] destructor Destructor function for values
 *
 * Set the value destructor function.
 *
 * Default is free().
 */
void wget_hashmap_set_value_destructor(wget_hashmap_t *h, wget_hashmap_value_destructor_t destructor)
{
	if (h)
		h->value_destructor = destructor;
}

/**
 * \param[in] h Hashmap
 * \param[in] factor The load factor
 *
 * Set the load factor function.
 *
 * The load factor is determines when to resize the internal memory.
 * 0.75 means "resize if 75% or more of all slots are used".
 *
 * The resize strategy is set by wget_hashmap_set_growth_policy().
 *
 * The resize (and rehashing) occurs earliest on the next insertion of a new key.
 *
 * Default is 0.75.
 */
void wget_hashmap_setloadfactor(wget_hashmap_t *h, float factor)
{
	if (h) {
		h->factor = factor;
		h->threshold = (int)(h->max * h->factor);
		// rehashing occurs earliest on next put()
	}
}

/**
 * \param[in] h Hashmap
 * \param[in] off Hashmap growth mode:
 *   positive values: increase size by multiplying \p off, e.g. 2 doubles the size on each resize
 *   negative values: increase size by \p -off entries on each resize (the integer value is taken).
 *   0: switch off resizing
 *
 * Set the growth policy for internal memory.
 *
 * Default is 2.
 */
void wget_hashmap_set_growth_policy(wget_hashmap_t *h, float off)
{
	if (!h)
		return;

	h->off = off;
}

/**@}*/
