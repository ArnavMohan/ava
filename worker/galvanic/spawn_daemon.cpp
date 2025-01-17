#include <grpc++/grpc++.h>
#include <linux/limits.h>
#include <nvml.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "daemon_service.grpc.fb.h"
#include "daemon_service_generated.h"
#include "manager.h"
#include "manager_service.grpc.fb.h"
#include "manager_service_generated.h"

class ManagerServiceClient;

class DaemonConfig {
 public:
  static std::string const kDefaultManagerAddress;
  static std::string const kDefaultDaemonAddress;
  static int const kDefaultWorkerPortBase;
  static int const kDefaultWorkerPoolSize;

  DaemonConfig(std::string cf, std::string wp,
               std::string da = kDefaultDaemonAddress,
               ServerAddress ma = kDefaultManagerAddress,
               int wpb = kDefaultWorkerPortBase,
               int wps = kDefaultWorkerPoolSize)
      : config_file_(cf),
        worker_path_(wp),
        daemon_address_(da),
        manager_address_(ma),
        worker_port_base_(wpb),
        worker_pool_size_(wps) {}

  DaemonConfig(std::string cf, std::string wp, ServerAddress& da,
               ServerAddress& ma, int wpb = kDefaultWorkerPortBase,
               int wps = kDefaultWorkerPoolSize)
      : config_file_(cf),
        worker_path_(wp),
        daemon_address_(da),
        manager_address_(ma),
        worker_port_base_(wpb),
        worker_pool_size_(wps) {}

  std::string GetDaemonIp() const { return daemon_address_.GetIp(); }
  int GetDaemonPort() const { return daemon_address_.GetPort(); }

  void Print() {
    std::cerr << "* Manager address: " << manager_address_ << std::endl
              << "* Daemon address: " << daemon_address_ << std::endl
              << "* API server: " << worker_path_ << std::endl
              << "* API server base port: " << worker_port_base_ << std::endl
              << "* API server pool size: " << worker_pool_size_ << std::endl
              << "* Total GPU: " << visible_cuda_devices_.size() << std::endl;
    for (unsigned i = 0; i < visible_cuda_devices_.size(); ++i)
      std::cerr << "  - GPU-" << i << " UUID is "
                << visible_cuda_devices_[i].uuid_ << std::endl;
  }

  std::string config_file_;
  std::string worker_path_;
  ServerAddress daemon_address_;
  ServerAddress manager_address_;
  int worker_port_base_;
  int worker_pool_size_;
  std::unique_ptr<ManagerServiceClient> client_;

  std::vector<GpuInfo> visible_cuda_devices_;
};

std::string const DaemonConfig::kDefaultManagerAddress = "0.0.0.0:3334";
std::string const DaemonConfig::kDefaultDaemonAddress = "0.0.0.0:3335";
int const DaemonConfig::kDefaultWorkerPortBase = 4000;
int const DaemonConfig::kDefaultWorkerPoolSize = 3;

std::shared_ptr<DaemonConfig> config;

std::shared_ptr<DaemonConfig> parse_arguments(int argc, char* argv[]) {
  int c;
  opterr = 0;
  char* config_file_name = NULL;
  const char* worker_relative_path = NULL;
  char worker_path[PATH_MAX];
  std::string manager_address = DaemonConfig::kDefaultManagerAddress;
  std::string daemon_address = DaemonConfig::kDefaultDaemonAddress;
  int worker_port_base = DaemonConfig::kDefaultWorkerPortBase;
  int worker_pool_size = DaemonConfig::kDefaultWorkerPoolSize;

  while ((c = getopt(argc, argv, "f:w:m:d:b:n:")) != -1) {
    switch (c) {
      case 'f':
        config_file_name = optarg;
        break;
      case 'w':
        worker_relative_path = optarg;
        break;
      case 'm':
        manager_address = optarg;
        break;
      case 'd':
        daemon_address = optarg;
        break;
      case 'b':
        worker_port_base = atoi(optarg);
        break;
      case 'n':
        worker_pool_size = atoi(optarg);
        break;
      default:
        fprintf(stderr,
                "Usage: %s <-f config_file_name> "
                "<-w worker_path {./worker}> "
                "[-m manager_address {%s}] "
                "[-d daemon_ip:daemon_port {%s}] "
                "[-b worker_port_base {%d}] "
                "[-n worker_pool_size {%d}]\n",
                argv[0], DaemonConfig::kDefaultManagerAddress.c_str(),
                DaemonConfig::kDefaultDaemonAddress.c_str(),
                DaemonConfig::kDefaultWorkerPortBase,
                DaemonConfig::kDefaultWorkerPoolSize);
        exit(EXIT_FAILURE);
    }
  }

  if (config_file_name == NULL) {
    fprintf(stderr, "-f is mandatory. Please specify config file name\n");
    exit(EXIT_FAILURE);
  }
  if (worker_relative_path == NULL) {
    fprintf(stderr,
            "-w is mandatory. Please specify path to API server executable\n");
    exit(EXIT_FAILURE);
  }
  if (!realpath(worker_relative_path, worker_path)) {
    fprintf(stderr, "Worker binary (%s) not found. -w is optional\n",
            worker_relative_path);
    exit(EXIT_FAILURE);
  }

  return std::make_shared<DaemonConfig>(config_file_name, worker_path,
                                        daemon_address, manager_address,
                                        worker_port_base, worker_pool_size);
}

