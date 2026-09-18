#include "sdk_log.h"
#include "session_log.h"
#include "sidecar_queue.h"
#include "transport.h"
#include "worker.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    tombstone::SdkLog log;
    tombstone::SessionLog session(log);
    tombstone::SidecarQueue sidecars(log);
    session.configure(argv[2]);
    sidecars.configure(argv[2]);
    session.rotate_for_new_session();
    const char message[] = "native wire audit\0UTF-8: \xe6\xbc\xa2";
    session.append("2026-09-18T00:00:00.000Z", "info", {message, sizeof(message) - 1});
    tombstone::Transport transport(log);
    const char *token = std::getenv("TOMBSTACK_WIRE_TOKEN");
    tombstone::Worker worker(transport, sidecars, session, log,
                             token ? token : "audit-public-token");
    const char *response_path = std::getenv("TOMBSTACK_WIRE_RESPONSE");
    if (response_path) worker.set_ack_handler([response_path](const std::string &body) {
        std::ofstream(response_path, std::ios::binary) << body;
    });
    tombstone::UploadJob job;
    job.url = argv[1];
    job.body = R"({"log":true,"fixture":"native wire audit"})";
    if (const char *body_path = std::getenv("TOMBSTACK_WIRE_BODY")) {
        std::ifstream input(body_path, std::ios::binary);
        if (!input) return 3;
        job.body.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    job.sign_body = true;
    job.request_log = true;
    job.parse_ack = response_path != nullptr;
    job.no_persist = true;
    worker.enqueue(std::move(job));
    worker.start();
    const bool drained = worker.flush(std::chrono::seconds{20});
    worker.stop();
    return drained ? 0 : 1;
}
