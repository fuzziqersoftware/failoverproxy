#define _STDC_FORMAT_MACROS

#include <event2/event.h>
#include <event2/dns.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <memory>
#include <unordered_map>

#include <phosg/Network.hh>
#include <phosg/Strings.hh>

using namespace std;



struct ProxyConnection {
  shared_ptr<struct bufferevent> client_bev;
  shared_ptr<struct bufferevent> server_bev;
};

struct ServerConfiguration {
  string dest_host;
  uint16_t dest_port;

  uint16_t proxy_listen_port;
  int proxy_listen_fd;
  shared_ptr<struct evconnlistener> proxy_listener;
  uint16_t shell_listen_port;
  int shell_listen_fd;
  shared_ptr<struct evconnlistener> shell_listener;

  shared_ptr<struct event_base> base;
  shared_ptr<struct evdns_base> dns_base;
  unordered_map<ProxyConnection*, shared_ptr<ProxyConnection>> all_conns;
  unordered_map<struct bufferevent*, shared_ptr<struct bufferevent>> shell_clients;
};

ServerConfiguration state;



void signal_handler(int signum) {
  if (((signum == SIGTERM) || (signum == SIGINT)) && state.base.get()) {
    event_base_loopexit(state.base.get(), NULL);
  }
}



void on_proxy_bufferevent_read(struct bufferevent* bev, void* ctx) {
  ProxyConnection* conn = reinterpret_cast<ProxyConnection*>(ctx);
  if (!conn) {
    log(ERROR, "No connection exists for bufferevent %p; closing it", bev);
    bufferevent_free(bev);
    return;
  }

  bool is_from_client = bev == conn->client_bev.get();
  struct evbuffer* src_buf = bufferevent_get_input(
      is_from_client ? conn->client_bev.get() : conn->server_bev.get());
  struct evbuffer* dst_buf = bufferevent_get_output(
      is_from_client ? conn->server_bev.get() : conn->client_bev.get());
  evbuffer_add_buffer(dst_buf, src_buf);
}

void on_proxy_bufferevent_error(struct bufferevent* bev, short what, void* ctx) {
  ProxyConnection* conn = reinterpret_cast<ProxyConnection*>(ctx);
  if (!conn) {
    log(ERROR, "No connection exists for bufferevent %p; closing it", bev);
    bufferevent_free(bev);
    return;
  }

  // If either side has disconnected, close both ends (which will disconnect the
  // other side as well).
  if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    state.all_conns.erase(conn);
  }
}



static const string SHELL_HELP_STRING = "\
commands:\n\
  help: you\'re reading it now.\n\
  count (or c): show the number of connected clients.\n\
  reset [dest-host] [dest-port]: if a new dest-host/dest-port are given, change\n\
    the destination host and disconnect all clients. if a new destination is\n\
    not given, just disconnect all clients.\n\
";

void execute_shell_command(const char* line_data, size_t line_length,
    struct evbuffer* out_buffer) {
  auto tokens = split(line_data, ' ');
  if (tokens.size() == 0) {
    throw runtime_error("command is empty; try \'help\'");
  }

  string command_name = move(tokens[0]);
  tokens.erase(tokens.begin());

  if ((command_name == "help") || (command_name == "?")) {
    evbuffer_add_reference(out_buffer, SHELL_HELP_STRING.data(),
        SHELL_HELP_STRING.size(), nullptr, nullptr);

  } else if ((command_name == "count") || (command_name == "c")) {
    if (!tokens.empty()) {
      throw runtime_error("incorrect argument count");
    }

    evbuffer_add_printf(out_buffer, "there are %zu clients connected\n", state.all_conns.size());

  } else if (command_name == "reset") {
    if (tokens.size() >= 1) {
      state.dest_host = tokens[0];
      evbuffer_add_printf(out_buffer, "dest host set to %s\n", state.dest_host.c_str());
    }
    if (tokens.size() >= 2) {
      state.dest_port = atoi(tokens[1].c_str());
      evbuffer_add_printf(out_buffer, "dest port set to %hu\n", state.dest_port);
    }

    evbuffer_add_printf(out_buffer, "disconnecting all clients\n");
    state.all_conns.clear();

  } else {
    throw runtime_error(string_printf("invalid command: %s (try \'help\')", command_name.c_str()));
  }
}

void write_shell_prompt(shared_ptr<struct bufferevent> bev) {
  struct evbuffer* out_buffer = bufferevent_get_output(bev.get());
  evbuffer_add(out_buffer, "proxy> ", 7);
}

