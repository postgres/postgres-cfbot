/*-------------------------------------------------------------------------
 *
 * fileops.h
 *	  Transactional file operations API
 *
 * This module provides transactional filesystem operations that are
 * WAL-logged and integrated with PostgreSQL's transaction management.
 * Constructive operations and renames take effect at once and are reversed
 * if the transaction aborts; deletions are deferred to transaction end (see
 * src/backend/storage/file/README).
 *
 * The RM_FILEOPS_ID resource manager handles WAL replay for these
 * operations, ensuring correct behavior during crash recovery and
 * standby replay.
 *
 * The operation set follows the Berkeley DB fileops.src model: each
 * filesystem operation is a composable unit with its own WAL record
 * type, redo handler, and descriptor.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fileops.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILEOPS_H
#define FILEOPS_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/*
 * WAL record types for FILEOPS operations.
 *
 * The high 4 bits of the info byte are used for record type,
 * leaving the low bits for flags (following PostgreSQL convention).
 *
 * Following the Berkeley DB fileops.src model, each filesystem
 * operation has its own WAL record type for independent redo.
 */
#define XLOG_FILEOPS_CREATE			0x00
#define XLOG_FILEOPS_DELETE			0x10
#define XLOG_FILEOPS_RENAME			0x20
#define XLOG_FILEOPS_WRITE			0x30
#define XLOG_FILEOPS_TRUNCATE		0x40
#define XLOG_FILEOPS_CHMOD			0x50
#define XLOG_FILEOPS_CHOWN			0x60
#define XLOG_FILEOPS_MKDIR			0x70
#define XLOG_FILEOPS_RMDIR			0x80
#define XLOG_FILEOPS_SYMLINK		0x90
#define XLOG_FILEOPS_LINK			0xA0
#define XLOG_FILEOPS_SETXATTR		0xB0
#define XLOG_FILEOPS_REMOVEXATTR	0xC0

/*
 * PendingFileOp - Deferred file operation entry
 *
 * File operations are collected in a linked list during a transaction
 * and executed at commit or abort time. This follows the same pattern
 * used by PendingRelDelete in catalog/storage.c.
 */
typedef enum PendingFileOpType
{
	PENDING_FILEOP_CREATE,
	PENDING_FILEOP_DELETE,
	PENDING_FILEOP_RENAME,
	PENDING_FILEOP_WRITE,
	PENDING_FILEOP_TRUNCATE,
	PENDING_FILEOP_CHMOD,
	PENDING_FILEOP_CHOWN,
	PENDING_FILEOP_MKDIR,
	PENDING_FILEOP_RMDIR,
	PENDING_FILEOP_RMTREE,		/* abort: remove a FileOpsMkdirTree() tree */
	PENDING_FILEOP_SYMLINK,
	PENDING_FILEOP_LINK,
	PENDING_FILEOP_SETXATTR,
	PENDING_FILEOP_REMOVEXATTR,
	PENDING_FILEOP_INVERSE
} PendingFileOpType;

typedef struct PendingFileOp
{
	PendingFileOpType type;		/* operation type */
	char	   *path;			/* primary file path */
	char	   *newpath;		/* old path to restore (RENAME), or xattr name */
	off_t		length;			/* truncation length, write offset, or mode */
	void	   *data;			/* generic data (e.g., original xattr value) */
	size_t		data_len;		/* length of data */
	bool		at_commit;		/* execute at commit (true) or abort (false) */
	int			nestLevel;		/* transaction nesting level */
	struct PendingFileOp *next; /* linked list link */
} PendingFileOp;

/*
 * Public API for transactional file operations
 *
 * These functions handle platform-specific differences automatically:
 *   - O_DIRECT: PG_O_DIRECT (Linux/FreeBSD native, macOS F_NOCACHE,
 *     Windows FILE_FLAG_NO_BUFFERING)
 *   - fsync: pg_fsync() (Linux fdatasync, macOS F_FULLFSYNC,
 *     BSD fsync, Windows FlushFileBuffers)
 *   - Directory sync: fsync_parent_path() (Unix only, no-op on Windows)
 *
 * Operation-specific API functions are declared below their WAL
 * record structures in subsequent sections.
 */

