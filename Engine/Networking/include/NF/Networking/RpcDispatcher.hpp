#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Networking/BitStream.hpp>
#include <string>
#include <functional>
#include <unordered_map>

namespace nf::net {

using RpcHandler = std::function<void(u32 sender_client_id, BitReader& payload)>;

struct RpcInfo {
    u32 id = 0;
    std::string name;
    RpcHandler handler;
};

/// RPC Dispatcher: routes incoming remote procedure calls to registered handlers
class RpcDispatcher {
public:
    RpcDispatcher() = default;

    /// Register a remote procedure call by ID and name
    void register_rpc(u32 rpc_id, std::string name, RpcHandler handler);

    /// Dispatch incoming RPC payload to the registered handler
    bool dispatch(u32 rpc_id, u32 sender_client_id, BitReader& payload);

    [[nodiscard]] bool has_rpc(u32 rpc_id) const;
    [[nodiscard]] std::string get_rpc_name(u32 rpc_id) const;
    [[nodiscard]] size_t registered_rpc_count() const noexcept { return m_rpcs.size(); }
    [[nodiscard]] u64 total_dispatched_count() const noexcept { return m_total_dispatched; }

    void clear();

private:
    std::unordered_map<u32, RpcInfo> m_rpcs;
    u64 m_total_dispatched = 0;
};

} // namespace nf::net
