// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/test/xwalkdriver/xwalk/sdb_impl.h"

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/json/string_escape.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/message_loop/message_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_tokenizer.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "xwalk/test/xwalkdriver/net/adb_client_socket.h"
#include "xwalk/test/xwalkdriver/xwalk/status.h"

namespace {

// This class is bound in the callback to AdbQuery and isn't freed until the
// callback is run, even if the function that creates the buffer times out.
class ResponseBuffer : public base::RefCountedThreadSafe<ResponseBuffer> {
 public:
  ResponseBuffer() : ready_(true, false) {}

  void OnResponse(int result, const std::string& response) {
    response_ = response;
    result_ = result;
    ready_.Signal();
  }

  Status GetResponse(
      std::string* response, const base::TimeDelta& timeout) {
    base::TimeTicks deadline = base::TimeTicks::Now() + timeout;
    while (!ready_.IsSignaled()) {
      base::TimeDelta delta = deadline - base::TimeTicks::Now();
      if (delta <= base::TimeDelta())
        return Status(kTimeout, base::StringPrintf(
            "Sdb command timed out after %d seconds",
            static_cast<int>(timeout.InSeconds())));
      ready_.TimedWait(timeout);
    }
    if (result_ < 0)
      return Status(kUnknownError,
          "Failed to run sdb command, is the sdb server running?");
    *response = response_;
    return Status(kOk);
  }

 private:
  friend class base::RefCountedThreadSafe<ResponseBuffer>;
  ~ResponseBuffer() {}

  std::string response_;
  int result_;
  base::WaitableEvent ready_;
};

void ExecuteCommandOnIOThread(
    const std::string& command, scoped_refptr<ResponseBuffer> response_buffer,
    int port) {
  CHECK(base::MessageLoopForIO::IsCurrent());
  AdbClientSocket::AdbQuery(port, command,
      base::Bind(&ResponseBuffer::OnResponse, response_buffer));
}

}  // namespace

SdbImpl::SdbImpl(
    const scoped_refptr<base::SingleThreadTaskRunner>& io_task_runner,
    int port)
    : io_task_runner_(io_task_runner), port_(port) {
  CHECK(io_task_runner_.get());
}

SdbImpl::~SdbImpl() {}

bool SdbImpl::IsTizenAppRunning(
    const std::string& device_serial,
    const std::string& app_id) {
  std::string  response;
  std::string app_launcher_cmd = "su - app -c \"app_launcher -S\"";

  Status status = ExecuteHostShellCommand(device_serial, app_launcher_cmd, &response);

  return (response.find(app_id) != std::string::npos);
}

Status SdbImpl::GetDevices(std::vector<std::string>* devices) {
  std::string response;
  Status status = ExecuteCommand("host:devices", &response);
  if (!status.IsOk())
    return status;
  base::StringTokenizer lines(response, "\n");
  while (lines.GetNext()) {
    std::vector<std::string> fields;
    base::SplitStringAlongWhitespace(lines.token(), &fields);
    if ((fields.size() == 3 || fields.size() == 2) && fields[1] == "device") {
      devices->push_back(fields[0]);
    }
  }
  return Status(kOk);
}

Status SdbImpl::ForwardPort(
    const std::string& device_serial, 
    int local_port,
    const std::string& remote_port) {
  std::string response;
  Status status = ExecuteHostCommand(
      device_serial,
      "forward:tcp:" + base::IntToString(local_port) + ";tcp:" + 
      remote_port,
      &response);
  if (!status.IsOk())
    return status;
  if (response == "OKAY")
    return Status(kOk);
  return Status(kUnknownError, response);
}

Status SdbImpl::SetCommandLineFile(
    const std::string& device_serial,
    const std::string& command_line_file,
    const std::string& exec_name,
    const std::string& args) {
  return Status(kOk);
}