/* Utility functions */
extern void FileOpsCancelPendingDelete(const char *path, bool at_commit);
extern void FileOpsSync(const char *path);

/* Abort WAL carries a length-prefixed sequence of inverse payloads. */
#define FILEOPS_MAX_ABORT_BYTES (16 * 1024 * 1024)
#define FILEOPS_MAX_DATA_BYTES (1024 * 1024)
#define FILEOPS_TWOPHASE_INVERSES 1
extern void FileOpsGetAbortData(char **data, uint32 *len);
extern void FileOpsApplyAbort(const char *data, uint32 len);

/* Scoped to a generic walk whose FILEOPS inverses belong to abort WAL. */
extern PGDLLIMPORT bool FileOpsSkipUndo;
extern void FileOpsApplyInverse(uint16 info, const char *data, uint32 len);
extern void FileOpsReserveInverseSyncs(void);
extern void FileOpsSyncInverses(int elevel);
extern void FileOpsBeginAbort(void);
extern void FileOpsEndAbort(void);

/* Transaction lifecycle hooks */
extern void FileOpsDoPendingOps(bool isCommit);
extern void AtSubCommit_FileOps(void);
extern void AtSubAbort_FileOps(void);
extern void PostPrepare_FileOps(void);
extern void FileOpsAtPrepare(void);

/*
 * xl_fileops_create - WAL record for file creation
 *
 * Records that a file was created within a transaction. If the transaction
 * aborts, the file will be deleted. The path is stored as variable-length
 * data following the fixed header.
 */
typedef struct xl_fileops_create
{
	int			flags;			/* open flags used for creation */
	mode_t		mode;			/* file permission mode */
	bool		register_delete;	/* register for delete-on-abort */
	/* variable-length path follows */
} xl_fileops_create;

#define SizeOfFileOpsCreate (offsetof(xl_fileops_create, register_delete) + sizeof(bool))

/* File creation API */
extern int	FileOpsCreate(const char *path, int flags, mode_t mode,
						  bool register_delete);

/*
 * xl_fileops_delete - WAL record for file deletion
 *
 * Records that a file deletion was requested. The at_commit flag indicates
 * whether the deletion should happen at commit (true) or was registered
 * as a delete-on-abort from a prior create (false).
 */
typedef struct xl_fileops_delete
{
	bool		at_commit;		/* true = delete at commit, false = at abort */
	/* variable-length path follows */
} xl_fileops_delete;

#define SizeOfFileOpsDelete (offsetof(xl_fileops_delete, at_commit) + sizeof(bool))

/* File deletion API */
extern void FileOpsDelete(const char *path, bool at_commit);

/*
 * xl_fileops_rename - WAL record for file rename
 *
 * Records that a file or directory is being renamed; it is written before
 * the rename is made.  Both old and new paths are stored as variable-length
 * data: oldpath_len bytes of old path, then the new path follows.
 */
typedef struct xl_fileops_rename
{
	uint16		oldpath_len;	/* length of old path (including NUL) */
	/* variable-length old path follows, then new path */
} xl_fileops_rename;

#define SizeOfFileOpsRename (offsetof(xl_fileops_rename, oldpath_len) + sizeof(uint16))

/*
 * File rename API: immediate, refuses an existing newpath, renamed back if
 * the transaction aborts.
 */
extern int	FileOpsRename(const char *oldpath, const char *newpath);

/*
 * xl_fileops_write - WAL record for file write at offset
 *
 * Records that data was written to a file at a specific offset.
 * The path and data are stored as variable-length data following
 * the fixed header.
 */
typedef struct xl_fileops_write
{
	off_t		offset;			/* write offset in file */
	uint32		len;			/* data length */
	uint16		path_len;		/* length of path (including NUL) */
	/* variable-length path follows, then data */
} xl_fileops_write;

