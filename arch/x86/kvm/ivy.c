#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include "dsm-util.h"
#include "ivy.h"
#include "mmu.h"
#include "dsm_flush.h"

#include <linux/kthread.h>
#include <linux/mmu_context.h>

/*
To use GiantVM's DSM implementation: enable DSM_TRANSFER_PAGE and DSM_WRITE_GUEST_PAGE, disable FLUSH_GFN_METHOD_3
To use HAC's DSM implementation: disable DSM_TRANSFER_PAGE and DSM_WRITE_GUEST_PAGE, enable FLUSH_GFN_METHOD_3
 */

enum kvm_dsm_request_type {
	DSM_REQ_INVALIDATE,
	DSM_REQ_READ,
	DSM_REQ_WRITE,
};
static char* req_desc[] = {"INV", "READ", "WRITE", "FLUSH"};

static inline copyset_t *dsm_get_copyset(
		struct kvm_dsm_memory_slot *slot, hfn_t vfn)
{
	return slot->vfn_dsm_state[vfn - slot->base_vfn].copyset;
}

static inline void dsm_add_to_copyset(struct kvm_dsm_memory_slot *slot, hfn_t vfn, int id)
{
	set_bit(id, slot->vfn_dsm_state[vfn - slot->base_vfn].copyset);
}

static inline void dsm_clear_copyset(struct kvm_dsm_memory_slot *slot, hfn_t vfn)
{
	bitmap_zero(dsm_get_copyset(slot, vfn), DSM_MAX_INSTANCES);
}

/*
 * @requester:	the requester (real message sender or manager or probOwner) of
 * this invalidate request.
 * @msg_sender: the real message sender.
 */
struct dsm_request {
	gfn_t gfn;
	unsigned char requester;
	unsigned char msg_sender;
	unsigned char req_type;
	bool is_smm;

	/*
	 * If version of two pages in different nodes are the same, the contents
	 * are the same.
	 */
	uint16_t version;
};

struct dsm_response {
	copyset_t inv_copyset;
	uint16_t version;
};

/*
 * @msg_sender: the message may be delegated by manager (or other probOwners)
 * (kvm->arch.dsm_id) and real sender can be appointed here.
 * @inv_copyset: if req_type = DSM_REQ_WRITE, the requester becomes owner and has duty
 * to broadcast invalidate.
 * @return: the length of response
 */

static noinline int find_and_lock_sock(struct kvm *kvm, int dest_id, int base_idx, gfn_t gfn)
{
	// try to find a free socket
	static int to_wait_on = 0;
	int start_idx = dest_id*SOCKS_PER_INSTANCE+base_idx;
	int stop_idx = (dest_id+1)*SOCKS_PER_INSTANCE+base_idx;
	int sock_id = start_idx;
	for(; sock_id<stop_idx; sock_id++)
	{
		kconnection_t ** conn_sock = &kvm->arch.dsm_conn_socks[sock_id];

		/*
		* Mutiple vCPUs/servers may connect to a remote node simultaneously.
		*/
		if (*conn_sock == NULL) {
			mutex_lock(&kvm->arch.conn_init_lock);
			if (*conn_sock == NULL) {
				// setting up a new socket
				if(kvm_dsm_connect(kvm, dest_id, conn_sock)<0){
					mutex_unlock(&kvm->arch.conn_init_lock);
					pr_err("error: dsm_connect failed\n");
					return -1;
				}
			}
			mutex_unlock(&kvm->arch.conn_init_lock);
		}

		if(mutex_trylock(&kvm->arch.dsm_conn_socks[sock_id]->lock))
		{
			// dsm_debug_tidy("found free socket %d gfn %llu\n", sock_id, gfn);
			break;
		}
	}
	if(sock_id==stop_idx)
	{
		to_wait_on = (to_wait_on+1)%SOCKS_PER_INSTANCE;
		sock_id = start_idx + to_wait_on;
		// dsm_debug_tidy("free socket not found, just lock %d gfn %llu\n", sock_id, gfn);
		mutex_lock(&kvm->arch.dsm_conn_socks[sock_id]->lock);
		// dsm_debug_tidy("locked %d gfn %llu\n", sock_id, gfn);
	}
	return sock_id;
}
static noinline int kvm_dsm_fetch(struct kvm *kvm, uint16_t dest_id, bool from_server,
		const struct dsm_request *req, void *data, struct dsm_response *resp)
{
	kconnection_t **conn_sock;
	int ret;
	tx_add_t tx_add = {
		.txid = generate_txid(kvm, dest_id),
	};

	if (kvm->arch.dsm_stopped)
	{
		pr_err("error: dsm_stopped\n");
		return -EINVAL;
	}

	int sock_id;
	if (!from_server)
	{
		sock_id = find_and_lock_sock(kvm, dest_id, 0, req->gfn);
	}
	else {
		sock_id = find_and_lock_sock(kvm, dest_id, DSM_MAX_INSTANCES*SOCKS_PER_INSTANCE, req->gfn);
	}
	if(sock_id<0){
		ret = -1;
	}
	conn_sock = &kvm->arch.dsm_conn_socks[sock_id];

	dsm_debug_v("kvm[%d] sent request[0x%x] to kvm[%d] req_type[%s] gfn[%llu,%d]",
			kvm->arch.dsm_id, tx_add.txid, dest_id, req_desc[req->req_type],
			req->gfn, req->is_smm);
	#ifdef IVY_PAGE_FAULT_TIME_DSM_FETCH
	ktime_t start, diff;
	start = ktime_get();
	#endif
#ifdef IVY_PAGE_FAULT_SEND_RECV_REQ_TIME
	ktime_t start = ktime_get();
#endif
	ret = network_ops.send(*conn_sock, (const char *)req, sizeof(struct
				dsm_request), 0, &tx_add);
	if (ret < 0){
		pr_err("%s: error: ret < 0, ret: %d\n", __func__, ret);
		goto done;
	}
#ifdef IVY_PAGE_FAULT_SEND_RECV_REQ_TIME
	if(req->req_type == DSM_REQ_WRITE){
		ret = network_ops.receive(*conn_sock, data, MSG_WAITALL, &tx_add);
		if (ret < 0){
			pr_err("%s: error: ret < 0, ret: %d\n", __func__, ret);
			goto done;
		}
	}
	ktime_t diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_SEND_RECV_REQ, true);
