/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Multi-packet state handling
 * @file src/lib/server/state.c
 *
 * @ingroup AVP
 *
 * For each round of a multi-round authentication method such as EAP,
 * or a 2FA method such as OTP, a state entry will be created.  The state
 * entry holds data that should be available during the complete lifecycle
 * of the authentication attempt.
 *
 * When a request is complete, #fr_request_to_state is called to transfer
 * ownership of the state fr_pair_ts and state_ctx (which the fr_pair_ts
 * are allocated in) to a #fr_state_entry_t.  This #fr_state_entry_t holds the
 * value of the State attribute, that will be send out in the response.
 *
 * When the next request is received, #fr_state_to_request is called to transfer
 * the fr_pair_ts and state ctx to the new request.
 *
 * The ownership of the state_ctx and state fr_pair_ts is transferred as below:
 *
 * @verbatim
   request -> state_entry -> request -> state_entry -> request -> free()
          \-> reply                 \-> reply                 \-> access-reject/access-accept
 * @endverbatim
 *
 * @copyright 2014 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/request.h>
#include <freeradius-devel/server/state.h>

#include <freeradius-devel/io/listen.h>

#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/dlist.h>
#include <freeradius-devel/util/md5.h>
#include <freeradius-devel/util/misc.h>
#include <freeradius-devel/util/rand.h>

/** Holds a state value, and associated fr_pair_ts and data
 *
 */
typedef struct {
	uint64_t		id;				//!< State number within state heap.
	fr_rb_node_t		node;				//!< Entry in the state rbtree.
	union {
		/** Server ID components
		 *
		 * State values should be unique to a given server
		 */
		struct state_comp {
			uint8_t		tries;			//!< Number of rounds so far in this state sequence.
			uint8_t		tx;			//!< Bits changed in the tries counter for this round.
			uint8_t		r_0;			//!< Random component.
			uint8_t		server_id;		//!< Configured server ID.  Used for debugging
								//!< to locate authentication sessions originating
								//!< from a particular backend authentication server.

			uint32_t	context_id;		//!< Hash of the current virtual server, xor'd with
								//!< r1, r2, r3, r4 after the original state value
								//!< is sent, but before the state entry is inserted
								//!< into the tree.  The receiving virtual server
								//!< xor's its hash with the received state before
								//!< performing the lookup.  This means one virtual
								//!< server can't act on a state entry generated by
								//!< another, even though the state tree is global
								//!< to all virtual servers.

			uint8_t		vx_0;			//!< Random component.
			uint8_t		r_5;			//!< Random component.
			uint8_t		vx_1;			//!< Random component.
			uint8_t		r_6;			//!< Random component.

			uint8_t		vx_2;			//!< Random component.
			uint8_t		r_7;			//!< Random component.
			uint8_t		r_8;			//!< Random component.
			uint8_t		r_9;			//!< Random component.
		} state_comp;

		uint8_t		state[sizeof(struct state_comp)];	//!< State value in binary.
	};

	uint64_t		seq_start;			//!< Number of first request in this sequence.
	fr_time_t		cleanup;			//!< When this entry should be cleaned up.

	/*
	 *	Should only even be in one at a time
	 */
	union {
		fr_dlist_t		expire_entry;		//!< Entry in the list of things to expire.
		fr_dlist_t		free_entry;		//!< Entry in the list of things to free.
	};

	int			tries;

	fr_pair_t		*ctx;				//!< for all session specific data.

	fr_dlist_head_t		data;				//!< Persistable request data, also parented by ctx.

	request_t		*thawed;			//!< The request that thawed this entry.

	fr_state_tree_t		*state_tree;			//!< Tree this entry belongs to.
} fr_state_entry_t;

/** A child of a fr_state_entry_t
 *
 * Children are tracked using the request data of parents.
 *
 * request data is added with identifiers that uniquely identify the
 * subrequest it should be restored to.
 *
 * In this way a top level fr_state_entry_t can hold the session
 * information for multiple children, and the children may hold
 * state_child_entry_ts for grandchildren.
 */
typedef struct {
	fr_pair_t		*ctx;				//!< for all session specific data.

	fr_dlist_head_t		data;				//!< Persistable request data, also parented by ctx.

	request_t		*thawed;			//!< The request that thawed this entry.
} state_child_entry_t;

