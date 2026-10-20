#include "internal.hpp"
#include <yaml-cpp/yaml.h>
#include <arpa/inet.h>
#include <fstream>
namespace fqimpl {
namespace {
void keys(const YAML::Node &n, std::initializer_list<const char *> allowed) {
    if (!n.IsMap())
        throw std::runtime_error("expected a YAML map");
    std::set<std::string> seen;
    for (auto i : n) {
        std::string k = i.first.as<std::string>();
        if (!seen.insert(k).second)
            throw std::runtime_error("duplicate YAML key: " + k);
        bool found = false;
        for (auto a : allowed)
            if (k == a)
                found = true;
        if (!found)
            throw std::runtime_error("unknown configuration key: " + k);
    }
}
template <class T> T get(const YAML::Node &n, const char *k, T def) {
    return n[k] ? n[k].as<T>() : def;
}
uint64_t bounded(const YAML::Node &n, const char *k, uint64_t def, uint64_t lo, uint64_t hi) {
    uint64_t v = get<uint64_t>(n, k, def);
    if (v < lo || v > hi)
        throw std::runtime_error(std::string("out of range: ") + k);
    return v;
}
void ip(const std::string &s, bool multicast) {
    in_addr addr = {};
    if (inet_pton(AF_INET, s.c_str(), &addr) != 1)
        throw std::runtime_error("invalid IPv4: " + s);
    bool multi = IN_MULTICAST(ntohl(addr.s_addr));
    if (multicast != multi)
        throw std::runtime_error("invalid multicast/unicast address: " + s);
}
template <class T> void values(const YAML::Node &n, const char *key, std::set<T> &out) {
    if (!n[key])
        return;
    if (!n[key].IsSequence())
        throw std::runtime_error(std::string(key) + " must be a list");
    for (auto v : n[key])
        out.insert(v.as<T>());
}
} // namespace
Config load_config(const std::string &path) {
    YAML::Node root = YAML::LoadFile(path);
    keys(root, {"config_version", "network", "feeds"});
    if (get<int>(root, "config_version", 0) != 1)
        throw std::runtime_error("unsupported config_version");
    Config c;
    auto network = root["network"];
    if (network) {
        keys(network, {"interface_ip", "receive_buffer_bytes", "receive_batch_max"});
        c.interface_ip = get<std::string>(network, "interface_ip", "0.0.0.0");
        c.receive_buffer =
            static_cast<int>(bounded(network, "receive_buffer_bytes", 16777216, 65536, 1073741824));
        c.batch = static_cast<uint32_t>(bounded(network, "receive_batch_max", 16, 1, 64));
    }
    ip(c.interface_ip, false);
    if (!root["feeds"] || !root["feeds"].IsSequence() || root["feeds"].size() == 0)
        throw std::runtime_error("feeds must be a nonempty list");
    std::set<uint32_t> ids;
    std::set<std::string> endpoints;
    for (auto f : root["feeds"]) {
        keys(f,
             {"id", "protocol", "revision", "channel_id", "depth", "max_instruments", "recovery_buffer_bytes",
              "raw_messages", "publish_invalid_books", "endpoints", "subscriptions", "protocol_options"});
        FeedConfig q;
        q.id = static_cast<uint32_t>(bounded(f, "id", 0, 1, UINT32_MAX));
        if (!ids.insert(q.id).second)
            throw std::runtime_error("duplicate feed id");
        q.protocol = f["protocol"].as<std::string>();
        q.revision = f["revision"].as<std::string>();
        q.channel = static_cast<uint32_t>(bounded(f, "channel_id", 0, 1, UINT32_MAX));
        q.depth = static_cast<uint32_t>(bounded(f, "depth", 10, 1, FQ_MAX_DEPTH));
        q.max_instruments = static_cast<uint32_t>(bounded(f, "max_instruments", 4096, 1, 100000));
        q.cache_bytes = static_cast<size_t>(bounded(f, "recovery_buffer_bytes", 16777216, 32768, 268435456));
        q.raw = get<bool>(f, "raw_messages", false);
        q.publish_invalid = get<bool>(f, "publish_invalid_books", false);
        if (q.protocol == "cme_mdp3" && q.revision != "13")
            throw std::runtime_error("cme_mdp3 currently requires revision 13");
        if (q.protocol == "lmesource_v4" && q.revision != "4.13")
            throw std::runtime_error("lmesource_v4 currently requires revision 4.13");
        // 612 是 112 的刷新端点，二者共享一个簿记状态；614 使用独立序号，单列 feed。
        if (q.protocol == "lmesource_v4" && q.channel != 307 && q.channel != 112 && q.channel != 614)
            throw std::runtime_error("supported LME logical channels are 307, 112 and 614");
        if (q.protocol == "lmesource_v4" && q.channel == 112 && q.depth > 15)
            throw std::runtime_error("LME Core L2 supports at most 15 price levels");
        if (f["endpoints"]) {
            if (!f["endpoints"].IsSequence())
                throw std::runtime_error("endpoints must be a list");
            for (auto e : f["endpoints"]) {
                keys(e, {"group", "port", "source_ip", "role", "line"});
                Endpoint p;
                p.group = e["group"].as<std::string>();
                ip(p.group, true);
                p.port = static_cast<uint16_t>(bounded(e, "port", 0, 1, 65535));
                p.source = get<std::string>(e, "source_ip", "");
                if (!p.source.empty()) {
                    ip(p.source, false);
                    if (p.source == "0.0.0.0")
                        throw std::runtime_error("source_ip cannot be INADDR_ANY");
                }
                std::string role = e["role"].as<std::string>();
                if (role == "incremental")
                    p.role = FQ_INCREMENTAL;
                else if (role == "snapshot")
                    p.role = FQ_SNAPSHOT;
                else if (role == "definition")
                    p.role = FQ_DEFINITION;
                else if (role == "dr")
                    p.role = FQ_DR;
                else
                    throw std::runtime_error("unknown endpoint role: " + role);
                std::string line = get<std::string>(e, "line", "A");
                if (line != "A" && line != "B")
                    throw std::runtime_error("line must be A or B");
                p.line = line == "A" ? 0 : 1;
                std::string key = p.group + ":" + std::to_string(p.port);
                if (!endpoints.insert(key).second)
                    throw std::runtime_error("ambiguous duplicate group/port: " + key);
                q.endpoints.push_back(p);
            }
        }
        if (f["subscriptions"]) {
            auto s = f["subscriptions"];
            keys(s, {"instrument_ids", "symbols", "security_groups", "commodities"});
            values(s, "instrument_ids", q.instruments);
            values(s, "symbols", q.symbols);
            values(s, "security_groups", q.groups);
            values(s, "commodities", q.commodities);
            if (q.protocol == "lmesource_v4") {
                if (q.channel == 307 &&
                    (!q.instruments.empty() || !q.symbols.empty() || !q.groups.empty()))
                    throw std::runtime_error("LBMA subscriptions use commodities");
                if (q.channel != 307 &&
                    (!q.symbols.empty() || !q.groups.empty() || !q.commodities.empty()))
                    throw std::runtime_error("LME Core 112/614 subscriptions use instrument_ids");
            }
        }
        if (f["protocol_options"] && (q.protocol == "cme_mdp3" || q.protocol == "lmesource_v4"))
            throw std::runtime_error("protocol_options is reserved for registered adapters");
        if (q.protocol == "cme_mdp3" && !q.commodities.empty())
            throw std::runtime_error("CME subscriptions do not use commodities");
        if (q.protocol == "lmesource_v4" && (q.channel == 112 || q.channel == 614)) {
            bool has_incremental = false, has_snapshot = false;
            for (const auto &endpoint : q.endpoints) {
                if (q.channel == 112 && endpoint.role != FQ_INCREMENTAL && endpoint.role != FQ_SNAPSHOT)
                    throw std::runtime_error("LME 112 requires incremental 112 or snapshot 612 endpoints");
                if (q.channel == 614 && endpoint.role != FQ_SNAPSHOT)
                    throw std::runtime_error("LME 614 requires snapshot endpoints");
                has_incremental |= endpoint.role == FQ_INCREMENTAL;
                has_snapshot |= endpoint.role == FQ_SNAPSHOT;
            }
            if (!q.endpoints.empty() && q.channel == 112 && (!has_incremental || !has_snapshot))
                throw std::runtime_error("LME 112 requires both realtime 112 and refresh 612 endpoints");
        }
        q.adapter_config = YAML::Dump(f);
        c.feeds.push_back(q);
    }
    if (c.feeds.size() > 64)
        throw std::runtime_error("too many feeds (maximum 64)");
    return c;
}
} // namespace fqimpl