#endif
	#ifdef IVY_PAGE_FAULT_TIME_DSM_FETCH
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_DSM_FETCH_INVD_SEND+req->req_type, true);
	start = ktime_get();
	#endif

	if (req->req_type == DSM_REQ_INVALIDATE) {
		ret = network_ops.receive(*conn_sock, data, MSG_WAITALL, &tx_add);
	}
	else {
		ret = network_ops.receive(*conn_sock, data, MSG_WAITALL, &tx_add);
		/* Nodes indicated by inv_copyset should be sent INV messages upon write
	 * fault. It's also used to transfer the complete copyset upon read fault. */
		resp->inv_copyset = tx_add.inv_copyset;
		resp->version = tx_add.version;
	}
	if (ret < 0){
		pr_err("%s: error: ret < 0, ret: %d\n", __func__, ret);
		goto done;

	}

done:
	if(sock_id>=0){
		mutex_unlock(&kvm->arch.dsm_conn_socks[sock_id]->lock);
	}
	dsm_debug_v("kvm[%d] received node reply ret=%d after sending request[0x%x] to kvm[%d] req_type[%s] gfn[%llu,%d]",ret,
			kvm->arch.dsm_id, tx_add.txid, dest_id, req_desc[req->req_type],
			req->gfn, req->is_smm);
	#ifdef IVY_PAGE_FAULT_TIME_DSM_FETCH
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_DSM_FETCH_INVD_RECV+req->req_type, true);
	#endif
	return ret;
}

/*
 * kvm_dsm_invalidate - issued by owner of a page to invalidate all of its copies
 * @cpyset: given copyset. NULL means using its own copyset.
 */
static int kvm_dsm_invalidate(struct kvm *kvm, gfn_t gfn, bool is_smm,
		struct kvm_dsm_memory_slot *slot, hfn_t vfn, copyset_t *cpyset, int req)
{
	#ifdef IVY_PAGE_FAULT_TIME_INVD
	ktime_t start, diff;
	start = ktime_get();
	#endif
	dsm_debug("enter kvm_dsm_invalidate(dsm_id=%d vfn=%lld)\n", kvm->arch.dsm_id, vfn);

	int holder;
	int ret = 0;
	char r = 1;
	copyset_t *copyset;
	struct dsm_response resp;

	copyset = cpyset ? cpyset : dsm_get_copyset(slot, vfn);

	/*
	 * A given copyset has been properly tailored so that no redundant INVs will
	 * be sent to invalid nodes (nodes in the call-chain).
	 */
	for_each_set_bit(holder, copyset, DSM_MAX_INSTANCES) {
		struct dsm_request req = {
			.req_type = DSM_REQ_INVALIDATE,
			.requester = kvm->arch.dsm_id,
			.msg_sender = kvm->arch.dsm_id,
			.gfn = gfn,
			.is_smm = is_smm,
			.version = dsm_get_version(slot, vfn),
		};
		if (kvm->arch.dsm_id == holder)
			continue;
		/* Santiy check on copyset consistency. */
		BUG_ON(holder >= kvm->arch.cluster_iplist_len);

		dsm_debug("call kvm_dsm_fetch on holder %d\n", holder);
		ret = kvm_dsm_fetch(kvm, holder, false, &req, &r, &resp);
		if (ret < 0)
			return ret;
	}
	#ifdef IVY_PAGE_FAULT_TIME_INVD
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_INVD, true);
	#endif
	return 0;
}

