// Samples/Server/main.cpp — NOVAForgeServer: a dedicated server that runs the
// same simulation a client does, with no window, no Vulkan device and no
// renderer (design §81).
//
// It links NFCore + NFJobs + NFNetworking only. Anything that pulls in RHI or
// Rendering would defeat the point: the whole reason this target exists is that
// a server must run on a machine with no GPU.
//
// Loop:
//   1. Wait (pumping sockets) until every connected client has delivered its
//      input for the current tick, or the per-tick deadline passes. A fixed
//      rate alone would race the clients — stepping tick 5 while a client's
//      input for tick 5 is still in flight applies neutral and that client's
//      snapshot never arrives.
//   2. tick(): step the authoritative world, broadcast the snapshot.
//   3. Exit after --ticks (default 600, i.e. 10s at 60Hz), so a test can start
//      and stop it deterministically.
//
// Usage: NOVAForgeServer --port 7777 --ticks 600 --max-players 16

#include <NF/Networking/DedicatedServer.hpp>
#include <NF/Networking/Snapshot.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace nf;
using namespace nf::net;

namespace {

struct CliArgs {
    u16 port = 0;              // 0 = ephemeral; the chosen port is printed
    u32 ticks = 600;           // 0 = run until shutdown command
    u32 max_players = 16;
    u32 tick_deadline_ms = 100; // per-tick input wait before stepping anyway
    float fixed_dt = 1.0f / 60.0f;
    bool print_port = true;    // "PORT <n>" line for a test harness to parse
};

CliArgs parse_args(int argc, char** argv) {
    CliArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto take = [&](u32& out) {
            if (i + 1 < argc) out = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        };
        if ((arg == "--port" && i + 1 < argc)) a.port = static_cast<u16>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--ticks" || arg.rfind("--ticks=", 0) == 0) {
            if (arg == "--ticks") take(a.ticks);
            else a.ticks = static_cast<u32>(std::strtoul(arg.data() + 8, nullptr, 10));
        } else if (arg == "--max-players" && i + 1 < argc) {
            take(a.max_players);
        } else if (arg == "--tick-deadline-ms" && i + 1 < argc) {
            take(a.tick_deadline_ms);
        } else if (arg == "--quiet") {
            a.print_port = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "NOVAForgeServer — headless dedicated server (design 81)\n"
                      << "  --port N             UDP port (0 = ephemeral)\n"
                      << "  --ticks N            Exit after N ticks (0 = until shutdown)\n"
                      << "  --max-players N      Player slots (default 16)\n"
                      << "  --tick-deadline-ms N Wait per tick for stragglers (default 100)\n"
                      << "  --quiet              Do not print the port line\n"
                      << "  --help               This message\n";
            std::exit(0);
        }
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const CliArgs args = parse_args(argc, argv);

    DedicatedServerConfig config;
    config.port = args.port;
    config.loopback_only = true; // a public host overrides this in a real deploy
    config.max_players = args.max_players;
    config.fixed_dt = args.fixed_dt;

    DedicatedServer server(config);
    std::string err;
    if (!server.start(&err)) {
        std::cerr << "NOVAForgeServer: failed to bind port " << args.port << ": "
                  << (err.empty() ? "unknown error" : err) << "\n";
        return 1;
    }
    if (args.print_port) {
        // A test harness parses this to discover an ephemeral port.
        std::cout << "PORT " << server.local_port() << std::endl;
    }

    u64 now_ms = 0;
    u32 tick = 0;
    while (server.is_running()) {
        if (args.ticks != 0 && tick >= args.ticks) break;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(args.tick_deadline_ms);
        while (!server.all_inputs_in_for(tick)) {
            server.pump(now_ms);
            if (std::chrono::steady_clock::now() >= deadline) break; // step anyway
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            now_ms += 1;
        }

        server.tick(now_ms);
        now_ms += 20; // 60Hz tick clock for resend pacing
        ++tick;
    }

    server.stop();
    if (args.print_port) {
        std::cout << "TICKS " << tick << " PLAYERS " << server.player_count()
                  << " REFUSED " << server.refused_connections()
                  << " MALFORMED " << server.malformed_packets() << std::endl;
    }
    return 0;
}
