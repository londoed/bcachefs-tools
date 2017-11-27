#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <uuid/uuid.h>

#include "libbcachefs/bcachefs_format.h"
#include "libbcachefs/checksum.h"
#include "crypto.h"
#include "libbcachefs.h"
#include "libbcachefs/opts.h"
#include "libbcachefs/super-io.h"

#define NSEC_PER_SEC	1000000000L

#define BCH_MIN_NR_NBUCKETS	(1 << 10)

/* minimum size filesystem we can create, given a bucket size: */
static u64 min_size(unsigned bucket_size)
{
	return BCH_MIN_NR_NBUCKETS * bucket_size;
}

static void init_layout(struct bch_sb_layout *l, unsigned block_size,
			u64 start, u64 end)
{
	unsigned sb_size;
	u64 backup; /* offset of 2nd sb */

	memset(l, 0, sizeof(*l));

	if (start != BCH_SB_SECTOR)
		start = round_up(start, block_size);
	end = round_down(end, block_size);

	if (start >= end)
		die("insufficient space for superblocks");

	/*
	 * Create two superblocks in the allowed range: reserve a maximum of 64k
	 */
	sb_size = min_t(u64, 128, end - start / 2);

	backup = start + sb_size;
	backup = round_up(backup, block_size);

	backup = min(backup, end);

	sb_size = min(end - backup, backup- start);
	sb_size = rounddown_pow_of_two(sb_size);

	if (sb_size < 8)
		die("insufficient space for superblocks");

	l->magic		= BCACHE_MAGIC;
	l->layout_type		= 0;
	l->nr_superblocks	= 2;
	l->sb_max_size_bits	= ilog2(sb_size);
	l->sb_offset[0]		= cpu_to_le64(start);
	l->sb_offset[1]		= cpu_to_le64(backup);
}

void bch2_pick_bucket_size(struct format_opts opts, struct dev_opts *dev)
{
	if (!dev->sb_offset) {
		dev->sb_offset	= BCH_SB_SECTOR;
		dev->sb_end	= BCH_SB_SECTOR + 256;
	}

	if (!dev->size)
		dev->size = get_size(dev->path, dev->fd) >> 9;

	if (!dev->bucket_size) {
		if (dev->size < min_size(opts.block_size))
			die("cannot format %s, too small (%llu sectors, min %llu)",
			    dev->path, dev->size, min_size(opts.block_size));

		/* Bucket size must be >= block size: */
		dev->bucket_size = opts.block_size;

		/* Bucket size must be >= btree node size: */
		dev->bucket_size = max(dev->bucket_size, opts.btree_node_size);

		/* Want a bucket size of at least 128k, if possible: */
		dev->bucket_size = max(dev->bucket_size, 256U);

		if (dev->size >= min_size(dev->bucket_size)) {
			unsigned scale = max(1,
					     ilog2(dev->size / min_size(dev->bucket_size)) / 4);

			scale = rounddown_pow_of_two(scale);

			/* max bucket size 1 mb */
			dev->bucket_size = min(dev->bucket_size * scale, 1U << 11);
		} else {
			do {
				dev->bucket_size /= 2;
			} while (dev->size < min_size(dev->bucket_size));
		}
	}

	dev->nbuckets	= dev->size / dev->bucket_size;

	if (dev->bucket_size < opts.block_size)
		die("Bucket size cannot be smaller than block size");

	if (dev->bucket_size < opts.btree_node_size)
		die("Bucket size cannot be smaller than btree node size");

	if (dev->nbuckets < BCH_MIN_NR_NBUCKETS)
		die("Not enough buckets: %llu, need %u (bucket size %u)",
		    dev->nbuckets, BCH_MIN_NR_NBUCKETS, dev->bucket_size);

}

struct bch_sb *bch2_format(struct format_opts opts,
			   struct dev_opts *devs, size_t nr_devs)
{
	struct bch_sb *sb;
	struct dev_opts *i;
	struct bch_sb_field_members *mi;
	unsigned u64s;

	/* calculate block size: */
	if (!opts.block_size)
		for (i = devs; i < devs + nr_devs; i++)
			opts.block_size = max(opts.block_size,
					      get_blocksize(i->path, i->fd));

