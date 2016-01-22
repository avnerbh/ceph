// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_LIBRBD_AIOREQUEST_H
#define CEPH_LIBRBD_AIOREQUEST_H

#include "include/int_types.h"

#include <map>

#include "common/snap_types.h"
#include "include/buffer.h"
#include "include/Context.h"
#include "include/rados/librados.hpp"
#include "include/rbd/librbd.hpp"
#include "librbd/ObjectMap.h"

namespace librbd {

  struct AioCompletion;
  struct ImageCtx;
  class CopyupRequest;

  /**
   * This class represents an I/O operation to a single RBD data object.
   * Its subclasses encapsulate logic for dealing with special cases
   * for I/O due to layering.
   */
  class AioObjectRequest
  {
  public:
    AioObjectRequest(ImageCtx *ictx, const std::string &oid, uint64_t objectno,
                     librados::snap_t snap_id, Context *completion,
                     bool hide_enoent);
    virtual ~AioObjectRequest() {}

    virtual void add_copyup_ops(librados::ObjectWriteOperation *wr) {};

    void complete(int r);

    virtual bool should_complete(int r) = 0;
    virtual void send() = 0;

    bool has_parent() const {
      return !m_parent_extents.empty();
    }

  protected:
    bool compute_parent_extents();

    ImageCtx *m_ictx;
    std::string m_oid;
    uint64_t m_object_no;
    librados::snap_t m_snap_id;
    Context *m_completion;
    std::vector<std::pair<uint64_t,uint64_t> > m_parent_extents;
    bool m_hide_enoent;
  };

  class AioObjectRead : public AioObjectRequest {
  public:
    AioObjectRead(ImageCtx *ictx, const std::string &oid,
	          uint64_t objectno, uint64_t offset, uint64_t len,
	          vector<pair<uint64_t,uint64_t> >& be,
	          librados::snap_t snap_id, bool sparse,
	          Context *completion, int op_flags);

    virtual bool should_complete(int r);
    virtual void send();
    void guard_read();

    ceph::bufferlist &data() {
      return m_read_data;
    }

    std::map<uint64_t, uint64_t> m_ext_map;

    friend class C_AioRead;

  private:
    uint64_t m_object_off;
    uint64_t m_object_len;
    vector<pair<uint64_t,uint64_t> > m_buffer_extents;
    bool m_tried_parent;
    bool m_sparse;
    int m_op_flags;
    ceph::bufferlist m_read_data;
    AioCompletion *m_parent_completion;

    /**
     * Reads go through the following state machine to deal with
     * layering:
     *
     *                          need copyup
     * LIBRBD_AIO_READ_GUARD ---------------> LIBRBD_AIO_READ_COPYUP
     *           |                                       |
     *           v                                       |
     *         done <------------------------------------/
     *           ^
     *           |
     * LIBRBD_AIO_READ_FLAT
     *
     * Reads start in LIBRBD_AIO_READ_GUARD or _FLAT, depending on
     * whether there is a parent or not.
     */
    enum read_state_d {
      LIBRBD_AIO_READ_GUARD,
      LIBRBD_AIO_READ_COPYUP,
      LIBRBD_AIO_READ_FLAT
    };

    read_state_d m_state;

    void send_copyup();

    void read_from_parent(const vector<pair<uint64_t,uint64_t> >& image_extents);
  };

  class AbstractAioObjectWrite : public AioObjectRequest {
  public:
    typedef std::vector<std::pair<uint64_t, uint64_t> > Extents;

    AbstractAioObjectWrite(ImageCtx *ictx, const std::string &oid,
                           uint64_t object_no, uint64_t object_off,
                           uint64_t len, const ::SnapContext &snapc,
		           Context *completion, bool hide_enoent)
      : AbstractAioObjectWrite(ictx, oid, object_no,
                               std::move(Extents{{object_off, len}}),
                               snapc, completion, hide_enoent) {
    }
    AbstractAioObjectWrite(ImageCtx *ictx, const std::string &oid,
                           uint64_t object_no, Extents &&object_extents,
                           const ::SnapContext &snapc, Context *completion,
                           bool hide_enoent);

