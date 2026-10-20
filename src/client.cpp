#include "internal.hpp"
#include <atomic>
#include <thread>
#include <mutex>
namespace {
struct CallbackFailure : std::runtime_error {
    CallbackFailure() : std::runtime_error("application callback threw an exception") {}
};
thread_local fq_client *callback_client = 0;
} // namespace
struct fq_client {
    fq_callbacks callbacks = {};
    fqimpl::Config config;
    std::map<uint32_t, std::unique_ptr<fqimpl::Adapter>> adapters;
    std::unique_ptr<fqimpl::Receiver> receiver;
    std::thread worker;
    std::mutex gate, error_mutex;
    std::string error;
    bool opened = false;
    std::atomic<bool> running{false}, stop{false};
    std::atomic<int> worker_result{0};
    std::atomic<uint64_t> datagrams{0}, bytes{0}, events{0}, parse_errors{0}, drops{0}, truncated{0};
    void set_error(const std::string &value) {
        std::lock_guard<std::mutex> lock(error_mutex);
        error = value;
    }
    void emit(const fq_event &event) {
        ++events;
        if (!callbacks.on_event)
            return;
        fq_client *previous = callback_client;
        callback_client = this;
        try {
            callbacks.on_event(callbacks.user, &event);
            callback_client = previous;
        } catch (...) {
            callback_client = previous;
            stop = true;
            throw CallbackFailure();
        }
    }
    int ingest(const fq_datagram &d) {
        auto it = adapters.find(d.feed_id);
        if (it == adapters.end()) {
            set_error("unknown feed_id");
            return FQ_EINVAL;
        }
        ++datagrams;
        bytes += d.size;
        try {
            it->second->feed(d);
            return FQ_OK;
        } catch (const CallbackFailure &) {
            set_error("application callback threw an exception");
            return FQ_ECALLBACK;
        } catch (const std::exception &ex) {
            ++parse_errors;
            set_error(ex.what());
            try {
                it->second->invalidate(ex.what());
            } catch (...) {
                stop = true;
                return FQ_ECALLBACK;
            }
            return FQ_EPARSE;
        }
    }
    void received(const fqimpl::Received &r, uint32_t flags) {
        fq_datagram d = {sizeof(d), FQ_ABI_VERSION, r.feed, r.role, r.data, r.size, r.time, flags, r.line};
        int result = ingest(d);
        if (result == FQ_ECALLBACK)
            throw CallbackFailure();
    }
    void poll_once(int timeout) {
        if (!receiver)
            receiver.reset(new fqimpl::Receiver(config));
        uint64_t lost = 0, cut = 0, invalidated_loss = 0, invalidated_cut = 0;
        auto invalidate_loss = [&] {
            if (lost != invalidated_loss || cut != invalidated_cut) {
                for (auto &a : adapters)
                    a.second->invalidate("transport dropped or truncated datagrams");
                invalidated_loss = lost;
                invalidated_cut = cut;
            }
        };
        receiver->poll(
            timeout,
            [&](const fqimpl::Received &r) {
                invalidate_loss();
                received(r, 0);
            },
            lost, cut);
        drops += lost;
        truncated += cut;
        invalidate_loss();
        for (auto &a : adapters)
            a.second->tick(fqimpl::now_ns());
    }
};
namespace {
bool valid_datagram(const fq_datagram *d) {
    return d && d->struct_size >= sizeof(*d) && d->abi_version == FQ_ABI_VERSION && d->data && d->size &&
           d->size <= 65507 && d->role <= FQ_DR && d->line <= 1;
}
struct Guard {
    fq_client *c;
    std::unique_lock<std::mutex> lock;
    explicit Guard(fq_client *p) : c(p), lock(p->gate, std::try_to_lock) {}
    bool usable() {
        if (!lock.owns_lock() || c->running || c->worker.joinable()) {
            c->set_error("client is busy; stop and join before this operation");
            return false;
        }
        return true;
    }
};
} // namespace
extern "C" {
const char *fq_version(void) {
    return "0.1.0";
}
int fq_create(const fq_callbacks *cb, fq_client **out) {
    if (!out)
        return FQ_EINVAL;
    *out = 0;
    if (cb && (cb->struct_size < sizeof(*cb) || cb->abi_version != FQ_ABI_VERSION))
        return FQ_EINVAL;
    try {
        std::unique_ptr<fq_client> c(new fq_client);
        if (cb)
            c->callbacks = *cb;
        *out = c.release();
        return FQ_OK;
    } catch (...) {
        return FQ_ECAPACITY;
    }
}
void fq_destroy(fq_client *c) {
    if (!c)
        return;
    if (callback_client == c) {
        c->set_error("destroy is forbidden inside callbacks; destroy after callback returns");
        return;
    }
    fq_request_stop(c);
    fq_join(c);
    delete c;
}
int fq_open(fq_client *c, const char *path) {
    if (!c || !path)
        return FQ_EINVAL;
    try {
        Guard guard(c);
        if (!guard.usable())
            return FQ_ESTATE;
        fqimpl::Config config = fqimpl::load_config(path);
        std::map<uint32_t, std::unique_ptr<fqimpl::Adapter>> adapters;
        for (const auto &f : config.feeds)
            adapters.emplace(f.id, fqimpl::make_adapter(f, [c](const fq_event &e) { c->emit(e); }));
        c->receiver.reset();
        c->adapters = std::move(adapters);
        c->config = std::move(config);
        c->opened = true;
        c->stop = false;
        c->worker_result = 0;
        c->set_error("");
        return FQ_OK;
    } catch (const std::exception &ex) {
        c->set_error(ex.what());
        return FQ_EINVAL;
    } catch (...) {
        c->set_error("configuration failed");
        return FQ_EINVAL;
    }
}
int fq_start(fq_client *c) {
    if (!c)
        return FQ_EINVAL;
    try {
        Guard guard(c);
        if (!guard.usable() || !c->opened)
            return FQ_ESTATE;
        c->receiver.reset(new fqimpl::Receiver(c->config));
        c->stop = false;
        c->running = true;
        c->worker_result = 0;
        c->worker = std::thread([c] {
            try {
                while (!c->stop)
                    c->poll_once(2);
            } catch (const CallbackFailure &ex) {
                c->set_error(ex.what());
                c->worker_result = FQ_ECALLBACK;
            } catch (const std::exception &ex) {
                c->set_error(ex.what());
                c->worker_result = FQ_EIO;
            } catch (...) {
                c->set_error("unknown worker failure");
                c->worker_result = FQ_EIO;
            }
            c->running = false;
        });
        return FQ_OK;
    } catch (const std::exception &ex) {
        c->running = false;
        c->receiver.reset();
        c->set_error(ex.what());
        return FQ_EIO;
    } catch (...) {
        c->running = false;
        c->receiver.reset();
        c->set_error("start failed");
        return FQ_EIO;
    }
}
void fq_request_stop(fq_client *c) {
    if (c)
        c->stop = true;
}
int fq_join(fq_client *c) {
    if (!c)
        return FQ_EINVAL;
    if (callback_client == c) {
        c->set_error("join is forbidden inside callbacks");
        return FQ_ESTATE;
    }
    try {
        std::unique_lock<std::mutex> lock(c->gate, std::try_to_lock);
        if (!lock.owns_lock())
            return FQ_ESTATE;
        if (c->worker.joinable())
            c->worker.join();
        c->receiver.reset();
        return c->worker_result;
    } catch (...) {
        c->set_error("join failed");
        return FQ_ESTATE;
    }
}
int fq_poll(fq_client *c, int timeout) {
    if (!c || timeout < 0 || timeout > 1000)
        return FQ_EINVAL;
    try {
        Guard guard(c);
        if (!guard.usable() || !c->opened)
            return FQ_ESTATE;
        if (c->stop)
            return FQ_OK;
        c->poll_once(timeout);
        return FQ_OK;
    } catch (const CallbackFailure &ex) {
        c->set_error(ex.what());
        return FQ_ECALLBACK;
    } catch (const std::exception &ex) {
        c->set_error(ex.what());
        return FQ_EIO;
    } catch (...) {
        c->set_error("poll failed");
        return FQ_EIO;
    }
}
int fq_feed(fq_client *c, const fq_datagram *d) {
    if (!c || !valid_datagram(d))
        return FQ_EINVAL;
    try {
        Guard guard(c);
        if (!guard.usable() || !c->opened)
            return FQ_ESTATE;
        if (c->stop)
            return FQ_ESTATE;
        return c->ingest(*d);
    } catch (...) {
        c->set_error("feed failed");
        return FQ_EPARSE;
    }
}
int fq_replay_pcap(fq_client *c, const char *path) {
    if (!c || !path)
        return FQ_EINVAL;
    try {
        Guard guard(c);
        if (!guard.usable() || !c->opened)
            return FQ_ESTATE;
        c->stop = false;
        int result = FQ_OK;
        fqimpl::replay_pcap(
            c->config, path,
            [&](const fqimpl::Received &r) {
                fq_datagram d = {sizeof(d), FQ_ABI_VERSION, r.feed,      r.role, r.data,
                                 r.size,    r.time,         FQ_REPLAYED, r.line};
                int rc = c->ingest(d);
                if (rc < 0) {
                    result = rc;
                    c->stop = true;
                }
            },
            [&] { return c->stop.load(); });
        return result;
    } catch (const std::exception &ex) {
        c->set_error(ex.what());
        return FQ_EPARSE;
    } catch (...) {
        c->set_error("PCAP replay failed");
        return FQ_EPARSE;
    }
}
int fq_get_statistics(fq_client *c, fq_statistics *out) {
    if (!c || !out || out->struct_size < sizeof(*out) || out->abi_version != FQ_ABI_VERSION)
        return FQ_EINVAL;
    out->datagrams = c->datagrams;
    out->bytes = c->bytes;
    out->events = c->events;
    out->parse_errors = c->parse_errors;
    out->socket_drops = c->drops;
    out->truncated_datagrams = c->truncated;
    return FQ_OK;
}
uint64_t fq_capabilities(fq_client *c, uint32_t id) {
    if (!c)
        return 0;
    try {
        std::unique_lock<std::mutex> lock(c->gate, std::try_to_lock);
        if (!lock.owns_lock())
            return 0;
        auto it = c->adapters.find(id);
        return it == c->adapters.end() ? 0 : it->second->capabilities();
    } catch (...) {
        return 0;
    }
}
size_t fq_last_error(fq_client *c, char *buffer, size_t capacity) {
    if (!c) {
        if (buffer && capacity)
            buffer[0] = 0;
        return 1;
    }
    try {
        std::lock_guard<std::mutex> lock(c->error_mutex);
        if (buffer && capacity)
            fqimpl::copy_text(buffer, capacity, c->error.data(), c->error.size());
        return c->error.size() + 1;
    } catch (...) {
        if (buffer && capacity)
            buffer[0] = 0;
        return 1;
    }
}
int fq_check_config(const char *path, char *error, size_t capacity) {
    if (!path)
        return FQ_EINVAL;
    try {
        auto c = fqimpl::load_config(path);
        for (const auto &f : c.feeds) {
            auto a = fqimpl::make_adapter(f, [](const fq_event &) {});
        }
        if (error && capacity)
            error[0] = 0;
        return FQ_OK;
    } catch (const std::exception &ex) {
        if (error && capacity)
            fqimpl::copy_text(error, capacity, ex.what(), std::strlen(ex.what()));
        return FQ_EINVAL;
    } catch (...) {
        if (error && capacity)
            fqimpl::copy_text(error, capacity, "configuration failed", 20);
        return FQ_EINVAL;
    }
}
}