	/* calculate bucket sizes: */
	for (i = devs; i < devs + nr_devs; i++)
		bch2_pick_bucket_size(opts, i);

	/* calculate btree node size: */
	if (!opts.btree_node_size) {
		/* 256k default btree node size */
		opts.btree_node_size = 512;

		for (i = devs; i < devs + nr_devs; i++)
			opts.btree_node_size =
				min(opts.btree_node_size, i->bucket_size);
	}

	if (!is_power_of_2(opts.block_size))
		die("block size must be power of 2");

	if (!is_power_of_2(opts.btree_node_size))
		die("btree node size must be power of 2");

	if (uuid_is_null(opts.uuid.b))
		uuid_generate(opts.uuid.b);

	sb = calloc(1, sizeof(*sb) +
		    sizeof(struct bch_sb_field_members) +
		    sizeof(struct bch_member) * nr_devs +
		    sizeof(struct bch_sb_field_crypt));

	sb->version	= cpu_to_le64(BCH_SB_VERSION_MAX);
	sb->magic	= BCACHE_MAGIC;
	sb->block_size	= cpu_to_le16(opts.block_size);
	sb->user_uuid	= opts.uuid;
	sb->nr_devices	= nr_devs;

	uuid_generate(sb->uuid.b);

	if (opts.label)
		strncpy((char *) sb->label, opts.label, sizeof(sb->label));

	SET_BCH_SB_CSUM_TYPE(sb,		opts.meta_csum_type);
	SET_BCH_SB_META_CSUM_TYPE(sb,		opts.meta_csum_type);
	SET_BCH_SB_DATA_CSUM_TYPE(sb,		opts.data_csum_type);
	SET_BCH_SB_COMPRESSION_TYPE(sb,		opts.compression_type);

	SET_BCH_SB_BTREE_NODE_SIZE(sb,		opts.btree_node_size);
	SET_BCH_SB_GC_RESERVE(sb,		8);
	SET_BCH_SB_META_REPLICAS_WANT(sb,	opts.meta_replicas);
	SET_BCH_SB_META_REPLICAS_REQ(sb,	opts.meta_replicas_required);
	SET_BCH_SB_DATA_REPLICAS_WANT(sb,	opts.data_replicas);
	SET_BCH_SB_DATA_REPLICAS_REQ(sb,	opts.data_replicas_required);
	SET_BCH_SB_ERROR_ACTION(sb,		opts.on_error_action);
	SET_BCH_SB_STR_HASH_TYPE(sb,		BCH_STR_HASH_SIPHASH);
	SET_BCH_SB_ENCODED_EXTENT_MAX_BITS(sb,	ilog2(opts.encoded_extent_max));

	SET_BCH_SB_POSIX_ACL(sb,		1);

	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now))
		die("error getting current time: %m");

	sb->time_base_lo	= cpu_to_le64(now.tv_sec * NSEC_PER_SEC + now.tv_nsec);
	sb->time_precision	= cpu_to_le32(1);

	if (opts.encrypted) {
		struct bch_sb_field_crypt *crypt = vstruct_end(sb);

		u64s = sizeof(struct bch_sb_field_crypt) / sizeof(u64);

		le32_add_cpu(&sb->u64s, u64s);
		crypt->field.u64s = cpu_to_le32(u64s);
		crypt->field.type = BCH_SB_FIELD_crypt;

		bch_sb_crypt_init(sb, crypt, opts.passphrase);
		SET_BCH_SB_ENCRYPTION_TYPE(sb, 1);
	}

	mi = vstruct_end(sb);
	u64s = (sizeof(struct bch_sb_field_members) +
		sizeof(struct bch_member) * nr_devs) / sizeof(u64);

	le32_add_cpu(&sb->u64s, u64s);
	mi->field.u64s = cpu_to_le32(u64s);
	mi->field.type = BCH_SB_FIELD_members;

	for (i = devs; i < devs + nr_devs; i++) {
		struct bch_member *m = mi->members + (i - devs);

		uuid_generate(m->uuid.b);
		m->nbuckets	= cpu_to_le64(i->nbuckets);
		m->first_bucket	= 0;
		m->bucket_size	= cpu_to_le16(i->bucket_size);

		SET_BCH_MEMBER_TIER(m,		i->tier);
		SET_BCH_MEMBER_REPLACEMENT(m,	CACHE_REPLACEMENT_LRU);
		SET_BCH_MEMBER_DISCARD(m,	i->discard);
		SET_BCH_MEMBER_DATA_ALLOWED(m,	i->data_allowed);
	}

	for (i = devs; i < devs + nr_devs; i++) {
		sb->dev_idx = i - devs;

		init_layout(&sb->layout, opts.block_size,
			    i->sb_offset, i->sb_end);

		if (i->sb_offset == BCH_SB_SECTOR) {
			/* Zero start of disk */
			static const char zeroes[BCH_SB_SECTOR << 9];

			xpwrite(i->fd, zeroes, BCH_SB_SECTOR << 9, 0);
		}

		bch2_super_write(i->fd, sb);
		close(i->fd);
	}

	return sb;
}