void on_shell_bufferevent_read(struct bufferevent* bev, void* ctx) {
  struct evbuffer* in_buffer = bufferevent_get_input(bev);
  struct evbuffer* out_buffer = bufferevent_get_output(bev);

  char* line_data = NULL;
  size_t line_length = 0;
  while ((line_data = evbuffer_readln(in_buffer, &line_length, EVBUFFER_EOL_CRLF))) {
    try {
      execute_shell_command(line_data, line_length, out_buffer);
      evbuffer_add_reference(out_buffer, "\n\nproxy> ", 9, NULL, NULL);
    } catch (const exception& e) {
      evbuffer_add_printf(out_buffer, "FAILED: %s\n\nproxy> ", e.what());
    }
    free(line_data);
  }
}

void on_shell_bufferevent_error(struct bufferevent* bev, short what, void* ctx) {
  if (what & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    state.shell_clients.erase(bev);
  }
}



void on_listen_accept(struct evconnlistener* listener, evutil_socket_t fd,
    struct sockaddr* address, int socklen, void* ctx) {
  int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }

  shared_ptr<struct bufferevent> client_bev(bufferevent_socket_new(
      state.base.get(), fd, BEV_OPT_CLOSE_ON_FREE), bufferevent_free);

  if (listener == state.shell_listener.get()) {
    bufferevent_setcb(client_bev.get(), on_shell_bufferevent_read, nullptr,
        on_shell_bufferevent_error, nullptr);
    bufferevent_enable(client_bev.get(), EV_READ | EV_WRITE);
    write_shell_prompt(client_bev);
    state.shell_clients.emplace(client_bev.get(), client_bev);

  } else {
    shared_ptr<ProxyConnection> conn_shared(new ProxyConnection());
    conn_shared->server_bev.reset(bufferevent_socket_new(state.base.get(), -1,
        BEV_OPT_CLOSE_ON_FREE), bufferevent_free);
    conn_shared->client_bev = client_bev;
    state.all_conns.emplace(conn_shared.get(), conn_shared);

    bufferevent_setcb(conn_shared->client_bev.get(), on_proxy_bufferevent_read,
        nullptr, on_proxy_bufferevent_error, conn_shared.get());
    bufferevent_setcb(conn_shared->server_bev.get(), on_proxy_bufferevent_read,
        nullptr, on_proxy_bufferevent_error, conn_shared.get());

    bufferevent_enable(conn_shared->client_bev.get(), EV_READ | EV_WRITE);
    bufferevent_enable(conn_shared->server_bev.get(), EV_READ | EV_WRITE);

    bufferevent_socket_connect_hostname(conn_shared->server_bev.get(),
        state.dns_base.get(), AF_UNSPEC, state.dest_host.c_str(),
        state.dest_port);
  }
}



void print_usage(const char* argv0) {
  fprintf(stderr, "Usage: %s <proxy-port> <shell-port> <dest-host> [dest-port]\n", argv0);
}

int main(int argc, char **argv) {
  if (argc < 4 || argc > 5) {
    print_usage(argv[0]);
    return 1;
  }

  state.proxy_listen_port = atoi(argv[1]);
  state.shell_listen_port = atoi(argv[2]);
  state.dest_host = argv[3];
  if (argc > 4) {
    state.dest_port = atoi(argv[4]);
  } else {
    state.dest_port = state.proxy_listen_port;
  }

  if ((state.proxy_listen_port == 0) || (state.shell_listen_port == 0)) {
    log(ERROR, "Neither listening port number may be zero");
    print_usage(argv[0]);
    return 1;
  }

  state.proxy_listen_fd = listen("", state.proxy_listen_port, SOMAXCONN);
  if (state.proxy_listen_fd < 0) {
    log(ERROR, "Can\'t open proxy listening socket on port %hu (errno %d)",
        state.proxy_listen_port, errno);
    return 2;
  }
  evutil_make_socket_nonblocking(state.proxy_listen_fd);
  log(INFO, "Opened proxy listening socket %d on port %d",
      state.proxy_listen_fd, state.proxy_listen_port);

  state.shell_listen_fd = listen("", state.shell_listen_port, SOMAXCONN);
  if (state.shell_listen_fd < 0) {
    log(ERROR, "Can\'t open shell listening socket on port %hu (errno %d)",
        state.shell_listen_port, errno);
    return 2;
  }
  evutil_make_socket_nonblocking(state.shell_listen_fd);
  log(INFO, "Opened shell listening socket %d on port %d",
      state.shell_listen_fd, state.shell_listen_port);

  signal(SIGPIPE, SIG_IGN);

  state.base.reset(event_base_new(), event_base_free);
  state.dns_base.reset(evdns_base_new(state.base.get(), 1), +[](struct evdns_base* dns_base) {
    evdns_base_free(dns_base, 1);
  });

  state.proxy_listener.reset(evconnlistener_new(state.base.get(),
      on_listen_accept, nullptr,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
      0, state.proxy_listen_fd), evconnlistener_free);
  state.shell_listener.reset(evconnlistener_new(state.base.get(),
      on_listen_accept, nullptr,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE,
      0, state.shell_listen_fd), evconnlistener_free);

  event_base_loop(state.base.get(), 0);

  return 0;
}
