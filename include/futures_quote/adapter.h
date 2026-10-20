#ifndef FUTURES_QUOTE_ADAPTER_H
#define FUTURES_QUOTE_ADAPTER_H
#include "fq.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 新协议的 C 扩展边界：先注册适配器，再创建/open client；每个 feed 拥有独立 state。
 * 注册名称对应 YAML 的 protocol，能力位供上层选择可用事件。
 * 描述符、名称及函数应保持有效到所有相关 client 销毁；禁止异常跨越这些入口。
 * emit 只能在 on_datagram 内同步调用，不能在创建/销毁或自建后台线程中调用。 */
typedef struct fq_adapter_options {
    uint32_t struct_size, abi_version, feed_id, channel_id, depth, max_instruments;
    uint64_t recovery_buffer_bytes;
    const char *protocol_revision;
    /* 完整 feed YAML，含可选 protocol_options；字符串仅在 create 期间借用，需要留存须复制。 */
    const char *feed_config_yaml;
} fq_adapter_options;
typedef struct fq_adapter_v1 {
    uint32_t struct_size, abi_version;
    const char *protocol;
    uint64_t capabilities;
    /* 创建协议私有状态；保存 emit/context 用于同步发布，失败返回 NULL。 */
    void *(*create)(const fq_adapter_options *, fq_on_event emit, void *context);
    /* 处理完整输入，负责本协议的边界、序号、恢复和订阅规则；负值表示拒绝该输入。 */
    int (*on_datagram)(void *state, const fq_datagram *);
    /* 释放 create 返回的状态；不再发出事件。 */
    void (*destroy)(void *state);
} fq_adapter_v1;
/* 仅注册工厂，不建立网络连接；内置名称或重复名称返回 FQ_ESTATE。 */
FQ_API int fq_register_adapter(const fq_adapter_v1 *adapter);
#ifdef __cplusplus
}
#endif
#endif