void parseConfigFile(std::shared_ptr<DaemonConfig> config) {
  std::ifstream config_file(config->config_file_);
  std::string line;
  nvmlReturn_t ret = nvmlInit();

  if (ret != NVML_SUCCESS) {
    fprintf(stderr, "Fail to get device by uuid: %s\n", nvmlErrorString(ret));
    exit(-1);
  }

  while (std::getline(config_file, line)) {
    nvmlDevice_t dev;
    nvmlMemory_t mem = {};
    char* line_cstr = (char*)line.c_str();
    char* pchr = strchr(line_cstr, '=');

    ret = nvmlDeviceGetHandleByUUID(pchr + 1, &dev);
    if (ret != NVML_SUCCESS) {
      fprintf(stderr, "Fail to get device by uuid: %s\n", nvmlErrorString(ret));
      exit(-1);
    }

    ret = nvmlDeviceGetMemoryInfo(dev, &mem);
    if (ret != NVML_SUCCESS) {
      fprintf(stderr, "Fail to get device by uuid: %s\n", nvmlErrorString(ret));
      exit(-1);
    }

    config->visible_cuda_devices_.push_back({pchr + 1, mem.free});
  }
}

class ManagerServiceClient {
 public:
  ManagerServiceClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(ManagerService::NewStub(channel)) {}

  grpc::Status RegisterDaemon(const ServerAddress& self_address) {
    /* Build message with daemon address and GPU info. */
    flatbuffers::grpc::MessageBuilder mb;
    auto sa_offset = mb.CreateString(self_address.GetAddress());
    std::vector<uint64_t> fm;
    std::vector<flatbuffers::Offset<flatbuffers::String>> uuid;
    for (auto const& gpuinfo : config->visible_cuda_devices_) {
      fm.push_back(gpuinfo.free_memory_);
      uuid.push_back(mb.CreateString(gpuinfo.uuid_));
    }
    auto fm_offset = mb.CreateVector(fm.data(), fm.size());
    auto uu_offset = mb.CreateVector(uuid.data(), uuid.size());
    auto request_offset =
        CreateDaemonRegisterRequest(mb, sa_offset, fm_offset, uu_offset);
    mb.Finish(request_offset);
    auto request_msg = mb.ReleaseMessage<DaemonRegisterRequest>();

    /* Send request. */
    grpc::ClientContext context;
    auto status = stub_->RegisterDaemon(&context, request_msg, nullptr);
    if (!status.ok()) {
      std::cerr << status.error_code() << ": " << status.error_message()
                << std::endl;
    }
    return status;
  }

  grpc::Status NotifyWorkerExit(const int worker_port,
                                const std::string& uuid) {
    /* Build message. */
    flatbuffers::grpc::MessageBuilder mb;
    auto wa_offset = mb.CreateString(config->GetDaemonIp() + ":" +
                                     std::to_string(worker_port));
    std::vector<flatbuffers::Offset<flatbuffers::String>> uuid_mb;
    uuid_mb.push_back(mb.CreateString(uuid));
    auto uu_offset = mb.CreateVector(uuid_mb);
    auto request_offset =
        CreateWorkerExitNotifyRequest(mb, wa_offset, uu_offset);
    mb.Finish(request_offset);
    auto request_msg = mb.ReleaseMessage<WorkerExitNotifyRequest>();

    /* Send request. */
    grpc::ClientContext context;
    auto status = stub_->NotifyWorkerExit(&context, request_msg, nullptr);
    if (!status.ok()) {
      std::cerr << status.error_code() << ": " << status.error_message()
                << std::endl;
    }
    return status;
  }