    virtual void add_copyup_ops(librados::ObjectWriteOperation *wr)
    {
      add_write_ops(wr);
    }

    virtual bool should_complete(int r);
    virtual void send();

    /**
     * Writes go through the following state machine to deal with
     * layering and the object map:
     *
     * <start>
     *  .  |
     *  .  |
     *  .  \---> LIBRBD_AIO_WRITE_PRE
     *  .           |         |
     *  . . . . . . | . . . . | . . . . . . . . . . .
     *      .       |   -or-  |                     .
     *      .       |         |                     v
     *      .       |         \----------------> LIBRBD_AIO_WRITE_FLAT . . .
     *      .       |                                               |      .
     *      v       v         need copyup                           |      .
     * LIBRBD_AIO_WRITE_GUARD -----------> LIBRBD_AIO_WRITE_COPYUP  |      .
     *  .       |                               |        .          |      .
     *  .       |                               |        .          |      .
     *  .       |                         /-----/        .          |      .
     *  .       |                         |              .          |      .
     *  .       \-------------------\     |     /-------------------/      .
     *  .                           |     |     |        .                 .
     *  .                           v     v     v        .                 .
     *  .                       LIBRBD_AIO_WRITE_POST    .                 .
     *  .                               |                .                 .
     *  .                               |  . . . . . . . .                 .
     *  .                               |  .                               .
     *  .                               v  v                               .
     *  . . . . . . . . . . . . . . > <finish> < . . . . . . . . . . . . . .
     *
     * The _PRE/_POST states are skipped if the object map is disabled.
     * The write starts in _WRITE_GUARD or _FLAT depending on whether or not
     * there is a parent overlap.
     */
  protected:
    enum write_state_d {
      LIBRBD_AIO_WRITE_GUARD,
      LIBRBD_AIO_WRITE_COPYUP,
      LIBRBD_AIO_WRITE_FLAT,
      LIBRBD_AIO_WRITE_PRE,
      LIBRBD_AIO_WRITE_POST,
      LIBRBD_AIO_WRITE_ERROR
    };

    Extents m_object_extents;
    write_state_d m_state;
    librados::ObjectWriteOperation m_write;
    uint64_t m_snap_seq;
    std::vector<librados::snap_t> m_snaps;
    bool m_object_exist;

    virtual void add_write_ops(librados::ObjectWriteOperation *wr) = 0;
    virtual const char* get_write_type() const = 0;
    virtual void guard_write();
    virtual void pre_object_map_update(uint8_t *new_state) = 0;
    virtual bool post_object_map_update() {
      return false;
    }
    virtual void send_write();
    virtual void send_write_op(bool write_guard);
    virtual void handle_write_guard();

  private:
    void send_pre();
    bool send_post();
    void send_copyup();
  };

  class AioObjectWrite : public AbstractAioObjectWrite {
  public:
    typedef std::vector<std::pair<uint64_t, bufferlist> > OffsetBuffers;

    AioObjectWrite(ImageCtx *ictx, const std::string &oid, uint64_t object_no,
                   OffsetBuffers &&offset_buffers, const ::SnapContext &snapc,
                   Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no,
                               std::move(compute_object_extents(offset_buffers)),
                               snapc, completion, false),
        m_offset_buffers(std::move(offset_buffers)), m_op_flags(0) {
    }

    AioObjectWrite(ImageCtx *ictx, const std::string &oid, uint64_t object_no,
                   uint64_t object_off, const ceph::bufferlist &data,
                   const ::SnapContext &snapc, Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no, object_off, data.length(),
                               snapc, completion, false),
	m_offset_buffers({{object_off, data}}), m_op_flags(0) {
    }

    void set_op_flags(int op_flags) {
      m_op_flags = op_flags;
    }

