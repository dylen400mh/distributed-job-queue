// src/ctl/main.cc
// jq-ctl: operator CLI tool.

#include <getopt.h>

#include <iostream>
#include <string>

#include "common/config/flags.h"
#include "ctl/client.h"
#include "ctl/commands/commands.h"
#include "ctl/output/formatter.h"

static void PrintUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [GLOBAL OPTIONS] <group> <subcommand> [OPTIONS]\n\n"
        << "Groups / subcommands:\n"
        << "  job     submit | status | cancel | list | logs | retry\n"
        << "  queue   list | create | delete | stats\n"
        << "  worker  list | drain | shutdown\n"
        << "  system  status\n"
        << "  version\n\n"
        << "Global options:\n"
        << "  --server-addr <host:port>    jq-server address (default: localhost:50051)\n"
        << "  --output <table|json|yaml>   output format (default: table)\n"
        << "  --timeout <s>                request timeout (default: 10)\n"
        << "  --tls-cert <path>            client TLS certificate\n"
        << "  --tls-key <path>             client TLS key\n"
        << "  --version                    print version and exit\n"
        << "  --help                       show this help\n";
}

int main(int argc, char** argv) {
    auto flags = jq::ParseCtlFlags(argc, argv);

    if (flags.version) { jq::PrintVersion(); return 0; }
    if (flags.help || optind >= argc) {
        if (!flags.help) std::cerr << "Error: missing command\n\n";
        PrintUsage(argv[0]);
        return flags.help ? 0 : 1;
    }

    jq::ctl::OutputFormat fmt = jq::ctl::ParseFormat(flags.output);
    jq::GrpcClient client(flags.server_addr, flags.timeout,
                          flags.tls_cert, flags.tls_key);

    std::string group = argv[optind++];

    // ---- version (no subcommand needed) ----
    if (group == "version") {
        return jq::ctl::RunVersion(client, fmt, argc, argv);
    }

    // ---- system status (subcommand optional) ----
    if (group == "system" || group == "status") {
        if (optind < argc && std::string(argv[optind]) == "status") ++optind;
        return jq::ctl::RunSystemStatus(client, fmt, argc, argv);
    }

    // All other groups require a subcommand.
    if (optind >= argc) {
        std::cerr << "Error: missing subcommand for '" << group << "'\n\n";
        PrintUsage(argv[0]);
        return 1;
    }
    std::string sub = argv[optind++];

    // ---- job ----
    if (group == "job") {
        if (sub == "submit")  return jq::ctl::RunJobSubmit (client, fmt, argc, argv);
        if (sub == "status")  return jq::ctl::RunJobStatus (client, fmt, argc, argv);
        if (sub == "cancel")  return jq::ctl::RunJobCancel (client, fmt, argc, argv);
        if (sub == "list")    return jq::ctl::RunJobList   (client, fmt, argc, argv);
        if (sub == "logs")    return jq::ctl::RunJobLogs   (client, fmt, argc, argv);
        if (sub == "retry")   return jq::ctl::RunJobRetry  (client, fmt, argc, argv);
        std::cerr << "Unknown job subcommand: " << sub << '\n';
        return 1;
    }

    // ---- queue ----
    if (group == "queue") {
        if (sub == "list")   return jq::ctl::RunQueueList   (client, fmt, argc, argv);
        if (sub == "create") return jq::ctl::RunQueueCreate (client, fmt, argc, argv);
        if (sub == "delete") return jq::ctl::RunQueueDelete (client, fmt, argc, argv);
        if (sub == "stats")  return jq::ctl::RunQueueStats  (client, fmt, argc, argv);
        std::cerr << "Unknown queue subcommand: " << sub << '\n';
        return 1;
    }

    // ---- worker ----
    if (group == "worker") {
        if (sub == "list")     return jq::ctl::RunWorkerList     (client, fmt, argc, argv);
        if (sub == "drain")    return jq::ctl::RunWorkerDrain    (client, fmt, argc, argv);
        if (sub == "shutdown") return jq::ctl::RunWorkerShutdown (client, fmt, argc, argv);
        std::cerr << "Unknown worker subcommand: " << sub << '\n';
        return 1;
    }

    std::cerr << "Unknown command: " << group << '\n';
    PrintUsage(argv[0]);
    return 1;
}
