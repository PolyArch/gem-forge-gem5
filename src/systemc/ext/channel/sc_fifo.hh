/*
 * Copyright 2018 Google, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Gabe Black
 */

#ifndef __SYSTEMC_EXT_CHANNEL_SC_FIFO_HH__
#define __SYSTEMC_EXT_CHANNEL_SC_FIFO_HH__

#include <list>

#include "../core/sc_module.hh" // for sc_gen_unique_name
#include "../core/sc_prim.hh"
#include "sc_fifo_in_if.hh"
#include "sc_fifo_out_if.hh"
#include "warn_unimpl.hh"

namespace sc_core
{

class sc_port_base;
class sc_event;

template <class T>
class sc_fifo : public sc_fifo_in_if<T>,
                public sc_fifo_out_if<T>,
                public sc_prim_channel
{
  public:
    explicit sc_fifo(int size=16) :
            sc_fifo_in_if<T>(), sc_fifo_out_if<T>(),
            sc_prim_channel(sc_gen_unique_name("fifo")),
            _size(size), _readsHappened(false)
    {}
    explicit sc_fifo(const char *name, int size=16) :
            sc_fifo_in_if<T>(), sc_fifo_out_if<T>(),
            sc_prim_channel(name), _size(size), _readsHappened(false)
    {}
    virtual ~sc_fifo() {}

    virtual void
    register_port(sc_port_base &, const char *)
    {
        sc_channel_warn_unimpl(__PRETTY_FUNCTION__);
    }

    virtual void
    read(T &t)
    {
        while (num_available() == 0)
            sc_core::wait(_dataWriteEvent);
        _readsHappened = true;
        t = _entries.front();
        _entries.pop_front();
        request_update();
    }
    virtual T
    read()
    {
        T t;
        read(t);
        return t;
    }
    virtual bool
    nb_read(T &t)
    {
        if (num_available()) {
            read(t);
            return true;
        } else {
            return false;
        }
    }
    operator T() { return read(); }

    virtual void
    write(const T &t)
    {
        while (num_free() == 0)
            sc_core::wait(_dataReadEvent);
        _pending.emplace_back(t);
        request_update();
    }
    virtual bool
    nb_write(const T &t)
    {
        if (num_free()) {
            write(t);
            return true;
        } else {
            return false;
        }
    }
    sc_fifo<T> &
    operator = (const T &t)
    {
        write(t);
        return *this;
    }

    virtual const sc_event &
    data_written_event() const
    {
        return _dataWriteEvent;
    }
    virtual const sc_event &
    data_read_event() const
    {
        return _dataReadEvent;
    }

    virtual int num_available() const { return _entries.size(); }
    virtual int
    num_free() const
    {
        return _size - _entries.size() - _pending.size();
    }

    virtual void
    print(std::ostream &os=std::cout) const
    {
        for (typename ::std::list<T>::iterator pos = _pending.begin();
                pos != _pending.end(); pos++) {
            os << *pos << ::std::endl;
        }
        for (typename ::std::list<T>::iterator pos = _entries.begin();
                pos != _entries.end(); pos++) {
            os << *pos << ::std::endl;
        }
    }
    virtual void
    dump(std::ostream &os=std::cout) const
    {
        os << "name = " << name() << std::endl;
        int idx = 0;
        for (typename ::std::list<T>::iterator pos = _pending.begin();
                pos != _pending.end(); pos++) {
            os << "value[" << idx++ << "] = " << *pos << ::std::endl;
        }
        for (typename ::std::list<T>::iterator pos = _entries.begin();
                pos != _entries.end(); pos++) {
            os << "value[" << idx++ << "] = " << *pos << ::std::endl;
        }
    }
    virtual const char *kind() const { return "sc_fifo"; }

  protected:
    virtual void
    update()
    {
        if (!_pending.empty()) {
            _dataWriteEvent.notify(SC_ZERO_TIME);
            _entries.insert(_entries.end(), _pending.begin(), _pending.end());
            _pending.clear();
        }
        if (_readsHappened) {
            _readsHappened = false;
            _dataReadEvent.notify(SC_ZERO_TIME);
        }
    }

  private:
    // Disabled
    sc_fifo(const sc_fifo<T> &) :
            sc_fifo_in_if<T>(), sc_fifo_in_if<T>(), sc_prim_channel()
    {}
    sc_fifo &operator = (const sc_fifo<T> &) { return *this; }

    sc_event _dataReadEvent;
    sc_event _dataWriteEvent;

    int _size;
    mutable std::list<T> _entries;
    mutable std::list<T> _pending;
    bool _readsHappened;
};

template <class T>
inline std::ostream &
operator << (std::ostream &os, const sc_fifo<T> &f)
{
    f.print(os);
    return os;
}

} // namespace sc_core

#endif  //__SYSTEMC_EXT_CHANNEL_SC_FIFO_HH__