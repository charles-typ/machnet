/**
 * @file main.cc
 * Simple hello world application using only Machnet public APIs
 * Usage:
 *  - First start the server: ./hello_world --local=<local IP>
 *  - Client:
 *    ./hello_world --local=<local IP> --remote=<server IP> --is_client=1
 */

#include <gflags/gflags.h>
#include <machnet.h>

#include <array>
#include <chrono>
#include <iostream>

DEFINE_string(local, "", "Local IP address");
DEFINE_string(remote, "", "Remote IP address");

static constexpr uint16_t kPort = 31580;

// assert with message
void assert_with_msg(bool cond, const char *msg) {
  if (!cond) {
    printf("%s\n", msg);
    exit(-1);
  }
}

int main(int argc, char *argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  int ret = machnet_init();
  assert_with_msg(ret == 0, "machnet_init() failed");

  void *channel = machnet_attach();
  assert_with_msg(channel != nullptr, "machnet_attach() failed");

  ret = machnet_listen(channel, FLAGS_local.c_str(), kPort);
  assert_with_msg(ret == 0, "machnet_listen() failed");

  printf("Listening on %s:%d\n", FLAGS_local.c_str(), kPort);

  if (FLAGS_remote != "") {
    printf("Sending message to %s:%d\n", FLAGS_remote.c_str(), kPort);
    MachnetFlow flow;
    std::array<char, 1024> buf;
    std::string msg = "Hello World!";
    ret = machnet_connect(channel, FLAGS_local.c_str(), FLAGS_remote.c_str(),
                          kPort, &flow);
    assert_with_msg(ret == 0, "machnet_connect() failed");

    for(int i = 0; i < 100; i++) {
      auto start = std::chrono::high_resolution_clock::now();
      int ret = machnet_send(channel, flow, msg.data(), msg.size());
      if (ret == -1) printf("machnet_send() failed\n");
      while (true) {
        ret = machnet_recv(channel, buf.data(), buf.size(), &flow);
        if (ret == 0) {
          //usleep(10);
          continue;
        }
      }
      auto end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
      std::cout << "Time taken by operation: "
              << duration.count() / (double)1000 << " nanoseconds" << std::endl;

    }
  } else {
    printf("Waiting for message from client\n");
    size_t count = 0;

    while (true) {
      std::array<char, 1024> buf;
      MachnetFlow flow;
      ssize_t ret = machnet_recv(channel, buf.data(), buf.size(), &flow);
      assert_with_msg(ret >= 0, "machnet_recvmsg() failed");
      if (ret == 0) {
        usleep(10);
        continue;
      }

      std::string msg(buf.data(), ret);
      printf("Received message: %s, count = %zu\n", msg.c_str(), count++);
      ret = machnet_send(channel, flow, msg.data(), msg.size());
      if (ret == -1) printf("machnet_send() failed\n");
      printf("Sent message: %d\n", ret);
    }
  }

  return 0;
}
