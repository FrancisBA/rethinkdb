
#ifndef __LOG_SERIALIZER_HPP__
#define __LOG_SERIALIZER_HPP__

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <map>

#include "serializer/types.hpp"
#include "config/cmd_args.hpp"
#include "config/alloc.hpp"
#include "utils.hpp"

#include "log_serializer_callbacks.hpp"
#include "metablock/metablock_manager.hpp"
#include "extents/extent_manager.hpp"
#include "lba/lba_list.hpp"
#include "data_block_manager.hpp"

/**
 * This is the log-structured serializer, the holiest of holies of
 * RethinkDB. Please treat it with courtesy, professionalism, and
 * respect that it deserves.
 */

/*
TODO: Consider the following situation:
1. Block A is stored at address X.
2. Client issues a read for block A at address X. It gets hung up in the OS somewhere.
3. Client issues a write for block A. Address Y is chosen. The write completes quickly.
4. The garbage collector recognizes that block A is no longer at address X, so it releases the
    extent containing address X.
5. Client issues a write for block B. Address X, which is now free, is chosen. The write completes
    quickly.
6. The read from step #2 finally gets performed, but because block B is now at address X, it gets
    the contents of block B instead of block A.
*/

/*
TODO: Consider the following situation:
1. The data block manager's current extent is X. From X to X+Y have been filled.
2. The data block manager fills the range from X+Y to X+Y+Z.
3. The server crashes before the metablock has been written
4. On restart, the server only remembers that there is data from X to X+Y.
5. The data block manager re-fills the range from X+Y to X+Y+Z.
6. The disk experiences fragmentation, possibly causing a slowdown.
*/

typedef lba_list_t lba_index_t;

struct log_serializer_metablock_t {
    extent_manager_t::metablock_mixin_t extent_manager_part;
    lba_index_t::metablock_mixin_t lba_index_part;
    data_block_manager_t::metablock_mixin_t data_block_manager_part;
};

typedef metablock_manager_t<log_serializer_metablock_t> mb_manager_t;

// Used internally
struct ls_block_writer_t;
struct ls_write_fsm_t;
struct ls_start_fsm_t;

struct log_serializer_t :
    public home_cpu_mixin_t,
    private data_block_manager_t::shutdown_callback_t,
    private lba_list_t::shutdown_callback_t
{
    friend class ls_block_writer_t;
    friend class ls_write_fsm_t;
    friend class ls_start_fsm_t;
    
public:
    log_serializer_t(cmd_config_t *cmd_config, char *db_path, size_t _block_size);
    virtual ~log_serializer_t();

public:
    typedef log_serializer_metablock_t metablock_t;
    
public:
    /* data to be serialized with each data block */
    typedef data_block_manager_t::buf_data_t buf_data_t;

public:
    /* start() must be called before the serializer can be used. It will return 'true' if it is
    ready immediately; otherwise, it will return 'false' and then call the given callback later. */
    struct ready_callback_t {
        virtual void on_serializer_ready(log_serializer_t *) = 0;
    };
    bool start(ready_callback_t *ready_cb);

public:
    /* do_read() reads the block with the given ID. It returns 'true' if the read completes
    immediately; otherwise, it will return 'false' and call the given callback later. */
    struct read_callback_t : private iocallback_t {
        friend class log_serializer_t;
        virtual void on_serializer_read() = 0;
    private:
        void on_io_complete(event_t *unused) {
            on_serializer_read();
        }
    };
    bool do_read(ser_block_id_t block_id, void *buf, read_callback_t *callback);

public:
    /* do_write() updates or deletes a group of bufs.
    
    Each write_t passed to do_write() identifies an update or deletion. If 'buf' is NULL, then it
    represents a deletion. If 'buf' is non-NULL, then it identifies an update, and the given
    callback will be called as soon as the data has been copied out of 'buf'. If the entire
    transaction completes immediately, it will return 'true'; otherwise, it will return 'false' and
    call the given callback at a later date.
    
    'writes' can be freed as soon as do_write() returns. */

    typedef _write_txn_callback_t write_txn_callback_t;

    typedef _write_block_callback_t write_block_callback_t;
    
    struct write_t {
        ser_block_id_t block_id;
        void *buf;   /* If NULL, a deletion */
        write_block_callback_t *callback;
    };
    bool do_write(write_t *writes, int num_writes, write_txn_callback_t *callback);
    
public:
    /* max_block_id() and block_in_use() are used by the buffer cache to reconstruct
    the free list of unused block IDs. */
    
    /* Returns a block ID such that every existing block has an ID less than
    that ID. Note that block_in_use(max_block_id() - 1) is not guaranteed. */
    ser_block_id_t max_block_id();
    
    /* Checks whether a given block ID exists */
    bool block_in_use(ser_block_id_t id);

public:
    /* shutdown() should be called when you are done with the serializer.
    
    If the shutdown is done immediately, shutdown() will return 'true'. Otherwise, it will return
    'false' and then call the given callback when the shutdown is done. */
    struct shutdown_callback_t {
        virtual void on_serializer_shutdown(log_serializer_t *) = 0;
    };
    bool shutdown(shutdown_callback_t *cb);

private:
    bool next_shutdown_step();
    shutdown_callback_t *shutdown_callback;
    
    enum shutdown_state_t {
        shutdown_begin,
        shutdown_waiting_on_serializer,
        shutdown_waiting_on_datablock_manager,
        shutdown_waiting_on_lba
    } shutdown_state;
    bool shutdown_in_one_shot;

    virtual void on_datablock_manager_shutdown();
    virtual void on_lba_shutdown();

public:
    size_t block_size;

private:
    void prepare_metablock(metablock_t *mb_buffer);

    void consider_start_gc();

private:
    enum state_t {
        state_unstarted,
        state_starting_up,
        state_ready,
        state_shutting_down,
        state_shut_down,
    } state;

    char db_path[MAX_DB_FILE_NAME];
    direct_file_t *dbfile;
    
    extent_manager_t extent_manager;
    mb_manager_t metablock_manager;
    data_block_manager_t data_block_manager;
    lba_index_t lba_index;
    
    /* The ls_write_fsm_ts organize themselves into a list so that they can be sure to
    write their metablocks in the correct order. last_write points to the most recent
    transaction that started but did not finish; new ls_write_fsm_ts use it to find the
    end of the list so they can append themselves to it. */
    ls_write_fsm_t *last_write;
    
    int active_write_count;
    
    /* Keeps track of buffers that are currently being written, so that if we get a read
    for a block ID that we are currently writing but is not on disk yet, we can return
    the most current version. */
    typedef std::map<
        ser_block_id_t, ls_block_writer_t*,
        std::less<ser_block_id_t>,
        gnew_alloc<std::pair<ser_block_id_t, ls_block_writer_t*> >
        > block_writer_map_t;
    block_writer_map_t block_writer_map;
#ifndef NDEBUG
public:
    bool is_extent_referenced(off64_t offset) {
        return lba_index.is_extent_referenced(offset);
    }

    int extent_refcount(off64_t offset) {
        return lba_index.extent_refcount(offset);
    }
#endif

#ifndef NDEBUG
    metablock_t debug_mb_buffer;
#endif
};

#endif /* __LOG_SERIALIZER_HPP__ */