struct fr_state_tree_s {
	uint64_t		id;				//!< Next ID to assign.
	uint64_t		timed_out;			//!< Number of states that were cleaned up due to
								//!< timeout.
	uint32_t		max_sessions;			//!< Maximum number of sessions we track.
	uint32_t		used_sessions;			//!< How many sessions are currently in progress.
	fr_rb_tree_t		*tree;				//!< rbtree used to lookup state value.
	fr_dlist_head_t		to_expire;			//!< Linked list of entries to free.

	fr_time_delta_t		timeout;			//!< How long to wait before cleaning up state entires.

	bool			thread_safe;			//!< Whether we lock the tree whilst modifying it.
	pthread_mutex_t		mutex;				//!< Synchronisation mutex.

	uint8_t			server_id;			//!< ID to use for load balancing.
	uint32_t		context_id;			//!< ID binding state values to a context such
								///< as a virtual server.

	fr_dict_attr_t const	*da;				//!< State attribute used.
};

#define PTHREAD_MUTEX_LOCK if (state->thread_safe) pthread_mutex_lock
#define PTHREAD_MUTEX_UNLOCK if (state->thread_safe) pthread_mutex_unlock

static void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry);

/** Compare two fr_state_entry_t based on their state value i.e. the value of the attribute
 *
 */
static int8_t state_entry_cmp(void const *one, void const *two)
{
	fr_state_entry_t const *a = one, *b = two;
	int ret;

	ret = memcmp(a->state, b->state, sizeof(a->state));
	return CMP(ret, 0);
}

/** Free the state tree
 *
 */
static int _state_tree_free(fr_state_tree_t *state)
{
	fr_state_entry_t *entry;

	if (state->thread_safe) pthread_mutex_destroy(&state->mutex);

	DEBUG4("Freeing state tree %p", state);

	while ((entry = fr_dlist_head(&state->to_expire))) {
		DEBUG4("Freeing state entry %p (%"PRIu64")", entry, entry->id);
		state_entry_unlink(state, entry);
		talloc_free(entry);
	}

	/*
	 *	Free the rbtree
	 */
	talloc_free(state->tree);

	return 0;
}

/** Initialise a new state tree
 *
 * @param[in] ctx		to link the lifecycle of the state tree to.
 * @param[in] da		Attribute used to store and retrieve state from.
 * @param[in] thread_safe		Whether we should mutex protect the state tree.
 * @param[in] max_sessions	we track state for.
 * @param[in] timeout		How long to wait before cleaning up entries.
 * @param[in] server_id		ID byte to use in load-balancing operations.
 * @param[in] context_id	Specifies a unique ctx id to prevent states being
 *				used in contexts for which they weren't intended.
 * @return
 *	- A new state tree.
 *	- NULL on failure.
 */
fr_state_tree_t *fr_state_tree_init(TALLOC_CTX *ctx, fr_dict_attr_t const *da, bool thread_safe,
				    uint32_t max_sessions, fr_time_delta_t timeout,
				    uint8_t server_id, uint32_t context_id)
{
	fr_state_tree_t *state;

	state = talloc_zero(NULL, fr_state_tree_t);
	if (!state) return 0;

	state->max_sessions = max_sessions;
	state->timeout = timeout;

	/*
	 *	Create a break in the contexts.
	 *	We still want this to be freed at the same time
	 *	as the parent, but we also need it to be thread
	 *	safe, and multiple threads could be using the
	 *	tree.
	 */
	talloc_link_ctx(ctx, state);

	if (thread_safe && (pthread_mutex_init(&state->mutex, NULL) != 0)) {
		talloc_free(state);
		return NULL;
	}

	fr_dlist_talloc_init(&state->to_expire, fr_state_entry_t, free_entry);

	/*
	 *	We need to do controlled freeing of the
	 *	rbtree, so that all the state entries
	 *	are freed before it's destroyed.  Hence
	 *	it being parented from the NULL ctx.
	 */
	state->tree = fr_rb_inline_talloc_alloc(NULL, fr_state_entry_t, node, state_entry_cmp, NULL);
	if (!state->tree) {
		talloc_free(state);
		return NULL;
	}
	talloc_set_destructor(state, _state_tree_free);

	state->da = da;		/* Remember which attribute we use to load/store state */
	state->server_id = server_id;
	state->context_id = context_id;
	state->thread_safe = thread_safe;

	return state;
}

