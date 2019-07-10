/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_RECOVERY_H
#define _BCACHEFS_RECOVERY_H

struct journal_keys {
	struct journal_key {
		enum btree_id	btree_id:8;
		unsigned	allocated:1;
		struct bpos	pos;
		struct bkey_i	*k;
		u32		journal_seq;
		u32		journal_offset;
	}			*d;
	size_t			nr;
	u64			journal_seq_base;
};

#define for_each_journal_key(keys, i)				\
	for (i = (keys).d; i < (keys).d + (keys).nr; (i)++)

int bch2_fs_recovery(struct bch_fs *);
int bch2_fs_initialize(struct bch_fs *);

#endif /* _BCACHEFS_RECOVERY_H */