static int dsm_handle_invalidate_req(struct kvm *kvm, kconnection_t *conn_sock,
		struct kvm_memory_slot *memslot, struct kvm_dsm_memory_slot *slot,
		const struct dsm_request *req, bool *retry, hfn_t vfn, char *page,
		tx_add_t *tx_add)
{
	dsm_debug_v("kvm.id=%d dsm_handle_invalidate_req\n", kvm->arch.dsm_id);
	int ret = 0;
	char r;

	if (dsm_is_pinned(slot, vfn) && !kvm->arch.dsm_stopped) {
		*retry = true;
		pr_err("kvm[%d] REQ_INV blocked by pinned gfn[%llu,%d], sleep then retry\n",
				kvm->arch.dsm_id, req->gfn, req->is_smm);
		return 0;
	}

	/*
	 * The vfn->gfn rmap can be inconsistent with kvm_memslots when
	 * we're setting memslot, but this will not affect the correctness.
	 * If the old memslot is deleted, then the sptes will be zapped
	 * anyway, so nothing should be done with this case. On the other
	 * hand, if the new memslot is inserted (freshly created or moved),
	 * its sptes are yet to be constructed in tdp_page_fault, and that
	 * is protected by dsm_lock and cannot happen concurrently with the
	 * server side transaction, so the correct DSM state will be seen
	 * in spte construction.
	 *
	 * For usual cases, order between these two operations (change DSM state and
	 * modify page table right) counts. After spte is zapped, DSM software
	 * should make sure that #PF handler read the correct DSM state.
	 */
	BUG_ON(dsm_is_modified(slot, vfn));

	dsm_lock_fast_path(slot, vfn, true);

	#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
	ktime_t start, diff;
	start = ktime_get();
	#endif
	dsm_change_state(slot, vfn, DSM_INVALID);
	kvm_dsm_apply_access_right(kvm, slot, vfn, DSM_INVALID, req->gfn);
	#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_PAGE_STATE_FLOW, false);
	#endif
	// FLUSH HERE
	flush_gfn(kvm, memslot, req->gfn, false);

	dsm_set_prob_owner(slot, vfn, req->msg_sender);
	dsm_clear_copyset(slot, vfn);
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	ktime_t start, diff;
	start = ktime_get();
	#endif
	ret = network_ops.send(conn_sock, &r, 1, 0, tx_add);
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_HANDLE_INVD_SEND, false);
	#endif

	dsm_unlock_fast_path(slot, vfn, true);

	return ret < 0 ? ret : 0;
}

static int dsm_handle_write_req(struct kvm *kvm, kconnection_t *conn_sock,
		struct kvm_memory_slot *memslot, struct kvm_dsm_memory_slot *slot,
		const struct dsm_request *req, bool *retry, hfn_t vfn, char *page,
		tx_add_t *tx_add)
{
	int ret = 0, length = PAGE_SIZE_TRANSFER;
	int owner = -1;
	dsm_debug("kvm.id=%d dsm_handle_write_req\n", kvm->arch.dsm_id);

	bool is_owner = false;
	struct dsm_response resp;

	if (dsm_is_pinned(slot, vfn) && !kvm->arch.dsm_stopped) {
		*retry = true;
		pr_err("kvm[%d] REQ_WRITE blocked by pinned gfn[%llu,%d], sleep then retry\n",
				kvm->arch.dsm_id, req->gfn, req->is_smm);
		return 0;
	}

	if ((is_owner = dsm_is_owner(slot, vfn))) {
		BUG_ON(dsm_get_prob_owner(slot, vfn) != kvm->arch.dsm_id);

		/* I'm owner */
		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_debug_v("kvm[%d](M1) changed owner of gfn[%llu,%d] "
				"from kvm[%d] to kvm[%d]\n", kvm->arch.dsm_id, req->gfn,
				req->is_smm, kvm->arch.dsm_id, req->msg_sender);
		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		ktime_t start, diff;
		start = ktime_get();
		#endif
		dsm_change_state(slot, vfn, DSM_INVALID);
		kvm_dsm_apply_access_right(kvm, slot, vfn, DSM_INVALID,req->gfn);
		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		diff = ktime_sub(ktime_get(), start);
		save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_PAGE_STATE_FLOW, false);
		#endif
		// FLUSH HERE
		flush_gfn(kvm, memslot, req->gfn, false);
		/* Send back copyset to new owner. */
		resp.inv_copyset = *dsm_get_copyset(slot, vfn);
		resp.version = dsm_get_version(slot, vfn);
		clear_bit(kvm->arch.dsm_id, &resp.inv_copyset);
#ifdef DSM_TRANSFER_PAGE
		ret = kvm_read_guest_page_nonlocal(kvm, memslot, req->gfn, page, 0, PAGE_SIZE);
		if (ret < 0){
			pr_err("read guest nonlocal err\n");
			return ret;
		}
#endif
	}
	else if (dsm_is_initial(slot, vfn) && kvm->arch.dsm_id == 0) {
		/* Send back a dummy copyset. */
		resp.inv_copyset = 0;
		resp.version = dsm_get_version(slot, vfn);
		flush_gfn(kvm, memslot, req->gfn, false);
#ifdef DSM_TRANSFER_PAGE
		ret = kvm_read_guest_page_nonlocal(kvm, memslot, req->gfn, page, 0, PAGE_SIZE);
		if (ret < 0){
			pr_err("read guest nonlocal err\n");
			return ret;
		}
#endif
		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_change_state(slot, vfn, DSM_INVALID);
	}
	else {
		BUG();
		dsm_debug_v("error: should not be here in two nodes mode!");
		struct dsm_request new_req = {
			.req_type = DSM_REQ_WRITE,
			.requester = kvm->arch.dsm_id,
			.msg_sender = req->msg_sender,
			.gfn = req->gfn,
			.is_smm = req->is_smm,
			.version = req->version,
		};
		owner = dsm_get_prob_owner(slot, vfn);
		ret = length = kvm_dsm_fetch(kvm, owner, true, &new_req, page, &resp);
		if (ret < 0)
			return ret;

		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		ktime_t start, diff;
		start = ktime_get();
		#endif
		dsm_change_state(slot, vfn, DSM_INVALID);
		kvm_dsm_apply_access_right(kvm, slot, vfn, DSM_INVALID, req->gfn);
		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		diff = ktime_sub(ktime_get(), start);
		save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_PAGE_STATE_FLOW, false);
		#endif
		// FLUSH HERE
		flush_gfn(kvm, memslot, req->gfn, false);
		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_debug_v("kvm[%d](M3) changed owner of gfn[%llu,%d] "
				"from kvm[%d] to kvm[%d]\n", kvm->arch.dsm_id, req->gfn,
				req->is_smm, owner, req->msg_sender);

		clear_bit(kvm->arch.dsm_id, &resp.inv_copyset);
	}

	tx_add->inv_copyset = resp.inv_copyset;
	tx_add->version = resp.version;
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	ktime_t start, diff;
	start = ktime_get();
	#endif
	ret = network_ops.send(conn_sock, page, length, 0, tx_add);
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_HANDLE_WRITE_SEND, false);
	#endif
	if (ret < 0)
		return ret;
	dsm_debug_v("kvm[%d] sent page[%llu,%d] to kvm[%d] length %d hash: 0x%x\n",
			kvm->arch.dsm_id, req->gfn, req->is_smm, req->requester, length,
			jhash(page, length, JHASH_INITVAL));
	return 0;
}