/** Unlink an entry and remove if from the tree
 *
 */
static inline CC_HINT(always_inline)
void state_entry_unlink(fr_state_tree_t *state, fr_state_entry_t *entry)
{
	/*
	 *	Check the memory is still valid
	 */
	(void) talloc_get_type_abort(entry, fr_state_entry_t);

	fr_dlist_remove(&state->to_expire, entry);
	fr_rb_delete(state->tree, entry);

	DEBUG4("State ID %" PRIu64 " unlinked", entry->id);
}

/** Frees any data associated with a state
 *
 */
static int _state_entry_free(fr_state_entry_t *entry)
{
#ifdef WITH_VERIFY_PTR
	fr_dcursor_t cursor;
	fr_pair_t *vp;

	/*
	 *	Verify all state attributes are parented
	 *	by the state context.
	 */
	if (entry->ctx) {
		for (vp = fr_dcursor_init(&cursor, fr_pair_list_order(&entry->ctx->children));
		     vp;
		     vp = fr_dcursor_next(&cursor)) {
			fr_assert(entry->ctx == talloc_parent(vp));
		}
	}

	/*
	 *	Ensure any request data is parented by us
	 *	so we know it'll be cleaned up.
	 */
	(void)fr_cond_assert(request_data_verify_parent(entry->ctx, &entry->data));
#endif

	/*
	 *	Should also free any state attributes
	 */
	if (entry->ctx) TALLOC_FREE(entry->ctx);

	DEBUG4("State ID %" PRIu64 " freed", entry->id);

	entry->state_tree->used_sessions--;

	return 0;
}

/** Create a new state entry
 *
 * @note Called with the mutex held.
 */