Status SdbImpl::CheckAppInstalled(
    const std::string& device_serial, 
    const std::string& app_id) {
  std::string response;
  std::string app_launcher_cmd = "su - app -c \"app_launcher -l\"";

  Status status = ExecuteHostShellCommand(device_serial, app_launcher_cmd, &response);
  printf (">>>>>>>> SdbImpl::CheckInstall %s \n", status.message().c_str());
 
  if (response.find(app_id) == std::string::npos)
    return Status(kUnknownError, app_id + " is not installed on device " +
                  device_serial);
  return Status(kOk);
}

Status SdbImpl::ClearAppData(
    const std::string& device_serial, 
    const std::string& app_id) {
  return Status(kOk);
}

Status SdbImpl::SetDebugApp(
    const std::string& device_serial, 
    const std::string& app_id) {
  return Status(kOk);
}

Status SdbImpl::Launch(
    const std::string& device_serial,
    const std::string& app_id) {
  Status status(kOk);
  // when re-launch tizen app by invoking app_launcher on tizen, the app
  // is an old process, so we need to kill it first.
  if (IsTizenAppRunning(device_serial, app_id))
    status = ForceStop(device_serial, app_id);

  if (status.IsError())
    return Status(kUnknownError, 
                  "Failed to re-launch " + app_id + " on device " + device_serial);

  std::string response;
  std::string app_launcher_cmd = "su - app -c \" "\
                              "app_launcher -s " + app_id + " -d \"";

  status = ExecuteHostShellCommand(device_serial, app_launcher_cmd, &response);

  printf (">>>>>>>> SdbImpl::launched \n");
  if (status.IsError())
    printf("Launch failed %s \n", response.c_str());

  return status;
}

Status SdbImpl::ForceStop(
    const std::string& device_serial, 
    const std::string& app_id) {
  std::string response;
  std::string app_launcher_cmd = "su - app -c \" "\
                      "app_launcher -k " + app_id + " \"";
  return  ExecuteHostShellCommand(device_serial, app_launcher_cmd, &response);
}

Status SdbImpl::GetPidByName(
    const std::string& device_serial,
    const std::string& process_name,
    int* pid) {
  std::string response;
  Status status = ExecuteHostShellCommand(device_serial, "ps", &response);
  if (!status.IsOk())
    return status;

  std::vector<std::string> lines;
  base::SplitString(response, '\n', &lines);
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string line = lines[i];
    if (line.empty())
      continue;
    std::vector<std::string> tokens;
    base::SplitStringAlongWhitespace(line, &tokens);
    if (tokens.size() != 9)
      continue;
    if (tokens[8].compare(process_name) == 0) {
      if (base::StringToInt(tokens[1], pid)) {
        return Status(kOk);
      } else {
        break;
      }
    }
  }

  return Status(kUnknownError,
                "Failed to get PID for the following process: " + process_name);
}

std::string SdbImpl::GetOperatingSystemName() {
  return "Tizen";
}

Status SdbImpl::ExecuteCommand(
    const std::string& command, 
    std::string* response) {
  scoped_refptr<ResponseBuffer> response_buffer = new ResponseBuffer;
  VLOG(1) << "Sending sdb command: " << command;
  io_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&ExecuteCommandOnIOThread, command, response_buffer, port_));
  if (command.find("ps auxww") != std::string::npos)
    sleep(1);
  int delta_time = 30;
  if (command.find("xwalkctl") != std::string::npos)
    delta_time = 3;
  Status status = response_buffer->GetResponse(
      response, base::TimeDelta::FromSeconds(delta_time));
  if (status.IsOk()) {
    VLOG(1) << "Received sdb response: " << *response;
  }
  return status;
}

Status SdbImpl::ExecuteHostCommand(
    const std::string& device_serial,
    const std::string& host_command, std::string* response) {
  return ExecuteCommand(
      "host-serial:" + device_serial + ":" + host_command, response);
}

Status SdbImpl::ExecuteHostShellCommand(
    const std::string& device_serial,
    const std::string& shell_command,
    std::string* response) {
  return ExecuteCommand(
      "host:transport:" + device_serial + "|shell:" + shell_command,
      response);
}