/*
 * A read fault causes owner transmission, too. It's different from original MSI
 * protocol. It mainly addresses a subtle data-race that *AFTER* DSM page fault
 * and *BEFORE* setting appropriate right a write requests (invalidation
 * request) issued by owner will be 'swallowed'. Specifically, in
 * mmu.c:tdp_page_fault:
 * // A read fault
 * [pf handler] dsm_access = kvm_dsm_vcpu_acquire_page()
 * .
 * . [server] dsm_handle_invalidate_req()
 * .
 * [pf handler] __direct_map(dsm_access)
 * [pf handler] kvm_dsm_vcpu_release_page()
 * dsm_handle_invalidate_req() takes no effects then (Note that invalidate
 * handler is lock-free). And if a read fault changes owner too, others write
 * faults will be synchronized by this node.
 */
static int dsm_handle_read_req(struct kvm *kvm, kconnection_t *conn_sock,
		struct kvm_memory_slot *memslot, struct kvm_dsm_memory_slot *slot,
		const struct dsm_request *req, bool *retry, hfn_t vfn, char *page,
		tx_add_t *tx_add)
{
	dsm_debug("kvm.id=%d dsm_handle_read_req\n", kvm->arch.dsm_id);

	int ret = 0, length = PAGE_SIZE_TRANSFER;
	int owner = -1;
	bool is_owner = false;
	struct dsm_response resp = {
		.version = 0,
	};

	if (dsm_is_pinned_read(slot, vfn) && !kvm->arch.dsm_stopped) {
		*retry = true;
		pr_err("kvm[%d] REQ_READ blocked by pinned gfn[%llu,%d], sleep then retry\n",
				kvm->arch.dsm_id, req->gfn, req->is_smm);
		return 0;
	}

	if ((is_owner = dsm_is_owner(slot, vfn))) {
		BUG_ON(dsm_get_prob_owner(slot, vfn) != kvm->arch.dsm_id);

		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_debug_v("kvm[%d](S1) changed owner of gfn[%llu,%d] "
				"from kvm[%d] to kvm[%d]\n", kvm->arch.dsm_id, req->gfn,
				req->is_smm, kvm->arch.dsm_id, req->msg_sender);
		/* TODO: if modified */
		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		ktime_t start, diff;
		start = ktime_get();
		#endif
		dsm_change_state(slot, vfn, DSM_SHARED);
		kvm_dsm_apply_access_right(kvm, slot, vfn, DSM_SHARED,req->gfn);
		#ifdef IVY_PAGE_FAULT_PAGE_STATE_FLOW_TIME
		diff = ktime_sub(ktime_get(), start);
		save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_PAGE_STATE_FLOW, false);
		#endif
		// if modified FLUSH HERE
		flush_gfn(kvm, memslot, req->gfn, false);
#ifdef DSM_TRANSFER_PAGE
		ret = kvm_read_guest_page_nonlocal(kvm, memslot, req->gfn, page, 0, PAGE_SIZE);
		if (ret < 0){
			pr_err("read guest nonlocal err\n");
			goto out;
		}
#endif
		/*
		 * read fault causes owner transmission, too. Send copyset back to new
		 * owner.
		 */
		resp.inv_copyset = *dsm_get_copyset(slot, vfn);
		BUG_ON(!(test_bit(kvm->arch.dsm_id, &resp.inv_copyset)));
		resp.version = dsm_get_version(slot, vfn);
	}
	else if (dsm_is_initial(slot, vfn) && kvm->arch.dsm_id == 0) {
		flush_gfn(kvm, memslot, req->gfn, false);
#ifdef DSM_TRANSFER_PAGE
		ret = kvm_read_guest_page_nonlocal(kvm, memslot, req->gfn, page, 0, PAGE_SIZE);
		if (ret < 0){
			pr_err("read guest nonlocal err\n");
			goto out;
		}
#endif

		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_change_state(slot, vfn, DSM_SHARED);
		dsm_add_to_copyset(slot, vfn, kvm->arch.dsm_id);
		resp.inv_copyset = *dsm_get_copyset(slot, vfn);
		resp.version = dsm_get_version(slot, vfn);
	}
	else {
		BUG();
		dsm_debug_v("error: should not be here in two nodes mode!");
		struct dsm_request new_req = {
			.req_type = DSM_REQ_READ,
			.requester = kvm->arch.dsm_id,
			.msg_sender = req->msg_sender,
			.gfn = req->gfn,
			.is_smm = req->is_smm,
			.version = req->version,
		};
		owner = dsm_get_prob_owner(slot, vfn);
		ret = length = kvm_dsm_fetch(kvm, owner, true, &new_req, page, &resp);
		if (ret < 0)
			goto out;
		BUG_ON(dsm_is_readable(slot, vfn) && !(test_bit(kvm->arch.dsm_id,
						&resp.inv_copyset)));
		/* Even read fault changes owner now. May the force be with you. */
		dsm_set_prob_owner(slot, vfn, req->msg_sender);
		dsm_debug_v("kvm[%d](S3) changed owner of gfn[%llu,%d] vfn[%llu] "
				"from kvm[%d] to kvm[%d]\n", kvm->arch.dsm_id, req->gfn,
				req->is_smm, vfn, owner, req->msg_sender);
	}

	tx_add->inv_copyset = resp.inv_copyset;
	tx_add->version = resp.version;
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	ktime_t start, diff;
	start = ktime_get();
	#endif
	ret = network_ops.send(conn_sock, page, length, 0, tx_add);
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_SEND
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_HANDLE_READ_SEND, false);
	#endif
	if (ret < 0)
		goto out;
	dsm_debug_v("kvm[%d] sent page[%llu,%d] to kvm[%d] length %d hash: 0x%x\n",
			kvm->arch.dsm_id, req->gfn, req->is_smm, req->requester, length,
			jhash(page, length, JHASH_INITVAL));

