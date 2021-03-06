/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2013 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "core.h"
#include "txrx.h"
#include "htt.h"
#include "mac.h"
#include "debug.h"

static void ath10k_report_offchan_tx(struct ath10k *ar, struct sk_buff *skb)
{
	if (!ATH10K_SKB_CB(skb)->htt.is_offchan)
		return;

	/* If the original wait_for_completion() timed out before
	 * {data,mgmt}_tx_completed() was called then we could complete
	 * offchan_tx_completed for a different skb. Prevent this by using
	 * offchan_tx_skb. */
	spin_lock_bh(&ar->data_lock);
	if (ar->offchan_tx_skb != skb) {
		ath10k_warn("completed old offchannel frame\n");
		goto out;
	}

	complete(&ar->offchan_tx_completed);
	ar->offchan_tx_skb = NULL; /* just for sanity */

	ath10k_dbg(ATH10K_DBG_HTT, "completed offchannel skb %p\n", skb);
out:
	spin_unlock_bh(&ar->data_lock);
}

static void ath10k_set_tx_rate_status(struct ieee80211_tx_rate *rate,
				      const struct htt_tx_done *tx_done)
{
	u8 nss = (tx_done->tx_rate_code >> 4) & 0x3;
	u8 rate_idx = tx_done->tx_rate_code & 0xF;

	rate->count = 1;
	rate->idx = rate_idx; // TODO-BEN:  This is most-likely wrong.

	/*ath10k_warn("tx-rate-status, nss: %d  rate_idx: %d  code: 0x%x  flags 0x%x\n",
		    (int)(nss), (int)(rate_idx), (int)(tx_done->tx_rate_code),
		    (int)(tx_done->tx_rate_flags));
	*/

	if (((tx_done->tx_rate_code >> 6) & 0x3) == 1) { // CCK
		//rate->flags |= IEEE80211_TX_RC_MCS;
	}

	if ((tx_done->tx_rate_code & 0xcc) == 0x44)
		rate->flags |= IEEE80211_TX_RC_USE_SHORT_PREAMBLE;

	if ((tx_done->tx_rate_code & 0xc0) == 0x80)
		rate->flags |= IEEE80211_TX_RC_MCS;

	if ((tx_done->tx_rate_code & 0xc0) == 0xc0) {
		rate->flags |= IEEE80211_TX_RC_VHT_MCS;
		rate->idx = (nss << 4) | rate_idx; // TODO-BEN:  This is most-likely wrong.
	}

	if (tx_done->tx_rate_flags & ATH10K_RC_FLAG_40MHZ)
		rate->flags |= IEEE80211_TX_RC_40_MHZ_WIDTH;
	if (tx_done->tx_rate_flags & ATH10K_RC_FLAG_80MHZ)
		rate->flags |= IEEE80211_TX_RC_80_MHZ_WIDTH;
	if (tx_done->tx_rate_flags & ATH10K_RC_FLAG_160MHZ)
		rate->flags |= IEEE80211_TX_RC_160_MHZ_WIDTH;
	if (tx_done->tx_rate_flags & ATH10K_RC_FLAG_SGI)
		rate->flags |= IEEE80211_TX_RC_SHORT_GI;
}


void ath10k_txrx_tx_unref(struct ath10k_htt *htt,
			  const struct htt_tx_done *tx_done)
{
	struct device *dev = htt->ar->dev;
	struct ieee80211_tx_info *info;
	struct ath10k_skb_cb *skb_cb;
	struct sk_buff *msdu;

	lockdep_assert_held(&htt->tx_lock);

	ath10k_dbg(ATH10K_DBG_HTT, "htt tx completion msdu_id %u discard %d no_ack %d\n",
		   tx_done->msdu_id, !!tx_done->discard, !!tx_done->no_ack);

	if (tx_done->msdu_id >= htt->max_num_pending_tx) {
		ath10k_warn("error: msdu_id %d too big, ignoring\n",
			    tx_done->msdu_id);
		return;
	}

	msdu = htt->pending_tx[tx_done->msdu_id];
	skb_cb = ATH10K_SKB_CB(msdu);

	dma_unmap_single(dev, skb_cb->paddr, msdu->len, DMA_TO_DEVICE);

	if (skb_cb->htt.txbuf)
		dma_pool_free(htt->tx_pool,
			      skb_cb->htt.txbuf,
			      skb_cb->htt.txbuf_paddr);

	ath10k_report_offchan_tx(htt->ar, msdu);

	info = IEEE80211_SKB_CB(msdu);
	memset(&info->status, 0, sizeof(info->status));

	if (tx_done->discard) {
		ieee80211_free_txskb(htt->ar->hw, msdu);
		goto exit;
	}

	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK))
		info->flags |= IEEE80211_TX_STAT_ACK;

	if (tx_done->no_ack)
		info->flags &= ~IEEE80211_TX_STAT_ACK;

	if (tx_done->tx_rate_code || tx_done->tx_rate_flags) {
		ath10k_set_tx_rate_status(&info->status.rates[0], tx_done);
	} else {
		info->status.rates[0].idx = -1;
	}

	ieee80211_tx_status(htt->ar->hw, msdu);
	/* we do not own the msdu anymore */

