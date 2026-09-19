#ifndef EXPLORER_HTTP_HPP
#define EXPLORER_HTTP_HPP

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <sstream>
#include <string>
#include <sys/socket.h>

struct Request {
  std::string method, path, body;
};

// Read one HTTP/1.1 request.
inline bool read_request(int socket, Request &request) {
  constexpr size_t header_limit = 8192, body_limit = 65536;
  std::string data;
  char buffer[4096];
  size_t end;
  while ((end = data.find("\r\n\r\n")) == std::string::npos) {
    ssize_t size = recv(socket, buffer, sizeof(buffer), 0);
    if (size < 0 && errno == EINTR)
      continue;
    if (size <= 0)
      return false;
    data.append(buffer, size);
    if (data.find("\r\n\r\n") == std::string::npos &&
        data.size() > header_limit)
      return false;
  }
  if (end > header_limit)
    return false;

  std::istringstream headers(data.substr(0, end));
  std::string line, version, extra;
  std::getline(headers, line);
  std::istringstream first(line);
  if (!(first >> request.method >> request.path >> version) || first >> extra ||
      version != "HTTP/1.1" || !request.path.starts_with('/'))
    return false;

  size_t length = 0;
  bool has_length = false;
  while (std::getline(headers, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
      return false;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string value = line.substr(colon + 1);
    size_t start = value.find_first_not_of(" \t");
    value = start == std::string::npos ? "" : value.substr(start);
    size_t last = value.find_last_not_of(" \t");
    if (last != std::string::npos)
      value.resize(last + 1);
    if (name == "transfer-encoding")
      return false;
    if (name == "content-length") {
      auto result =
          std::from_chars(value.data(), value.data() + value.size(), length);
      if (has_length || result.ec != std::errc() ||
          result.ptr != value.data() + value.size() || length > body_limit)
        return false;
      has_length = true;
    }
  }
  request.body = data.substr(end + 4, length);
  while (request.body.size() < length) {
    ssize_t size =
        recv(socket, buffer,
             std::min(sizeof(buffer), length - request.body.size()), 0);
    if (size < 0 && errno == EINTR)
      continue;
    if (size <= 0)
      return false;
    request.body.append(buffer, size);
  }
  return request.method != "POST" || has_length;
}

inline void send_response(int socket, const std::string &status,
                          const std::string &body) {
  std::string response =
      "HTTP/1.1 " + status +
      "\r\nContent-Type: application/json\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < response.size()) {
    ssize_t size =
        send(socket, response.data() + sent, response.size() - sent, 0);
    if (size < 0 && errno == EINTR)
      continue;
    if (size <= 0)
      break;
    sent += size;
  }
}

#endif