 private:
  std::unique_ptr<ManagerService::Stub> stub_;
};

class DaemonServiceImpl final : public DaemonService::Service {
 public:
  DaemonServiceImpl() : DaemonService::Service() { worker_id_.store(0); }

  virtual grpc::Status SpawnWorker(
      grpc::ServerContext* context,
      const flatbuffers::grpc::Message<WorkerSpawnRequest>* request_msg,
      flatbuffers::grpc::Message<WorkerSpawnReply>* response_msg) override {
    const WorkerSpawnRequest* request = request_msg->GetRoot();
    unsigned count = request->count();
    if (count == 0 || request->uuid() == nullptr ||
        request->uuid()->str().empty()) {
      grpc::Status status(grpc::StatusCode::INVALID_ARGUMENT,
                          "Zero count or mismatched uuid/gpu_mem vectors");
      return status;
    }

    std::string uuid = request->uuid()->str();
    std::cerr << "[" << context->peer() << "] Request to spawn "
              << std::to_string(count) << " API servers on" << uuid
              << std::endl;

    std::vector<flatbuffers::Offset<flatbuffers::String>> worker_address;
    flatbuffers::grpc::MessageBuilder mb;

    /* Spawn API servers. */
    for (unsigned i = 0; i < count; ++i) {
      // TODO(galvanic): check API server pool before spawning a new API server.
      int port = SpawnWorker(uuid);
      auto ao =
          mb.CreateString(config->GetDaemonIp() + ":" + std::to_string(port));
      worker_address.push_back(ao);
    }

    auto vo = mb.CreateVector(worker_address.data(), worker_address.size());
    auto response_offset = CreateWorkerSpawnReply(mb, vo);
    mb.Finish(response_offset);
    *response_msg = mb.ReleaseMessage<WorkerSpawnReply>();

    return grpc::Status::OK;
  }

 private:
  int GetWorkerPort() {
    return config->worker_port_base_ +
           worker_id_.fetch_add(1, std::memory_order_relaxed);
  }

  int SpawnWorker(const std::string& uuid) {
    int port = GetWorkerPort();
    std::cerr << "Spawn API server at port=" << port << " UUID=" << uuid
              << std::endl;

    pid_t child_pid = fork();
    if (child_pid) {
      auto child_monitor = std::make_shared<std::thread>(
          MonitorWorkerExit, this, child_pid, port, uuid);
      worker_monitor_map_.insert({port, child_monitor});
      return port;
    }

    std::string visible_dev = "CUDA_VISIBLE_DEVICES=" + uuid;
    char* const argv_list[] = {(char*)"worker",
                               (char*)std::to_string(port).c_str(), NULL};
    char* const envp_list[] = {(char*)visible_dev.c_str(),
                               (char*)"AVA_CHANNEL=TCP", NULL};
    if (execvpe(config->worker_path_.c_str(), argv_list, envp_list) < 0)
      perror("execv worker");

    /* Never reach here. */
    return port;
  }

  static void MonitorWorkerExit(DaemonServiceImpl* service, pid_t child_pid,
                                int port, const std::string& uuid) {
    pid_t ret = waitpid(child_pid, NULL, 0);
    std::cerr << "API server (" << uuid[0] << "...) at :" << port
              << " has exit (waitpid=" << ret << ")" << std::endl;
    config->client_->NotifyWorkerExit(port, uuid);
    service->worker_monitor_map_.erase(port);
  }

  std::atomic<int> worker_id_;
  std::map<int, std::shared_ptr<std::thread>> worker_monitor_map_;
};

void runDaemonService(std::shared_ptr<DaemonConfig> config) {
  std::string server_address("0.0.0.0:" +
                             std::to_string(config->GetDaemonPort()));
  DaemonServiceImpl service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cerr << "Daemon Service listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char* argv[]) {
  config = parse_arguments(argc, argv);
  parseConfigFile(config);
  config->Print();

  /* Create API server pool. */
  // TODO(galvanic): call SpawnWorker() to create API server pool.

  std::thread server_thread(runDaemonService, config);

  /* Register daemon. */
  auto channel = grpc::CreateChannel(config->manager_address_.GetAddress(),
                                     grpc::InsecureChannelCredentials());
  config->client_ = std::make_unique<ManagerServiceClient>(channel);
  auto status = config->client_->RegisterDaemon(config->daemon_address_);

  server_thread.join();

  return 0;
}