static fr_state_entry_t *state_entry_create(fr_state_tree_t *state, request_t *request,
					    fr_pair_list_t *reply_list, fr_state_entry_t *old)
{
	size_t			i;
	uint32_t		x;
	fr_time_t		now = fr_time();
	fr_pair_t		*vp;
	fr_state_entry_t	*entry, *next;

	uint8_t			old_state[sizeof(old->state)];
	int			old_tries = 0;
	uint64_t		timed_out = 0;
	bool			too_many = false;
	fr_dlist_head_t		to_free;

	/*
	 *	Shouldn't be in any lists if it's being reused
	 */
	fr_assert(!old ||
		  (!fr_dlist_entry_in_list(&old->expire_entry) &&
		   !fr_rb_node_inline_in_tree(&old->node)));

	fr_dlist_init(&to_free, fr_state_entry_t, free_entry);

	/*
	 *	Clean up expired entries
	 */
	for (entry = fr_dlist_head(&state->to_expire);
	     entry != NULL;
	     entry = next) {
 		(void)talloc_get_type_abort(entry, fr_state_entry_t);	/* Allow examination */
		next = fr_dlist_next(&state->to_expire, entry);		/* Advance *before* potential unlinking */

		if (entry == old) continue;

		/*
		 *	Too old, we can delete it.
		 */
		if (fr_time_lt(entry->cleanup, now)) {
			state_entry_unlink(state, entry);
			fr_dlist_insert_tail(&to_free, entry);
			timed_out++;
			continue;
		}

		break;
	}

	state->timed_out += timed_out;

	if (!old) {
		too_many = (state->used_sessions == (uint32_t) state->max_sessions);
		if (!too_many) state->used_sessions++;	/* preemptively increment whilst we hold the mutex */
	} else {
		old_tries = old->tries;
		memcpy(old_state, old->state, sizeof(old_state));
	}

	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	if (timed_out > 0) RWDEBUG("Cleaning up %"PRIu64" timed out state entries", timed_out);

	/*
	 *	Now free the unlinked entries.
	 *
	 *	We do it here as freeing may involve significantly more
	 *	work than just freeing the data.
	 *
	 *	If there's request data that was persisted it will now
	 *	be freed also, and it may have complex destructors associated
	 *	with it.
	 */
	while ((entry = fr_dlist_head(&to_free)) != NULL) {
		fr_dlist_remove(&to_free, entry);
		talloc_free(entry);
	}

	/*
	 *	Have to do this post-cleanup, else we end up returning with
	 *	a list full of entries to free with none of them being
	 *	freed which is bad...
	 */
	if (too_many) {
		RERROR("Failed inserting state entry - At maximum ongoing session limit (%u)",
		       state->max_sessions);
		PTHREAD_MUTEX_LOCK(&state->mutex);	/* Caller expects this to be locked */
		return NULL;
	}

	/*
	 *	Allocation doesn't need to occur inside the critical region
	 *	and would add significantly to contention.
	 */
	if (!old) {
		MEM(entry = talloc_zero(NULL, fr_state_entry_t));
		talloc_set_destructor(entry, _state_entry_free);
		/* tree->used_sessions incremented above */
	/*
	 *	Reuse the old state entry cleaning up any memory associated
	 *	with it.
	 */
	} else {
		_state_entry_free(old);
		talloc_free_children(old);
		memset(old, 0, sizeof(*old));
		entry = old;
	}

	entry->state_tree = state;

	request_data_list_init(&entry->data);

	entry->id = state->id++;

	/*
	 *	Limit the lifetime of this entry based on how long the
	 *	server takes to process a request.  Doing it this way
	 *	isn't perfect, but it's reasonable, and it's one less
	 *	thing for an administrator to configure.
	 */
	entry->cleanup = fr_time_add(now, state->timeout);

	/*
	 *	Some modules create their own magic
	 *	state attributes.  If a state value already exists
	 *	int the reply, we use that in preference to the
	 *	old state.
	 */
	vp = fr_pair_find_by_da(reply_list, state->da, 0);
	if (vp) {
		if (DEBUG_ENABLED && (vp->vp_length > sizeof(entry->state))) {
			WARN("State too long, will be truncated.  Expected <= %zd bytes, got %zu bytes",
			     sizeof(entry->state), vp->vp_length);
		}

		/*
		 *	Assume our own State first.
		 */
		if (vp->vp_length == sizeof(entry->state)) {
			memcpy(entry->state, vp->vp_octets, sizeof(entry->state));

		/*
		 *	Too big?  Get the MD5 hash, in order
		 *	to depend on the entire contents of State.
		 */
		} else if (vp->vp_length > sizeof(entry->state)) {
			fr_md5_calc(entry->state, vp->vp_octets, vp->vp_length);

			/*
			 *	Too small?  Use the whole thing, and
			 *	set the rest of my_entry.state to zero.
			 */
		} else {
			memcpy(entry->state, vp->vp_octets, vp->vp_length);
			memset(&entry->state[vp->vp_length], 0, sizeof(entry->state) - vp->vp_length);
		}
	} else {
		/*
		 *	16 octets of randomness should be enough to
		 *	have a globally unique state.
		 */
		if (old) {
			memcpy(entry->state, old_state, sizeof(entry->state));
			entry->tries = old_tries + 1;
		/*
		 *	Base the new state on the old state if we had one.
		 */
		} else {
			for (i = 0; i < sizeof(entry->state) / sizeof(x); i++) {
				x = fr_rand();
				memcpy(entry->state + (i * 4), &x, sizeof(x));
			}
		}

		entry->state_comp.tries = entry->tries + 1;

		entry->state_comp.tx = entry->state_comp.tries ^ entry->tries;

		entry->state_comp.vx_0 = entry->state_comp.r_0 ^
					 ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 16) & 0xff);
		entry->state_comp.vx_1 = entry->state_comp.r_0 ^
					 ((((uint32_t) HEXIFY(RADIUSD_VERSION)) >> 8) & 0xff);
		entry->state_comp.vx_2 = entry->state_comp.r_0 ^
					 (((uint32_t) HEXIFY(RADIUSD_VERSION)) & 0xff);

		/*
		 *	Allow a portion of the State attribute to be set,
		 *	this is useful for debugging purposes.
		 */
		entry->state_comp.server_id = state->server_id;

		MEM(vp = fr_pair_afrom_da(request->reply_ctx, state->da));
		fr_pair_value_memdup(vp, entry->state, sizeof(entry->state), false);
		fr_pair_append(reply_list, vp);
	}

	DEBUG4("State ID %" PRIu64 " created, value 0x%pH, expires %pV",
	       entry->id, fr_box_octets(entry->state, sizeof(entry->state)),
	       fr_box_time_delta(fr_time_sub(entry->cleanup, now)));

	PTHREAD_MUTEX_LOCK(&state->mutex);

	/*
	 *	XOR the server hash with four bytes of random data.
	 *	We XOR is again before resolving, to ensure state lookups
	 *	only succeed in the virtual server that created the state
	 *	value.
	 */
	*((uint32_t *)(&entry->state_comp.context_id)) ^= state->context_id;

	if (!fr_rb_insert(state->tree, entry)) {
		RERROR("Failed inserting state entry - Insertion into state tree failed");
		fr_pair_delete_by_da(reply_list, state->da);
		talloc_free(entry);
		return NULL;
	}

	/*
	 *	Link it to the end of the list, which is implicitely
	 *	ordered by cleanup time.
	 */
	fr_dlist_insert_tail(&state->to_expire, entry);

	return entry;
}

