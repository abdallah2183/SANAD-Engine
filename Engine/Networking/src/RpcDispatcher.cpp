#include <NF/Networking/RpcDispatcher.hpp>

namespace nf::net {

void RpcDispatcher::register_rpc(u32 rpc_id, std::string name, RpcHandler handler) {
    if (!handler) {
        return;
    }
    RpcInfo info;
    info.id = rpc_id;
    info.name = std::move(name);
    info.handler = std::move(handler);
    m_rpcs[rpc_id] = std::move(info);
}

bool RpcDispatcher::dispatch(u32 rpc_id, u32 sender_client_id, BitReader& payload) {
    auto it = m_rpcs.find(rpc_id);
    if (it == m_rpcs.end()) {
        return false;
    }
    it->second.handler(sender_client_id, payload);
    m_total_dispatched++;
    return true;
}

bool RpcDispatcher::has_rpc(u32 rpc_id) const {
    return m_rpcs.find(rpc_id) != m_rpcs.end();
}

std::string RpcDispatcher::get_rpc_name(u32 rpc_id) const {
    auto it = m_rpcs.find(rpc_id);
    return (it != m_rpcs.end()) ? it->second.name : "";
}

void RpcDispatcher::clear() {
    m_rpcs.clear();
    m_total_dispatched = 0;
}

} // namespace nf::net
