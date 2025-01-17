#include <sys/wait.h>
#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/lockfree/queue.hpp>
#include <future>
#include <iostream>
#include <thread>

#include "argument_parser.hpp"
#include "manager_service.hpp"
#include "manager_service.proto.h"

using ava_manager::ManagerServiceServerBase;

bool cfgWorkerPoolDisabled = true;
uint32_t cfgWorkerPoolSize = 3;

class LegacyManager : public ManagerServiceServerBase {
 public:
  LegacyManager(uint32_t port, uint32_t worker_port_base,
                std::string worker_path, std::vector<std::string>& worker_argv)
      : ManagerServiceServerBase(port, worker_port_base, worker_path,
                                 worker_argv) {
    // Spawn worker pool with default environment variables
    if (!cfgWorkerPoolDisabled) {
      for (uint32_t i = 0; i < cfgWorkerPoolSize; i++) {
        auto worker_address = SpawnWorkerWrapper();
        worker_pool_.push(worker_address);
      }
    }
  }

 private:
  uint32_t SpawnWorkerWrapper() {
    // Let API server use TCP channel
    std::vector<std::string> environments;
    environments.push_back("AVA_CHANNEL=TCP");

    // Pass port to API server
    auto port =
        worker_port_base_ + worker_id_.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::string> parameters;
    parameters.push_back(std::to_string(port));

    // Append custom API server arguments
    for (const auto& argv : worker_argv_) {
      parameters.push_back(argv);
    }

    std::cerr << "Spawn API server at 0.0.0.0:" << port << " (cmdline=\""
              << boost::algorithm::join(environments, " ") << " "
              << " " << boost::algorithm::join(parameters, " ") << "\")"
              << std::endl;

    auto child_pid = SpawnWorker(environments, parameters);

    auto child_monitor = std::make_shared<std::thread>(
        [](pid_t child_pid, uint32_t port,
           std::map<pid_t, std::shared_ptr<std::thread>>* worker_monitor_map) {
          pid_t ret = waitpid(child_pid, NULL, 0);
          std::cerr << "[pid=" << child_pid << "] API server at ::" << port
                    << " has exit (waitpid=" << ret << ")" << std::endl;
          worker_monitor_map->erase(port);
        },
        child_pid, port, &worker_monitor_map_);
    child_monitor->detach();
    worker_monitor_map_.insert({port, child_monitor});

    return port;
  }

  ava_proto::WorkerAssignReply HandleRequest(
      const ava_proto::WorkerAssignRequest& request) {
    ava_proto::WorkerAssignReply reply;
    uint32_t worker_port;

    if (worker_pool_.pop(worker_port)) {
      worker_pool_.push(SpawnWorkerWrapper());
    } else {
      worker_port = SpawnWorkerWrapper();
    }
    reply.worker_address().push_back("0.0.0.0:" + std::to_string(worker_port));

    return reply;
  }

  boost::lockfree::queue<uint32_t, boost::lockfree::capacity<128>> worker_pool_;
};

namespace {
std::unique_ptr<LegacyManager> manager;
}

int main(int argc, const char* argv[]) {
  auto arg_parser = ArgumentParser(argc, argv);
  arg_parser.init_and_parse_options();
  cfgWorkerPoolDisabled = arg_parser.disable_worker_pool;
  cfgWorkerPoolSize = arg_parser.worker_pool_size;

  std::at_quick_exit([] {
    if (manager) {
      manager->StopServer();
    }
  });
  signal(SIGINT, [](int) -> void {
    signal(SIGINT, SIG_DFL);
    std::quick_exit(EXIT_SUCCESS);
  });
  manager = std::make_unique<LegacyManager>(
      arg_parser.manager_port, arg_parser.worker_port_base,
      arg_parser.worker_path, arg_parser.worker_argv);
  manager->RunServer();
  return 0;
}