/** Find the entry based on the State attribute and remove it from the state tree
 *
 */
static fr_state_entry_t *state_entry_find_and_unlink(fr_state_tree_t *state, fr_value_box_t const *vb)
{
	fr_state_entry_t *entry, my_entry;

	/*
	 *	Assume our own State first.
	 */
	if (vb->vb_length == sizeof(my_entry.state)) {
		memcpy(my_entry.state, vb->vb_octets, sizeof(my_entry.state));

		/*
		 *	Too big?  Get the MD5 hash, in order
		 *	to depend on the entire contents of State.
		 */
	} else if (vb->vb_length > sizeof(my_entry.state)) {
		fr_md5_calc(my_entry.state, vb->vb_octets, vb->vb_length);

		/*
		 *	Too small?  Use the whole thing, and
		 *	set the rest of my_entry.state to zero.
		 */
	} else {
		memcpy(my_entry.state, vb->vb_octets, vb->vb_length);
		memset(&my_entry.state[vb->vb_length], 0, sizeof(my_entry.state) - vb->vb_length);
	}

	/*
	 *	Make it unique for different virtual servers handling the same request
	 */
	my_entry.state_comp.context_id ^= state->context_id;

	entry = fr_rb_remove(state->tree, &my_entry);
	if (entry) {
		(void) talloc_get_type_abort(entry, fr_state_entry_t);
		fr_dlist_remove(&state->to_expire, entry);
	}

	return entry;
}

/** Called when sending an Access-Accept/Access-Reject to discard state information
 *
 */
void fr_state_discard(fr_state_tree_t *state, request_t *request)
{
	fr_state_entry_t	*entry;
	fr_pair_t		*vp;

	vp = fr_pair_find_by_da(&request->request_pairs, state->da, 0);
	if (!vp) return;

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find_and_unlink(state, &vp->data);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return;
	}
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	/*
	 *	If fr_state_to_request was never called, this ensures
	 *	the state owned by entry is freed, otherwise this is
	 *	mostly a NOOP, other than freeing the memory held by
	 *	the entry.
	 */
	TALLOC_FREE(entry);

	/*
	 *	If fr_state_to_request was called, then the request
	 *	holds the state data, and we need to destroy it
	 *	and return the request to the state it was in when
	 *	it was first alloced, just in case a user does something
	 *	stupid like add additional session-state attributes
	 *	in  one of the later sections.
	 */
	TALLOC_FREE(request->session_state_ctx);

	MEM(request->session_state_ctx = fr_pair_afrom_da(NULL, request_attr_state));

	RDEBUG3("%s - discarded", state->da->name);

	return;
}

/** Copy a pointer to the head of the list of state fr_pair_ts (and their ctx) into the request
 *
 * @note Does not copy the actual fr_pair_ts.  The fr_pair_ts and their context
 *	are transferred between state entries as the conversation progresses.
 *
 * @note Called with the mutex free.
 *
 * @param[in] state	tree to lookup state in.
 * @param[in] request	to restore state for.
 * @return
 *	- 2 if the state attribute didn't match any known states.
 *	- 1 if no state attribute existed.
 *	- 0 on success (state restored)
 *	- -1 if a state entry has already been thawed by a another request.
 */