out:
	return ret;
}

int ivy_kvm_dsm_handle_req(void *data)
{
	int ret = 0, idx;

	struct dsm_conn *conn = (struct dsm_conn *)data;
	struct kvm *kvm = conn->kvm;
	kconnection_t *conn_sock = conn->sock;

	struct kvm_memory_slot *memslot;
	struct kvm_dsm_memory_slot *slot;
	struct dsm_request req;
	bool retry = false;
	hfn_t vfn;
	char comm[TASK_COMM_LEN];

	char *page;
	int len;

#ifdef DSM_TRANSFER_PAGE
	/* Size of the maximum buffer is PAGE_SIZE */
	page = kmalloc(PAGE_SIZE_TRANSFER, GFP_KERNEL);
	if (page == NULL)
		return -ENOMEM;
	//memset(page, 0xFF, PAGE_SIZE_TRANSFER);
#else
	char r;
	page = &r;
#endif

	while (1) {
		tx_add_t tx_add = {
			/* Accept any incoming requests. */
			.txid = 0xFF,
		};

		if (kthread_should_stop()) {
			ret = -EPIPE;
			goto out;
		}

		len = network_ops.receive(conn_sock, (char*)&req, MSG_WAITALL, &tx_add);
		BUG_ON(len > 0 && len != sizeof(struct dsm_request));

		if (len <= 0) {
			pr_err("kvm %d %s: receive req failed!\n", kvm->arch.dsm_id, __func__);
			ret = len;
			goto out;
		}

#ifdef IVY_PAGE_FAULT_SEND_RECV_REQ_TIME
		if(req.req_type == DSM_REQ_WRITE){
			network_ops.send(conn_sock, (const char *)&req, sizeof(struct dsm_request), 0, &tx_add);
		}
#endif

		BUG_ON(req.requester == kvm->arch.dsm_id);

retry_handle_req:
		idx = srcu_read_lock(&kvm->srcu);
		memslot = __gfn_to_memslot(__kvm_memslots(kvm, req.is_smm), req.gfn);
		/*
		 * We should ignore private memslots since they are not really visible
		 * to guest and thus are not part of guest state that should be
		 * distributedly shared.
		 */
		if (!memslot || memslot->id >= KVM_USER_MEM_SLOTS ||
				memslot->flags & KVM_MEMSLOT_INVALID) {
			pr_err("%s: kvm %d invalid gfn %llu!\n",
					__func__, kvm->arch.dsm_id, req.gfn);
			srcu_read_unlock(&kvm->srcu, idx);
			schedule();
			goto retry_handle_req;
		}
		vfn = __gfn_to_vfn_memslot(memslot, req.gfn);
		slot = gfn_to_hvaslot(kvm, memslot, req.gfn);
		if (!slot) {
			pr_err("%s: kvm %d slot of gfn %llu doesn't exist!\n",
					__func__, kvm->arch.dsm_id, req.gfn);
			srcu_read_unlock(&kvm->srcu, idx);
			schedule();
			goto retry_handle_req;
		}

		dsm_debug("kvm[%d] received request[0x%x] from kvm[%d->%d] req_type[%s] "
				"gfn[%llu,%d] vfn[%llu] version %d myversion %d\n",
				kvm->arch.dsm_id, tx_add.txid, req.msg_sender, req.requester,
				req_desc[req.req_type], req.gfn, req.is_smm, vfn, req.version,
				dsm_get_version(slot, vfn));

		BUG_ON(dsm_is_initial(slot, vfn) && dsm_get_prob_owner(slot, vfn) != 0);

		// dsm_debug("after BUG_ON(dsm_is_initial)\n");
		/*
		 * All #PF transactions begin with acquiring owner's (global visble)
		 * dsm_lock. Since only owner can issue DSM_REQ_INVALIDATE, there's no
		 * need to acquire lock. And locking here is prone to cause deadlock.
		 *
		 * If the thread waits for the lock for too long, just buffer the
		 * request and finds whether there's some more requests.
		 */
		if (req.req_type != DSM_REQ_INVALIDATE) {
			dsm_lock(kvm, slot, vfn);
		}

	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_REQ
	ktime_t start, diff;
	start = ktime_get();
	#endif
		switch (req.req_type) {
		case DSM_REQ_INVALIDATE:
			ret = dsm_handle_invalidate_req(kvm, conn_sock, memslot, slot, &req,
					&retry, vfn, page, &tx_add);
			if (ret < 0)
				goto out_unlock;
			break;

		case DSM_REQ_WRITE:
			ret = dsm_handle_write_req(kvm, conn_sock, memslot, slot, &req,
					&retry, vfn, page, &tx_add);
			if (ret < 0)
				goto out_unlock;
			break;

		case DSM_REQ_READ:
			ret = dsm_handle_read_req(kvm, conn_sock, memslot, slot, &req,
					&retry, vfn, page, &tx_add);
			if (ret < 0)
				goto out_unlock;
			break;

		default:
			BUG();
		}
	#ifdef IVY_PAGE_FAULT_TIME_HANDLE_REQ
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, DSM_TIME_TYPE_HANDLE_REQ, true);
	#endif

		/* Once a request has been completed, this node isn't owner then. */
		if (req.req_type != DSM_REQ_INVALIDATE)
			dsm_clear_copyset(slot, vfn);

		if (req.req_type != DSM_REQ_INVALIDATE)
			dsm_unlock(kvm, slot, vfn);

		srcu_read_unlock(&kvm->srcu, idx);

		if (retry) {
			pr_err("if retry before schedule\n");
			retry = false;
			schedule();
			dsm_debug("if retry after schedule goto retry_handle_req\n");
			goto retry_handle_req;
		}
	}