  protected:
    virtual void add_write_ops(librados::ObjectWriteOperation *wr);

    virtual const char* get_write_type() const {
      return "write";
    }

    virtual void pre_object_map_update(uint8_t *new_state) {
      *new_state = OBJECT_EXISTS;
    }
    virtual void send_write();

  private:
    OffsetBuffers m_offset_buffers;
    int m_op_flags;

    static Extents compute_object_extents(const OffsetBuffers &offset_buffers) {
      Extents extents;
      extents.reserve(offset_buffers.size());
      for (auto &pair : offset_buffers) {
        extents.push_back(std::make_pair(pair.first, pair.second.length()));
      }
      return extents;
    }
  };

  class AioObjectRemove : public AbstractAioObjectWrite {
  public:
    AioObjectRemove(ImageCtx *ictx, const std::string &oid, uint64_t object_no,
	            const ::SnapContext &snapc, Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no, 0, 0, snapc, completion,
                               true),
        m_object_state(OBJECT_NONEXISTENT) {
    }

  protected:
    virtual void add_write_ops(librados::ObjectWriteOperation *wr) {
      if (has_parent()) {
	wr->truncate(0);
      } else {
	wr->remove();
      }
    }

    virtual const char* get_write_type() const {
      if (has_parent()) {
        return "remove (trunc)";
      }
      return "remove";
    }
    virtual void pre_object_map_update(uint8_t *new_state) {
      if (has_parent()) {
	m_object_state = OBJECT_EXISTS;
      } else {
	m_object_state = OBJECT_PENDING;
      }
      *new_state = m_object_state;
    }

    virtual bool post_object_map_update() {
      if (m_object_state == OBJECT_EXISTS) {
	return false;
      }
      return true;
    }

    virtual void guard_write();
    virtual void send_write();

  private:
    uint8_t m_object_state;
  };

  class AioObjectTrim : public AbstractAioObjectWrite {
  public:
    AioObjectTrim(ImageCtx *ictx, const std::string &oid, uint64_t object_no,
                  const ::SnapContext &snapc, Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no, 0, 0, snapc, completion,
                               true) {
    }

  protected:
    virtual void add_write_ops(librados::ObjectWriteOperation *wr) {
      wr->remove();
    }

    virtual const char* get_write_type() const {
      return "remove (trim)";
    }

    virtual void pre_object_map_update(uint8_t *new_state) {
      *new_state = OBJECT_PENDING;
    }

    virtual bool post_object_map_update() {
      return true;
    }
  };

  class AioObjectTruncate : public AbstractAioObjectWrite {
  public:
    AioObjectTruncate(ImageCtx *ictx, const std::string &oid,
                      uint64_t object_no, uint64_t object_off,
                      const ::SnapContext &snapc, Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no, object_off, 0, snapc,
                               completion, true) {
    }

  protected:
    virtual void add_write_ops(librados::ObjectWriteOperation *wr) {
      wr->truncate(m_object_extents.front().first);
    }

    virtual const char* get_write_type() const {
      return "truncate";
    }

    virtual void pre_object_map_update(uint8_t *new_state) {
      *new_state = OBJECT_EXISTS;
    }
  };

  class AioObjectZero : public AbstractAioObjectWrite {
  public:
    AioObjectZero(ImageCtx *ictx, const std::string &oid, uint64_t object_no,
                  uint64_t object_off, uint64_t object_len,
                  const ::SnapContext &snapc, Context *completion)
      : AbstractAioObjectWrite(ictx, oid, object_no, object_off, object_len,
                               snapc, completion, true) {
    }

  protected:
    virtual void add_write_ops(librados::ObjectWriteOperation *wr) {
      const std::pair<uint64_t, uint64_t> &extent(m_object_extents.front());
      wr->zero(extent.first, extent.second);
    }

    virtual const char* get_write_type() const {
      return "zero";
    }

    virtual void pre_object_map_update(uint8_t *new_state) {
      *new_state = OBJECT_EXISTS;
    }
  };

}

#endif