#define SizeOfFileOpsWrite (offsetof(xl_fileops_write, path_len) + sizeof(uint16))

/* File write API */
extern int	FileOpsWrite(const char *path, off_t offset,
						 const void *data, uint32 len);

/*
 * xl_fileops_truncate - WAL record for file truncation
 */
typedef struct xl_fileops_truncate
{
	off_t		length;			/* new file length */
	/* variable-length path follows */
} xl_fileops_truncate;

#define SizeOfFileOpsTruncate (offsetof(xl_fileops_truncate, length) + sizeof(off_t))

/* File truncation API */
extern void FileOpsTruncate(const char *path, off_t length);

/*
 * xl_fileops_chmod - WAL record for file permission change
 */
typedef struct xl_fileops_chmod
{
	mode_t		mode;			/* new permission mode */
	mode_t		orig_mode;		/* pre-chmod mode (crash-revert before-state) */
	/* variable-length path follows */
} xl_fileops_chmod;

#define SizeOfFileOpsChmod (offsetof(xl_fileops_chmod, orig_mode) + sizeof(mode_t))

/*
 * xl_fileops_chown - WAL record for file ownership change
 */
typedef struct xl_fileops_chown
{
	uid_t		uid;			/* new owner user id */
	gid_t		gid;			/* new owner group id */
	uid_t		orig_uid;		/* pre-chown uid (crash-revert before-state) */
	gid_t		orig_gid;		/* pre-chown gid (crash-revert before-state) */
	/* variable-length path follows */
} xl_fileops_chown;

#define SizeOfFileOpsChown (offsetof(xl_fileops_chown, orig_gid) + sizeof(gid_t))

/* File metadata API */
extern int	FileOpsChmod(const char *path, mode_t mode);
extern int	FileOpsChown(const char *path, uid_t uid, gid_t gid);

/*
 * xl_fileops_mkdir - WAL record for directory creation
 */
typedef struct xl_fileops_mkdir
{
	mode_t		mode;			/* directory permission mode */
	uint16		flags;			/* FILEOPS_MKDIR_* */
	/* variable-length path follows */
} xl_fileops_mkdir;

#define SizeOfFileOpsMkdir (offsetof(xl_fileops_mkdir, flags) + sizeof(uint16))

/*
 * The transaction fills the directory itself (with files FILEOPS does not
 * track, e.g. through smgr), so undoing the mkdir removes the whole tree.
 */
#define FILEOPS_MKDIR_TREE		0x0001

/*
 * xl_fileops_rmdir - WAL record for directory removal
 */
typedef struct xl_fileops_rmdir
{
	bool		at_commit;		/* true = rmdir at commit, false = at abort */
	/* variable-length path follows */
} xl_fileops_rmdir;

#define SizeOfFileOpsRmdir (offsetof(xl_fileops_rmdir, at_commit) + sizeof(bool))

/* Directory lifecycle API */
extern int	FileOpsMkdir(const char *path, mode_t mode);
extern int	FileOpsMkdirTree(const char *path, mode_t mode, Oid dboid);
extern void FileOpsRmdir(const char *path, bool at_commit);
extern void FileOpsRmdirRecursive(const char *path, bool at_commit);

/*
 * xl_fileops_symlink - WAL record for symbolic link creation
 */
typedef struct xl_fileops_symlink
{
	uint16		target_len;		/* length of target (including NUL) */
	/* variable-length target follows, then linkpath */
} xl_fileops_symlink;

#define SizeOfFileOpsSymlink (offsetof(xl_fileops_symlink, target_len) + sizeof(uint16))

/*
 * xl_fileops_link - WAL record for hard link creation
 */
typedef struct xl_fileops_link
{
	uint16		oldpath_len;	/* length of old path (including NUL) */
	/* variable-length old path follows, then new path */
} xl_fileops_link;

#define SizeOfFileOpsLink (offsetof(xl_fileops_link, oldpath_len) + sizeof(uint16))

/* Link operations API */
extern int	FileOpsSymlink(const char *target, const char *linkpath);
extern int	FileOpsLink(const char *oldpath, const char *newpath);