out_unlock:
	pr_err("error %s break while out_unlock\n", __func__);
	if (req.req_type != DSM_REQ_INVALIDATE)
		dsm_unlock(kvm, slot, vfn);
	srcu_read_unlock(&kvm->srcu, idx);
out:
#ifdef DSM_TRANSFER_PAGE
	kfree(page);
#endif
	/* return zero since we quit voluntarily */
	if (kvm->arch.dsm_stopped) {
		ret = 0;
	}
	else {
		get_task_comm(comm, current);
		dsm_debug("kvm[%d] %s exited server loop, error %d\n",
				kvm->arch.dsm_id, comm, ret);
	}

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	pr_err("exit ivy_kvm_dsm_handle_req\n");
	return ret;
}

/*
 * A faulting vCPU can fill in the EPT correctly without network operations.
 * There're two scenerios:
 * 1. spte is dropped (swap, ksm, etc.)
 * 2. The faulting page has been updated by another vCPU.
 */
static bool is_fast_path(struct kvm *kvm, struct kvm_dsm_memory_slot *slot,
		hfn_t vfn, bool write)
{
	/*
	 * DCL is required here because the invalidation server may change the DSM
	 * state too.
	 * Futher, a data race ocurrs when an invalidation request
	 * arrives, the client is between kvm_dsm_page_fault and __direct_map (see
	 * the comment of dsm_handle_read_req). By then EPT is readable while DSM
	 * state is invalid. This causes invalidation request, i.e., a remote write
	 * is omitted.
	 * All transactions should be synchorized by the owner, which is a basic
	 * rule of IVY. But the fast path breaks it. To keep consistency, the fast
	 * path should not be interrupted by an invalidation request. So both fast
	 * path and dsm_handle_invalidate_req should hold a per-page fast_path_lock.
	 */
	 
	if (write && dsm_is_modified(slot, vfn)) {
		dsm_lock_fast_path(slot, vfn, false);
		if (write && dsm_is_modified(slot, vfn)) {
			return true;
		}
		else {
			dsm_unlock_fast_path(slot, vfn, false);
			return false;
		}
	}
	
	if (!write && dsm_is_readable(slot, vfn)) {
		dsm_lock_fast_path(slot, vfn, false);
		if (!write && dsm_is_readable(slot, vfn)) {
			return true;
		}
		else {
			dsm_unlock_fast_path(slot, vfn, false);
			return false;
		}
	}
	return false;
}

/*
 * copyset rules:
 * 1. Only copyset residing on the owner side is valid, so when owner
 * transmission occurs, copyset of the old one should be cleared.
 * 2. Copyset of a fresh write fault owner is zero.
 * 3. Every node can only operate its own bit of a copyset. For example, in a
 * typical msg_sender->manager->owner (write fault) chain, both owner and
 * manager should clear their own bit in the copyset sent back to the new
 * owner (msg_sender). In the current implementation, the chain may becomes
 * msg_sender->probOwner0->probOwner1->...->requester->owner, each probOwner
 * should clear their own bit.
 *
 * version rules:
 * Overview: Each page (gfn) has a version. If versions of two pages on different
 * nodes are the same, the data of two pages are the same.
 * 1. Upon a write fault, the version of requster is resp.version (old owner) + 1
 * 2. Upon a read fault, the version of requester is the same as resp.version
 */