exit:
	htt->pending_tx[tx_done->msdu_id] = NULL;
	ath10k_htt_tx_free_msdu_id(htt, tx_done->msdu_id);
	__ath10k_htt_tx_dec_pending(htt);
	if (htt->num_pending_tx == 0)
		wake_up(&htt->empty_tx_wq);
}

/* hold conf_mutex for simple iteration, or conf_mutex+data_lock for
 * modifications */
struct ath10k_peer *ath10k_peer_find(struct ath10k *ar, int vdev_id,
				     const u8 *addr)
{
	struct ath10k_peer *peer;

	list_for_each_entry(peer, &ar->peers, list) {
		if (peer->vdev_id != vdev_id)
			continue;
		if (memcmp(peer->addr, addr, ETH_ALEN))
			continue;

		return peer;
	}

	return NULL;
}

static struct ath10k_peer *ath10k_peer_find_by_id(struct ath10k *ar,
						  int peer_id)
{
	struct ath10k_peer *peer;

	lockdep_assert_held(&ar->data_lock);

	list_for_each_entry(peer, &ar->peers, list)
		if (test_bit(peer_id, peer->peer_ids))
			return peer;

	return NULL;
}

static int ath10k_wait_for_peer_common(struct ath10k *ar, int vdev_id,
				       const u8 *addr, bool expect_mapped)
{
	int ret;

	ret = wait_event_timeout(ar->peer_mapping_wq, ({
			struct ath10k_peer *peer;
			bool mapped = false;

			spin_lock_bh(&ar->data_lock);
			peer = ath10k_peer_find(ar, vdev_id, addr);
			if (peer)
				mapped = !bitmap_empty(peer->peer_ids,
						       ATH10K_MAX_NUM_PEER_IDS);
			spin_unlock_bh(&ar->data_lock);

			mapped == expect_mapped;
		}), 3*HZ);

	if (ret <= 0)
		return -ETIMEDOUT;

	return 0;
}

int ath10k_wait_for_peer_created(struct ath10k *ar, int vdev_id, const u8 *addr)
{
	return ath10k_wait_for_peer_common(ar, vdev_id, addr, true);
}

int ath10k_wait_for_peer_deleted(struct ath10k *ar, int vdev_id, const u8 *addr)
{
	return ath10k_wait_for_peer_common(ar, vdev_id, addr, false);
}

void ath10k_peer_map_event(struct ath10k_htt *htt,
			   struct htt_peer_map_event *ev)
{
	struct ath10k *ar = htt->ar;
	struct ath10k_peer *peer;

	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find(ar, ev->vdev_id, ev->addr);
	if (!peer) {
		ath10k_warn("failed to map peer %pM on vdev %i: no such entry\n",
			    ev->addr, ev->vdev_id);
		goto exit;
	}

	ath10k_dbg(ATH10K_DBG_HTT, "htt peer map vdev %d peer %pM id %d\n",
		   ev->vdev_id, ev->addr, ev->peer_id);

	set_bit(ev->peer_id, peer->peer_ids);
	wake_up(&ar->peer_mapping_wq);
exit:
	spin_unlock_bh(&ar->data_lock);
}

void ath10k_peer_unmap_event(struct ath10k_htt *htt,
			     struct htt_peer_unmap_event *ev)
{
	struct ath10k *ar = htt->ar;
	struct ath10k_peer *peer;

	spin_lock_bh(&ar->data_lock);
	peer = ath10k_peer_find_by_id(ar, ev->peer_id);
	if (!peer) {
		ath10k_warn("peer-unmap-event: unknown peer id %d\n",
			    ev->peer_id);
		goto exit;
	}

	ath10k_dbg(ATH10K_DBG_HTT, "htt peer unmap vdev %d peer %pM id %d\n",
		   peer->vdev_id, peer->addr, ev->peer_id);

	clear_bit(ev->peer_id, peer->peer_ids);

	if (bitmap_empty(peer->peer_ids, ATH10K_MAX_NUM_PEER_IDS))
		wake_up(&ar->peer_mapping_wq);

exit:
	spin_unlock_bh(&ar->data_lock);
}