/*
 * xl_fileops_setxattr - WAL record for setting an extended attribute
 */
typedef struct xl_fileops_setxattr
{
	uint16		name_len;		/* attribute name length (including NUL) */
	uint32		value_len;		/* attribute value length */
	uint16		path_len;		/* file path length (including NUL) */
	uint32		orig_value_len; /* pre-set value length (crash-revert) */
	bool		had_value;		/* true if xattr existed before this set */
	/* variable-length: path, name, value, orig_value (if had_value) */
} xl_fileops_setxattr;

#define SizeOfFileOpsSetxattr (offsetof(xl_fileops_setxattr, had_value) + sizeof(bool))

/*
 * xl_fileops_removexattr - WAL record for removing an extended attribute
 */
typedef struct xl_fileops_removexattr
{
	uint16		name_len;		/* attribute name length (including NUL) */
	uint16		path_len;		/* file path length (including NUL) */
	uint32		orig_value_len; /* removed value length (crash-revert); 0 =
								 * none */
	bool		had_value;		/* true if xattr existed before this remove */
	/* variable-length: path, name, orig_value (if had_value) */
} xl_fileops_removexattr;

#define SizeOfFileOpsRemovexattr (offsetof(xl_fileops_removexattr, had_value) + sizeof(bool))

/* Extended attribute API */
extern int	FileOpsSetXattr(const char *path, const char *name,
							const void *value, size_t len);
extern int	FileOpsRemoveXattr(const char *path, const char *name);

/* WAL redo and descriptor functions */
extern void fileops_redo(XLogReaderState *record);
extern void fileops_desc(StringInfo buf, XLogReaderState *record);
extern const char *fileops_identify(uint8 info);

/* FILEOPS UNDO RM (fileops_undo.c) */
extern void FileopsUndoRmgrInit(void);

/*
 * UNDO resource-manager ID for FILEOPS (stored in urec_rmid).  Defined here,
 * with FILEOPS, rather than in the generic access/undormgr.h, so the UNDO
 * core names no specific consumer.  See access/undormgr.h for the shared,
 * WAL-durable ID number space.
 */
#define UNDO_RMID_FILEOPS	2

/* FILEOPS UNDO subtypes (stored in urec_info) */
#define FILEOPS_UNDO_WRITE           0x0002
#define FILEOPS_UNDO_CREATE			0x0001
#define FILEOPS_UNDO_TRUNCATE		0x0003
#define FILEOPS_UNDO_CHMOD			0x0004
#define FILEOPS_UNDO_CHOWN			0x0005
#define FILEOPS_UNDO_MKDIR			0x0006
#define FILEOPS_UNDO_SYMLINK		0x0007
#define FILEOPS_UNDO_LINK			0x0008
#define FILEOPS_UNDO_SETXATTR		0x0009
#define FILEOPS_UNDO_REMOVEXATTR	0x000A
#define FILEOPS_UNDO_RENAME			0x000B

/*
 * FILEOPS UNDO payload structures
 *
 * These structs define the on-disk layout of UNDO record payloads for each
 * FILEOPS operation. Variable-length paths and data follow the fixed header.
 *
 * A chain can be walked more than once (an inline rollback that fails part
 * way is handed whole to the logical revert worker, and so is a crash-recovery
 * chain with a skipped record), and the object may have changed in between.
 * So each payload also records what the operation left behind (its post-state),
 * and fileops_undo_apply() reverts only an object still in that state.
 */

/* CREATE undo payload: the path to unlink, if it is still a regular file */
typedef struct FileopsUndoCreate
{
	uint16		path_len;		/* including NUL */
	/* followed by path */
} FileopsUndoCreate;

/* WRITE undo: path, before-image, then after-image. */
typedef struct FileopsUndoWrite
{
	off_t		orig_length;
	off_t		new_length;
	off_t		offset;
	uint32		before_len;
	uint32		after_len;
	uint16		path_len;
} FileopsUndoWrite;