// #define IVY_PAGE_FAULT_CNT

#ifdef IVY_PAGE_FAULT_CNT
static inline void cnt_ivy_pf(void)
{
	static atomic_t pf_cnt = ATOMIC_INIT(0);
	int r =  (int)atomic_add_return(1, &pf_cnt);
	if(r%10000==0){
		pr_info("ivy page fault count: %d\n", r);
	}
}
static inline void cnt_ivy_dsm_pf(void)
{
	static atomic_t dsm_pf_cnt = ATOMIC_INIT(0);
	int r =  (int)atomic_add_return(1, &dsm_pf_cnt);
	if(r%10000==0){
		pr_info("ivy dsm page fault count: %d\n", r);
	}
}
#endif

int ivy_kvm_dsm_page_fault(struct kvm *kvm, struct kvm_memory_slot *memslot,
		gfn_t gfn, bool is_smm, int write)
{
	int ret, resp_len = 0;
	struct kvm_dsm_memory_slot *slot;
	hfn_t vfn;
	char *page = NULL;
	struct dsm_response resp;
	int owner;

	ret = 0;
	vfn = __gfn_to_vfn_memslot(memslot, gfn);
	slot = gfn_to_hvaslot(kvm, memslot, gfn);

#ifdef IVY_PAGE_FAULT_CNT
	cnt_ivy_pf();
#endif
	dsm_debug("enter ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): vfn=%lld slot=%p slot->userspace_addr=%lu slot->base_gfn=%lld\n", kvm->arch.dsm_id, gfn, write, vfn,slot, memslot->userspace_addr, memslot->base_gfn);

	if (is_fast_path(kvm, slot, vfn, write)) {
		dsm_debug("--- is_fast_path(vfn=%llx, write=%d) -------\n", vfn, write);
		if (write) {
			return ACC_ALL;
		}
		else {
			return ACC_EXEC_MASK | ACC_USER_MASK;
		}
	}
#ifdef IVY_PAGE_FAULT_CNT
	cnt_ivy_dsm_pf();
#endif
#ifdef IVY_PAGE_FAULT_GFN_COUNT_TIME
save_pf_gfn_count_time_to_db(kvm, gfn, true);
#endif

	BUG_ON(dsm_is_initial(slot, vfn) && dsm_get_prob_owner(slot, vfn) != 0);

	#ifdef IVY_PAGE_FAULT_TIME_NOTFAST
	ktime_t start, diff;
	start = ktime_get();
	#endif

#ifdef DSM_TRANSFER_PAGE
	page = kmalloc(PAGE_SIZE_TRANSFER, GFP_KERNEL);
	if (page == NULL) {
		printk(KERN_ERR "kmalloc error\n");
		ret = -ENOMEM;
		goto out_error;
	}
	//memset(page, 0xFF, PAGE_SIZE_TRANSFER);
#else
	char r;
	page = &r;
#endif

	/*
	 * If #PF is owner write fault, then issue invalidate by itself.
	 * Or this node will be owner after #PF, it still issue invalidate by
	 * receiving copyset from old owner.
	 */
	if (write) {
		struct dsm_request req = {
			.req_type = DSM_REQ_WRITE,
			.requester = kvm->arch.dsm_id,
			.msg_sender = kvm->arch.dsm_id,
			.gfn = gfn,
			.is_smm = is_smm,
			.version = dsm_get_version(slot, vfn),
		};
		if (dsm_is_owner(slot, vfn)) {
			BUG_ON(dsm_get_prob_owner(slot, vfn) != kvm->arch.dsm_id);
			dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): dsm_is_owner(slot, vfn=%lld)=true\n", kvm->arch.dsm_id, gfn, write, vfn);

			ret = kvm_dsm_invalidate(kvm, gfn, is_smm, slot, vfn, NULL, kvm->arch.dsm_id);
			if (ret < 0)
				goto out_error;
			resp.version = dsm_get_version(slot, vfn);
			resp_len = PAGE_SIZE_TRANSFER;

			dsm_incr_version(slot, vfn);
		}
		else {
			owner = dsm_get_prob_owner(slot, vfn);
			/* Owner of all pages is 0 on init. */
			if (unlikely(dsm_is_initial(slot, vfn) && kvm->arch.dsm_id == 0)) {
				dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): dsm_is_initial(slot, vfn=%lld) && kvm->arch.dsm_id == 0\n", kvm->arch.dsm_id, gfn, write, vfn);
				dsm_set_prob_owner(slot, vfn, kvm->arch.dsm_id);
				dsm_change_state(slot, vfn, DSM_OWNER | DSM_MODIFIED);
				dsm_add_to_copyset(slot, vfn, kvm->arch.dsm_id);
				ret = ACC_ALL;
				goto out;
			}
			/*
			 * Ask the probOwner. The prob(ably) owner is probably true owner,
			 * or not. If not, forward the request to next probOwner until find
			 * the true owner.
			 */
				dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): not dsm_is_initial or not master\n", kvm->arch.dsm_id, gfn, write);
			ret = resp_len = kvm_dsm_fetch(kvm, owner, false, &req, page,
					&resp);
			if (ret < 0)
				goto out_error;
			ret = kvm_dsm_invalidate(kvm, gfn, is_smm, slot, vfn,
					&resp.inv_copyset, owner);
			if (ret < 0)
				goto out_error;

			dsm_set_version(slot, vfn, resp.version + 1);
		}

		dsm_clear_copyset(slot, vfn);
		dsm_add_to_copyset(slot, vfn, kvm->arch.dsm_id);

		flush_gfn(kvm, memslot, gfn, true);
		if (!dsm_is_owner(slot, vfn) && resp_len > 0) {
			#ifdef DSM_WRITE_GUEST_PAGE
			ret = __kvm_write_guest_page(kvm, memslot, gfn, page, 0, PAGE_SIZE);
			if (ret < 0) {
				pr_err("read guest nonlocal err\n");
				goto out_error;
			}
			flush_gfn(kvm, memslot, gfn, true);
			dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): kvm_dsm_fetch and write a page: hash=0x%x\n", kvm->arch.dsm_id, gfn, write, jhash(page, PAGE_SIZE, JHASH_INITVAL));
			#endif
		}
		dsm_set_prob_owner(slot, vfn, kvm->arch.dsm_id);
		dsm_change_state(slot, vfn, DSM_OWNER | DSM_MODIFIED);
		ret = ACC_ALL;
	} else {
		struct dsm_request req = {
			.req_type = DSM_REQ_READ,
			.requester = kvm->arch.dsm_id,
			.msg_sender = kvm->arch.dsm_id,
			.gfn = gfn,
			.is_smm = is_smm,
			.version = dsm_get_version(slot, vfn),
		};
		owner = dsm_get_prob_owner(slot, vfn);
		/*
		 * If I'm the owner, then I would have already been in Shared or
		 * Modified state.
		 */
		BUG_ON(dsm_is_owner(slot, vfn));

		/* Owner of all pages is 0 on init. */
		if (unlikely(dsm_is_initial(slot, vfn) && kvm->arch.dsm_id == 0)) {
			dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): dsm_is_initial(slot, vfn=%lld) && kvm->arch.dsm_id == 0\n", kvm->arch.dsm_id, gfn, write, vfn);
			dsm_set_prob_owner(slot, vfn, kvm->arch.dsm_id);
			dsm_change_state(slot, vfn, DSM_OWNER | DSM_SHARED);
			dsm_add_to_copyset(slot, vfn, kvm->arch.dsm_id);
			flush_gfn(kvm, memslot, gfn, true);
			ret = ACC_EXEC_MASK | ACC_USER_MASK;
			goto out;
		}
		/* Ask the probOwner */
		dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): vfn=%lld not dsm_is_initial or not master\n", kvm->arch.dsm_id, gfn, write, vfn);
		ret = resp_len = kvm_dsm_fetch(kvm, owner, false, &req, page, &resp);
		if (ret < 0)
			goto out_error;

		dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): vfn=%lld dsm_set_version\n", kvm->arch.dsm_id, gfn, write, vfn);
		dsm_set_version(slot, vfn, resp.version);
		memcpy(dsm_get_copyset(slot, vfn), &resp.inv_copyset, sizeof(copyset_t));
		dsm_add_to_copyset(slot, vfn, kvm->arch.dsm_id);

		flush_gfn(kvm, memslot, gfn, true);

		#ifdef DSM_WRITE_GUEST_PAGE
		ret = __kvm_write_guest_page(kvm, memslot, gfn, page, 0, PAGE_SIZE);
		if (ret < 0){
			pr_err("read guest nonlocal err\n");
			goto out_error;
		}
		dsm_debug("ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): kvm_dsm_fetch and write a page: hash=0x%x\n", kvm->arch.dsm_id, gfn, write, jhash(page, PAGE_SIZE, JHASH_INITVAL));
		flush_gfn(kvm, memslot, gfn, true);
		#endif

		dsm_set_prob_owner(slot, vfn, kvm->arch.dsm_id);
		/*
		 * The node becomes owner after read fault because of data locality,
		 * i.e. a write fault may occur soon. It's not designed to avoid annoying
		 * bugs, right? See comments of dsm_handle_read_req.
		 */
		dsm_change_state(slot, vfn, DSM_OWNER | DSM_SHARED);
		ret = ACC_EXEC_MASK | ACC_USER_MASK;
	}

out:

#ifdef DSM_TRANSFER_PAGE
	kfree(page);
#endif
	dsm_debug("exit ivy_kvm_dsm_page_fault(dsm_id=%d gfn=%lld write=%d): vfn=%lld\n", kvm->arch.dsm_id, gfn, write, vfn);

	#ifdef IVY_PAGE_FAULT_TIME_NOTFAST
	diff = ktime_sub(ktime_get(), start);
	save_pf_time_to_db(kvm, diff, write ? DSM_TIME_TYPE_NOTFAST_PF_WRITE : DSM_TIME_TYPE_NOTFAST_PF_READ, true);
	#endif

	return ret;

out_error:
	dump_stack();
	printk(KERN_ERR "kvm-dsm: node-%d failed to handle page fault on gfn[%llu,%d], "
			"error: %d\n", kvm->arch.dsm_id, gfn, is_smm, ret);

#ifdef DSM_TRANSFER_PAGE
	kfree(page);
#endif
	return ret;
}
