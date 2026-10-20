#ifndef FUTURES_QUOTE_CLIENT_HPP
#define FUTURES_QUOTE_CLIENT_HPP
#include "fq.h"
#include <stdexcept>
#include <string>
namespace fq {
/** C++11 的资源管理包装，真正的配置、收包、解码和恢复由 fq_client 实现。
 * 工作流：open 加载配置；start 在线程中收包，或由调用方使用 poll/feed/replay 驱动。
 * 输入按 feed_id 进入独立协议适配器，再通过同一个同步回调输出 fq_event。
 * 两种驱动方式互斥；本类不增加异步事件队列，也不延长回调数据的生命周期。
 * 生命周期：构造 → open → start → request_stop → join → 析构。
 * 对象独占句柄，禁止复制；析构会停止并回收资源，不得在自身回调中销毁对象。
 */
class Client {
    fq_client *handle_;
    // 仅在 C++ 包装层把 C 错误码转换成异常，协议错误详情来自句柄诊断。
    void check(int code) {
        if (code < 0) {
            char error[512];
            fq_last_error(handle_, error, sizeof error);
            throw std::runtime_error(error[0] ? error : "futures_quote operation failed");
        }
    }

  public:
    // user 由调用方持有；至少存活到所有回调结束。构造时尚未读取配置或建立网络连接。
    explicit Client(fq_on_event callback, void *user = 0) : handle_(0) {
        fq_callbacks c = {sizeof(c), FQ_ABI_VERSION, callback, user};
        if (fq_create(&c, &handle_) != FQ_OK)
            throw std::runtime_error("fq_create failed");
    }
    ~Client() { fq_destroy(handle_); }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    // 只初始化配置与适配器；实际加组发生在 start 或首次 poll。
    void open(const std::string &path) { check(fq_open(handle_, path.c_str())); }
    // 库线程负责串行收包/解析/回调，调用方不要再调用 poll/feed/replay。
    void start() { check(fq_start(handle_)); }
    // 可在回调中请求停止；此方法不等待线程退出。
    void request_stop() { fq_request_stop(handle_); }
    // 先请求停止再等待；等待期间业务上下文仍需有效。
    void join() { check(fq_join(handle_)); }
    // 调用方线程驱动在线接收，ms=0 为不阻塞等待。
    void poll(int ms = 0) { check(fq_poll(handle_, ms)); }
    // 接入已有收包框架：同步输入一份 UDP payload，缓冲区只需保持到返回。
    void feed(const fq_datagram &d) { check(fq_feed(handle_, &d)); }
    // 离线读取 PCAP，共用在线解析状态机；需要新会话时先重新 open。
    void replay(const std::string &path) { check(fq_replay_pcap(handle_, path.c_str())); }
    // 查询适配器实现了哪些能力，不代表盘口当前已恢复完成。
    uint64_t capabilities(uint32_t feed) { return fq_capabilities(handle_, feed); }
    // 借用底层句柄以调用额外 C API，不转移所有权，不能自行 destroy。
    fq_client *native_handle() { return handle_; }
};
} // namespace fq
#endif
