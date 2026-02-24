#pragma once

#include "ctl/client.h"
#include "ctl/output/formatter.h"

namespace jq::ctl {

// ---------------------------------------------------------------------------
// Job commands  (src/ctl/commands/jobs.cc)
// ---------------------------------------------------------------------------
int RunJobSubmit (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunJobStatus (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunJobCancel (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunJobList   (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunJobLogs   (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunJobRetry  (GrpcClient& client, OutputFormat fmt, int argc, char** argv);

// ---------------------------------------------------------------------------
// Queue commands  (src/ctl/commands/queues.cc)
// ---------------------------------------------------------------------------
int RunQueueList   (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunQueueCreate (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunQueueDelete (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunQueueStats  (GrpcClient& client, OutputFormat fmt, int argc, char** argv);

// ---------------------------------------------------------------------------
// Worker commands  (src/ctl/commands/workers.cc)
// ---------------------------------------------------------------------------
int RunWorkerList     (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunWorkerDrain    (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunWorkerShutdown (GrpcClient& client, OutputFormat fmt, int argc, char** argv);

// ---------------------------------------------------------------------------
// System commands  (src/ctl/commands/system.cc)
// ---------------------------------------------------------------------------
int RunSystemStatus (GrpcClient& client, OutputFormat fmt, int argc, char** argv);
int RunVersion      (GrpcClient& client, OutputFormat fmt, int argc, char** argv);

}  // namespace jq::ctl