void bch2_super_write(int fd, struct bch_sb *sb)
{
	struct nonce nonce = { 0 };

	unsigned i;
	for (i = 0; i < sb->layout.nr_superblocks; i++) {
		sb->offset = sb->layout.sb_offset[i];

		if (sb->offset == BCH_SB_SECTOR) {
			/* Write backup layout */
			xpwrite(fd, &sb->layout, sizeof(sb->layout),
				BCH_SB_LAYOUT_SECTOR << 9);
		}

		sb->csum = csum_vstruct(NULL, BCH_SB_CSUM_TYPE(sb), nonce, sb);
		xpwrite(fd, sb, vstruct_bytes(sb),
			le64_to_cpu(sb->offset) << 9);
	}

	fsync(fd);
}

struct bch_sb *__bch2_super_read(int fd, u64 sector)
{
	struct bch_sb sb, *ret;

	xpread(fd, &sb, sizeof(sb), sector << 9);

	if (memcmp(&sb.magic, &BCACHE_MAGIC, sizeof(sb.magic)))
		die("not a bcachefs superblock");

	size_t bytes = vstruct_bytes(&sb);

	ret = malloc(bytes);

	xpread(fd, ret, bytes, sector << 9);

	return ret;
}

static unsigned get_dev_has_data(struct bch_sb *sb, unsigned dev)
{
	struct bch_sb_field_replicas *replicas;
	struct bch_replicas_entry *r;
	unsigned i, data_has = 0;

	replicas = bch2_sb_get_replicas(sb);

	if (replicas)
		for_each_replicas_entry(replicas, r)
			for (i = 0; i < r->nr; i++)
				if (r->devs[i] == dev)
					data_has |= 1 << r->data_type;

	return data_has;
}