int fr_state_to_request(fr_state_tree_t *state, request_t *request)
{
	fr_state_entry_t	*entry;
	TALLOC_CTX		*old_ctx = NULL;
	fr_pair_t		*vp;

	/*
	 *	No State, don't do anything.
	 */
	vp = fr_pair_find_by_da(&request->request_pairs, state->da, 0);
	if (!vp) {
		RDEBUG3("No &request.%s attribute, can't restore &session-state", state->da->name);
		if (request->seq_start == 0) request->seq_start = request->number;	/* Need check for fake requests */
		return 1;
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);
	entry = state_entry_find_and_unlink(state, &vp->data);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		RDEBUG2("No state entry matching &request.%pP found", vp);
		return 2;
	}
   	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	/* Probably impossible in the current code */
	if (unlikely(entry->thawed != NULL)) {
		RERROR("State entry has already been thawed by a request %"PRIu64, entry->thawed->number);
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		return -2;
	}
	if (request->session_state_ctx) old_ctx = request->session_state_ctx;	/* Store for later freeing */

	fr_assert(entry->ctx);

	request->seq_start = entry->seq_start;
	request->session_state_ctx = entry->ctx;

	/*
	 *	Associate old state with the request
	 *
	 *	If the request is freed, it's freed immediately.
	 *
	 *	Otherwise, if there's another round, we re-use
	 *	the state entry and insert it back into the
	 *	tree.
	 */
	request_data_add(request, state, 0, entry, true, true, false);
	request_data_restore(request, &entry->data);

	entry->ctx = NULL;
	entry->thawed = request;

	if (!fr_pair_list_empty(&request->session_state_pairs)) {
		RDEBUG2("Restored &session-state");
		log_request_pair_list(L_DBG_LVL_2, request, NULL, &request->session_state_pairs, "&session-state.");
	}

	/*
	 *	Free this outside of the mutex for less contention.
	 */
	if (old_ctx) talloc_free(old_ctx);

	RDEBUG3("%s - restored", state->da->name);

	/*
	 *	Set sequence so that we can prioritize ongoing multi-packet sessions.
	 */
	request->async->sequence = entry->tries;
	REQUEST_VERIFY(request);
	return 0;
}


/** Transfer ownership of the state fr_pair_ts and ctx, back to a state entry
 *
 * Put request->session_state_pairs into the State attribute.  Put the State attribute
 * into the vps list.  Delete the original entry, if it exists
 *
 * Also creates a new state entry.
 */
int fr_request_to_state(fr_state_tree_t *state, request_t *request)
{
	fr_state_entry_t	*entry, *old;
	fr_dlist_head_t		data;

	old = request_data_get(request, state, 0);
	request_data_list_init(&data);
	request_data_by_persistance(&data, request, true);

	if (fr_pair_list_empty(&request->session_state_pairs) && fr_dlist_empty(&data)) return 0;

	if (!fr_pair_list_empty(&request->session_state_pairs)) {
		RDEBUG2("Saving &session-state");
		log_request_pair_list(L_DBG_LVL_2, request, NULL, &request->session_state_pairs, "&session-state.");
	}

	PTHREAD_MUTEX_LOCK(&state->mutex);

	/*
	 *	Reuses old if possible
	 */
	entry = state_entry_create(state, request, &request->reply_pairs, old);
	if (!entry) {
		PTHREAD_MUTEX_UNLOCK(&state->mutex);
		RERROR("Creating state entry failed");
		request_data_restore(request, &data);	/* Put it back again */
		return -1;
	}

	fr_assert(entry->ctx == NULL);
	fr_assert(request->session_state_ctx);

	entry->seq_start = request->seq_start;
	entry->ctx = request->session_state_ctx;
	fr_dlist_move(&entry->data, &data);
	PTHREAD_MUTEX_UNLOCK(&state->mutex);

	MEM(request->session_state_ctx = fr_pair_afrom_da(NULL, request_attr_state));	/* fixme - should use a pool */

	RDEBUG3("%s - saved", state->da->name);
	REQUEST_VERIFY(request);

	return 0;
}

/** Free any subrequest request data if the dlist head is freed
 *
 */
static int _free_child_data(state_child_entry_t *child_entry)
{
	fr_dlist_talloc_free(&child_entry->data);
	talloc_free(child_entry->ctx);		/* Free the child's session_state_ctx if we own it */

	return 0;
}

/** Store subrequest's session-state list and persistable request data in its parent
 *
 * @param[in] child		The child request to retrieve state from.
 * @param[in] unique_ptr	A parent may have multiple subrequests spawned
 *				by different modules.  This identifies the module
 *      			or other facility that spawned the subrequest.
 * @param[in] unique_int	Further identification.
 */