/* TRUNCATE undo payload: path, followed by the bytes removed by a shrink. */
typedef struct FileopsUndoTruncate
{
	off_t		orig_length;
	off_t		new_length;
	uint32		before_len;
	uint16		path_len;
	/* followed by path */
} FileopsUndoTruncate;

/* CHMOD undo payload: path + original and new mode */
typedef struct FileopsUndoChmod
{
	mode_t		orig_mode;
	mode_t		new_mode;
	uint16		path_len;
	/* followed by path */
} FileopsUndoChmod;

/* CHOWN undo payload: path + original and new uid/gid */
typedef struct FileopsUndoChown
{
	uid_t		orig_uid;
	gid_t		orig_gid;
	uid_t		new_uid;
	gid_t		new_gid;
	uint16		path_len;
	/* followed by path */
} FileopsUndoChown;

/*
 * MKDIR undo payload: the path to rmdir, if it is still a directory.
 *
 * With FILEOPS_MKDIR_TREE the whole tree is removed instead, after dropping
 * the shared buffers and pending fsync requests of database dboid (if valid),
 * as a database directory's files are written through shared buffers.  Since
 * that destroys whatever is in the tree, outside recovery it is done only
 * while the path is still the directory created (dev, ino): a later walk of
 * the chain must not remove a directory created again at the same path, e.g.
 * by a retried ALTER DATABASE SET TABLESPACE.  Crash recovery cannot rely on
 * the inode (redo on a standby or a restored backup creates the directory
 * anew), and needs not: until the transaction's locks are gone no other
 * transaction can have made that path.  Even so, the inode is evidence that
 * the directory is ours, not proof.
 */
typedef struct FileopsUndoMkdir
{
	dev_t		dev;			/* the directory created */
	ino_t		ino;
	Oid			dboid;			/* database filling the tree, or InvalidOid */
	uint16		flags;			/* FILEOPS_MKDIR_* */
	uint16		path_len;
	/* followed by path */
} FileopsUndoMkdir;

/* SYMLINK undo payload: the linkpath to unlink, if it still points to target */
typedef struct FileopsUndoSymlink
{
	uint16		linkpath_len;
	uint16		target_len;
	/* followed by linkpath, then target */
} FileopsUndoSymlink;

/* LINK undo payload: the newpath to unlink, if it is still the linked inode */
typedef struct FileopsUndoLink
{
	dev_t		dev;			/* device and inode of the link source */
	ino_t		ino;
	uint16		newpath_len;
	/* followed by newpath */
} FileopsUndoLink;

/*
 * RENAME undo payload: rename newpath back to oldpath, while newpath exists
 * and oldpath does not.  Like CREATE and MKDIR, the test is the state of the
 * two paths alone, not the device and inode of the object renamed: those are
 * not stable across a restore, a promoted standby or redo that makes the
 * object again, so ROLLBACK PREPARED on a promoted standby, for one, would
 * take its own copy of the object for a different one and leave it renamed.
 * The paths are evidence that the object is the one renamed, not proof.
 */
typedef struct FileopsUndoRename
{
	uint16		oldpath_len;	/* including NUL */
	uint16		newpath_len;	/* including NUL */
	/* followed by oldpath, then newpath */
} FileopsUndoRename;

/* SETXATTR undo payload: path + name + original value (if any) + new value */
typedef struct FileopsUndoSetxattr
{
	uint16		path_len;
	uint16		name_len;
	uint32		orig_value_len; /* 0 if xattr didn't exist before */
	uint32		new_value_len;
	bool		had_value;		/* true if xattr existed before setxattr */
	/* followed by path, name, original value (if had_value), new value */
} FileopsUndoSetxattr;

/* REMOVEXATTR undo payload: path + name + removed value */
typedef struct FileopsUndoRemovexattr
{
	uint16		path_len;
	uint16		name_len;
	uint32		value_len;
	/* followed by path, name, value */
} FileopsUndoRemovexattr;

/* Recursive directory removal */
extern void FileOpsRmdirRecursive(const char *path, bool at_commit);

#endif							/* FILEOPS_H */