void bch2_super_print(struct bch_sb *sb, int units)
{
	struct bch_sb_field_members *mi;
	char user_uuid_str[40], internal_uuid_str[40];
	char label[BCH_SB_LABEL_SIZE + 1];
	unsigned i;

	memset(label, 0, sizeof(label));
	memcpy(label, sb->label, sizeof(sb->label));
	uuid_unparse(sb->user_uuid.b, user_uuid_str);
	uuid_unparse(sb->uuid.b, internal_uuid_str);

	printf("External UUID:			%s\n"
	       "Internal UUID:			%s\n"
	       "Label:				%s\n"
	       "Version:			%llu\n"
	       "Block_size:			%s\n"
	       "Btree node size:		%s\n"
	       "Error action:			%s\n"
	       "Clean:				%llu\n"

	       "Metadata replicas:		have %llu, want %llu\n"
	       "Data replicas:			have %llu, want %llu\n"

	       "Metadata checksum type:		%s\n"
	       "Data checksum type:		%s\n"
	       "Compression type:		%s\n"

	       "String hash type:		%s\n"
	       "32 bit inodes:			%llu\n"
	       "GC reserve percentage:		%llu%%\n"
	       "Root reserve percentage:	%llu%%\n"

	       "Devices:			%u\n",
	       user_uuid_str,
	       internal_uuid_str,
	       label,
	       le64_to_cpu(sb->version),
	       pr_units(le16_to_cpu(sb->block_size), units),
	       pr_units(BCH_SB_BTREE_NODE_SIZE(sb), units),

	       BCH_SB_ERROR_ACTION(sb) < BCH_NR_ERROR_ACTIONS
	       ? bch2_error_actions[BCH_SB_ERROR_ACTION(sb)]
	       : "unknown",

	       BCH_SB_CLEAN(sb),

	       0LLU, //BCH_SB_META_REPLICAS_HAVE(sb),
	       BCH_SB_META_REPLICAS_WANT(sb),
	       0LLU, //BCH_SB_DATA_REPLICAS_HAVE(sb),
	       BCH_SB_DATA_REPLICAS_WANT(sb),

	       BCH_SB_META_CSUM_TYPE(sb) < BCH_CSUM_NR
	       ? bch2_csum_types[BCH_SB_META_CSUM_TYPE(sb)]
	       : "unknown",

	       BCH_SB_DATA_CSUM_TYPE(sb) < BCH_CSUM_NR
	       ? bch2_csum_types[BCH_SB_DATA_CSUM_TYPE(sb)]
	       : "unknown",

	       BCH_SB_COMPRESSION_TYPE(sb) < BCH_COMPRESSION_NR
	       ? bch2_compression_types[BCH_SB_COMPRESSION_TYPE(sb)]
	       : "unknown",

	       BCH_SB_STR_HASH_TYPE(sb) < BCH_STR_HASH_NR
	       ? bch2_str_hash_types[BCH_SB_STR_HASH_TYPE(sb)]
	       : "unknown",

	       BCH_SB_INODE_32BIT(sb),
	       BCH_SB_GC_RESERVE(sb),
	       BCH_SB_ROOT_RESERVE(sb),

	       sb->nr_devices);

	mi = bch2_sb_get_members(sb);
	if (!mi) {
		printf("Member info section missing\n");
		return;
	}

	for (i = 0; i < sb->nr_devices; i++) {
		struct bch_member *m = mi->members + i;
		time_t last_mount = le64_to_cpu(m->last_mount);
		char member_uuid_str[40];
		char data_allowed_str[100];
		char data_has_str[100];

		uuid_unparse(m->uuid.b, member_uuid_str);
		bch2_scnprint_flag_list(data_allowed_str,
					sizeof(data_allowed_str),
					bch2_data_types,
					BCH_MEMBER_DATA_ALLOWED(m));
		if (!data_allowed_str[0])
			strcpy(data_allowed_str, "(none)");

		bch2_scnprint_flag_list(data_has_str,
					sizeof(data_has_str),
					bch2_data_types,
					get_dev_has_data(sb, i));
		if (!data_has_str[0])
			strcpy(data_has_str, "(none)");

		printf("\n"
		       "Device %u:\n"
		       "  UUID:				%s\n"
		       "  Size:				%s\n"
		       "  Bucket size:			%s\n"
		       "  First bucket:			%u\n"
		       "  Buckets:			%llu\n"
		       "  Last mount:			%s\n"
		       "  State:			%s\n"
		       "  Tier:				%llu\n"
		       "  Data allowed:			%s\n"

		       "  Has data:			%s\n"

		       "  Replacement policy:		%s\n"
		       "  Discard:			%llu\n",
		       i, member_uuid_str,
		       pr_units(le16_to_cpu(m->bucket_size) *
				le64_to_cpu(m->nbuckets), units),
		       pr_units(le16_to_cpu(m->bucket_size), units),
		       le16_to_cpu(m->first_bucket),
		       le64_to_cpu(m->nbuckets),
		       last_mount ? ctime(&last_mount) : "(never)",

		       BCH_MEMBER_STATE(m) < BCH_MEMBER_STATE_NR
		       ? bch2_dev_state[BCH_MEMBER_STATE(m)]
		       : "unknown",

		       BCH_MEMBER_TIER(m),
		       data_allowed_str,
		       data_has_str,

		       BCH_MEMBER_REPLACEMENT(m) < CACHE_REPLACEMENT_NR
		       ? bch2_cache_replacement_policies[BCH_MEMBER_REPLACEMENT(m)]
		       : "unknown",

		       BCH_MEMBER_DISCARD(m));
	}
}
