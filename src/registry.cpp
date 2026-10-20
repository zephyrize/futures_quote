#include "internal.hpp"
#include <mutex>
#include <exception>
namespace fqimpl {
namespace {
std::mutex registry_mutex;
std::map<std::string, fq_adapter_v1> registry;
class Extension : public Adapter {
    fq_adapter_v1 api_;
    void *state_;
    std::exception_ptr failure_;
    static void forward(void *context, const fq_event *e) {
        Extension *self = static_cast<Extension *>(context);
        if (self->failure_)
            return;
        try {
            if (!e || e->abi_version != FQ_ABI_VERSION || e->struct_size < sizeof(fq_event))
                throw ParseError("invalid extension event");
            self->emit_(*e);
        } catch (...) {
            self->failure_ = std::current_exception();
        }
    }

  public:
    Extension(const FeedConfig &c, Emit emit, fq_adapter_v1 api) : Adapter(c, emit), api_(api), state_(0) {
        fq_adapter_options o = {sizeof(o),
                                FQ_ABI_VERSION,
                                c.id,
                                c.channel,
                                c.depth,
                                c.max_instruments,
                                c.cache_bytes,
                                c.revision.c_str(),
                                c.adapter_config.c_str()};
        state_ = api_.create(&o, &forward, this);
        if (!state_)
            throw std::runtime_error("extension adapter creation failed");
    }
    ~Extension() {
        if (state_)
            api_.destroy(state_);
    }
    void feed(const fq_datagram &d) override {
        prepare(d, FQ_CUSTOM);
        failure_ = nullptr;
        int r = api_.on_datagram(state_, &d);
        if (failure_)
            std::rethrow_exception(failure_);
        if (r < 0)
            throw ParseError("extension adapter rejected datagram");
    }
    void invalidate(const char *reason) override { status(FQ_EPARSE, reason); }
    uint64_t capabilities() const override { return api_.capabilities; }
};
} // namespace
std::unique_ptr<Adapter> make_adapter(const FeedConfig &c, Emit e) {
    if (c.protocol == "cme_mdp3")
        return make_cme(c, e);
    if (c.protocol == "lmesource_v4")
        return make_lmesource_adapter(c, e);
    fq_adapter_v1 api;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        auto it = registry.find(c.protocol);
        if (it == registry.end())
            throw std::runtime_error("unregistered protocol: " + c.protocol);
        api = it->second;
    }
    return std::unique_ptr<Adapter>(new Extension(c, e, api));
}
int register_adapter(const fq_adapter_v1 *a) {
    if (!a || a->abi_version != FQ_ABI_VERSION || a->struct_size < sizeof(*a) || !a->protocol ||
        !*a->protocol || !a->create || !a->on_datagram || !a->destroy)
        return FQ_EINVAL;
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::string name = a->protocol;
    if (name == "cme_mdp3" || name == "lmesource_v4" || registry.count(name))
        return FQ_ESTATE;
    registry.emplace(name, *a);
    return FQ_OK;
}
} // namespace fqimpl
extern "C" int fq_register_adapter(const fq_adapter_v1 *a) {
    try {
        return fqimpl::register_adapter(a);
    } catch (...) {
        return FQ_EINVAL;
    }
}