void fr_state_store_in_parent(request_t *child, void const *unique_ptr, int unique_int)
{
	state_child_entry_t	*child_entry;
	request_t		*request = child; /* Stupid logging */

	if (!fr_cond_assert_msg(child->parent,
				"Child request must have request->parent set when storing state")) return;

	RDEBUG3("Storing subrequest state in request %s", child->parent->name);

	if ((request_data_by_persistance_count(request, true) > 0) ||
		!fr_pair_list_empty(&request->session_state_pairs)) {
		MEM(child_entry = talloc_zero(request->parent->session_state_ctx, state_child_entry_t));
		request_data_list_init(&child_entry->data);
		talloc_set_destructor(child_entry, _free_child_data);

		child_entry->ctx = child->session_state_ctx;

		/*
		 *	Pull everything out of the child,
		 *	add it to our temporary list head...
		 *
		 *	request_data_add allocs persistable
		 *	request dta in the session_state_ctx
		 *	which is why we don't need to copy or
		 *	reparent any of this.
		 */
		request_data_by_persistance(&child_entry->data, request, true);

		/*
		 *	...and add the request_data from
		 *	the child back into the parent.
		 */
		request_data_talloc_add(request->parent, unique_ptr, unique_int,
					state_child_entry_t, child_entry, true, false, true);

		/*
		 *	Ensure fr_state_restore_to_child
		 *	can be called again if it's actually
		 *	needed, by giving the child it's own
		 * 	unique state_ctx again.
		 */
		MEM(request->session_state_ctx = fr_pair_afrom_da(NULL, request_attr_state));
	}
}

/** Restore subrequest data from a parent request
 *
 * @param[in] child		The child request to restore state to.
 * @param[in] unique_ptr	A parent may have multiple subrequests spawned
 *				by different modules.  This identifies the module
 *      			or other facility that spawned the subrequest.
 * @param[in] unique_int	Further identification.
 */
void fr_state_restore_to_child(request_t *child, void const *unique_ptr, int unique_int)
{
	state_child_entry_t	*child_entry;
	request_t		*request = child; /* Stupid logging */

	if (!fr_cond_assert_msg(child->parent,
				"Child request must have request->parent set when restoring state")) return;


	child_entry = request_data_get(child->parent, unique_ptr, unique_int);
	if (!child_entry) {
		RDEBUG3("No child state found in parent %s", child->parent->name);
		return;
	}

	/*
	 *	Shouldn't really be possible unless
	 *	there's a logic bug in this API.
	 */
	if (!fr_cond_assert_msg(!child_entry->thawed,
				"Child state entry already thawed by %s - %p",
				child_entry->thawed->name, child_entry->thawed)) return;

	RDEBUG3("Restoring subrequest state from request %s", child->parent->name);

	/*
	 *	If we can restore from the parent, do so
	 */
	TALLOC_FREE(child->session_state_ctx);
	fr_assert_msg(child_entry->ctx, "session child entry missing ctx");
	child->session_state_ctx = child_entry->ctx;
	child_entry->ctx = NULL;				/* No longer owns the ctx */
	child_entry->thawed = child;

	request_data_restore(child, &child_entry->data);	/* Put all the request data back */

	talloc_free(child_entry);
}

/** Remove state from a child
 *
 * This is useful for modules like EAP, where we keep a persistent eap_session
 * but may call multiple EAP method modules during negotiation, and need to
 * discard the state between each module call.
 *
 * @param[in] parent		Holding the child's state.
 * @param[in] unique_ptr	A parent may have multiple subrequests spawned
 *				by different modules.  This identifies the module
 *      			or other facility that spawned the subrequest.
 * @param[in] unique_int	Further identification.
 */
void fr_state_discard_child(request_t *parent, void const *unique_ptr, int unique_int)
{
	state_child_entry_t	*child_entry;
	request_t		*request = parent; /* Stupid logging */

	child_entry = request_data_get(parent, unique_ptr, unique_int);
	if (!child_entry) {
		RDEBUG3("No child state found in parent %s", parent->name);
		return;
	}

	talloc_free(child_entry);
}

/** Return number of entries created
 *
 */
uint64_t fr_state_entries_created(fr_state_tree_t *state)
{
	return state->id;
}

/** Return number of entries that timed out
 *
 */
uint64_t fr_state_entries_timeout(fr_state_tree_t *state)
{
	return state->timed_out;
}

/** Return number of entries we're currently tracking
 *
 */
uint64_t fr_state_entries_tracked(fr_state_tree_t *state)
{
	return fr_rb_num_elements(state->tree);
}
