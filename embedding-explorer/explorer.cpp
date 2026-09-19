#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <torch/torch.h>
#ifdef USE_XPU
#include <torch/xpu.h>
#endif

#include <devpost/fetch.h>
#include <github/fetch.h>
#include <model.hpp>
#include <tokenizer.hpp>

#include "server_tools/http.hpp"

using json = nlohmann::json;
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

// Local dataset server: github and devpost.
// GET /datasets/<dataset> returns metadata and PCA coordinates.
// POST /datasets/<dataset>/embed accepts {"description": "..."}.
// Sync runs in the background. User embeddings stay in memory.

struct Dataset {
  const char *name, *table, *key;
  int (*fetch_text)(const char *, char **);
};

const Dataset datasets[] = {
    {"github", "repos", "name", fetch_readme},
    {"devpost", "projects", "slug", fetch_project_description},
};

Database open_database() {
  sqlite3 *handle = nullptr;
  int result = sqlite3_open(EXPLORER_DB_PATH, &handle);
  Database db(handle, sqlite3_close);
  if (result != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(handle));
  sqlite3_busy_timeout(handle, 5000);
  return db;
}

Statement prepare(sqlite3 *db, const std::string &query) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  return Statement(stmt, sqlite3_finalize);
}

int step(sqlite3_stmt *stmt) {
  int result = sqlite3_step(stmt);
  if (result != SQLITE_ROW && result != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(stmt)));
  return result;
}

torch::Tensor embed(const std::string &text, Tokenizer &tokenizer,
                    torch::nn::Sequential &model, std::mutex &mutex) {
  std::lock_guard lock(mutex);
  torch::NoGradGuard no_grad;
  torch::Tensor tokens = tokenizer(text.substr(0, 32768)).reshape({-1});
  tokens = tokens.slice(0, 0, 2048);
  torch::Tensor vector = model->forward(tokens)
                             .to(torch::kCPU)
                             .to(torch::kFloat32)
                             .reshape({-1})
                             .contiguous();
  if (vector.numel() != 768 || !torch::isfinite(vector).all().item<bool>())
    throw std::runtime_error("Invalid embedding");
  return vector;
}

// Embed missing vectors.
void sync_vectors(sqlite3 *db, const Dataset &dataset, Tokenizer &tokenizer,
                  torch::nn::Sequential &model, std::mutex &mutex) {
  auto select =
      prepare(db, "SELECT id, " + std::string(dataset.key) + " FROM " +
                      dataset.table + " WHERE vector IS NULL;");
  std::vector<std::pair<sqlite3_int64, std::string>> pending;
  while (step(select.get()) == SQLITE_ROW) {
    const char *key =
        reinterpret_cast<const char *>(sqlite3_column_text(select.get(), 1));
    pending.emplace_back(sqlite3_column_int64(select.get(), 0), key);
  }
  select.reset();

  auto update = prepare(db, "UPDATE " + std::string(dataset.table) +
                                " SET vector = ?1 WHERE id = ?2;");
  for (const auto &[id, key] : pending) {
    char *content = nullptr;
    int result = dataset.fetch_text(key.c_str(), &content);
    std::unique_ptr<char, decltype(&std::free)> text(content, std::free);
    if (result || !content || !*content)
      continue;
    torch::Tensor vector = embed(content, tokenizer, model, mutex);
    sqlite3_bind_blob(update.get(), 1, vector.data_ptr<float>(),
                      vector.numel() * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.get(), 2, id);
    step(update.get());
    sqlite3_reset(update.get());
  }
}

json pca3(sqlite3 *db, const Dataset &dataset, const torch::Tensor &user = {}) {
  json points = json::array();
  std::vector<torch::Tensor> vectors;
  auto exists = prepare(
      db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1;");
  sqlite3_bind_text(exists.get(), 1, dataset.table, -1, SQLITE_STATIC);
  bool available = step(exists.get()) == SQLITE_ROW;
  exists.reset();
  if (available) {
    auto select = prepare(db, "SELECT * FROM " + std::string(dataset.table) +
                                  " WHERE vector IS NOT NULL ORDER BY id;");
    while (step(select.get()) == SQLITE_ROW) {
      json point;
      torch::Tensor vector;
      for (int col = 0; col < sqlite3_column_count(select.get()); ++col) {
        std::string name = sqlite3_column_name(select.get(), col);
        if (name == "vector") {
          int bytes = sqlite3_column_bytes(select.get(), col);
          if (sqlite3_column_type(select.get(), col) != SQLITE_BLOB ||
              bytes != 768 * sizeof(float))
            throw std::runtime_error("Invalid stored embedding size");
          vector = torch::from_blob(const_cast<void *>(
                                        sqlite3_column_blob(select.get(), col)),
                                    {768}, torch::kFloat32)
                       .clone();
        } else if (sqlite3_column_type(select.get(), col) == SQLITE_NULL) {
          point[name] = nullptr;
        } else if (sqlite3_column_type(select.get(), col) == SQLITE_INTEGER) {
          point[name] = sqlite3_column_int64(select.get(), col);
        } else {
          point[name] = reinterpret_cast<const char *>(
              sqlite3_column_text(select.get(), col));
        }
      }
      vectors.push_back(vector);
      points.push_back(point);
    }
  }

  if (user.defined())
    vectors.push_back(user);
  torch::Tensor positions =
      torch::zeros({static_cast<int64_t>(vectors.size()), 3});
  if (vectors.size() > 1) {
    torch::Tensor matrix = torch::stack(vectors);
    if (!torch::isfinite(matrix).all().item<bool>())
      throw std::runtime_error("Invalid stored embedding");
    torch::Tensor mean = matrix.mean(0);
    torch::Tensor centered = matrix - mean;
    int64_t count = std::min<int64_t>(3, matrix.size(0) - 1);
    auto [u, s, vh] = torch::linalg_svd(centered, false);
    torch::Tensor basis = vh.slice(0, 0, count).transpose(0, 1);
    positions.slice(1, 0, count).copy_(centered.matmul(basis));
  }
  for (size_t i = 0; i < points.size(); ++i)
    points[i]["position"] = {positions[i][0].item<float>(),
                             positions[i][1].item<float>(),
                             positions[i][2].item<float>()};
  json response = {{"dataset", dataset.name}, {"points", points}};
  if (user.defined()) {
    torch::Tensor user_position = positions[points.size()];
    response["query"] = {
        {"position",
         {user_position[0].item<float>(), user_position[1].item<float>(),
          user_position[2].item<float>()}}};
  }
  return response;
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    // Load model.
#ifdef USE_XPU
    const torch::Device device(torch::xpu::is_available() ? torch::kXPU
                                                        : torch::kCPU);
    if (device.is_cpu())
      std::cerr << "No usable Intel XPU device; using CPU.\n";
#else
    const torch::Device device(torch::kCPU);
#endif
    Tokenizer tokenizer = load_tokenizer(EXPLORER_MODEL_PATH, device);
    torch::nn::Sequential model = load_modules(EXPLORER_MODEL_PATH, device);
    if (!tokenizer || !model)
      return 1;
    model->eval();
    std::mutex mutex;
    Database db = open_database();

    // Listen on localhost.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1)
      throw std::runtime_error("Socket creation failed");
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(1026);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
        listen(listener, 10) < 0) {
      close(listener);
      throw std::runtime_error("Listen failed");
    }

    // Sync datasets.
    std::jthread sync([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        for (const Dataset &dataset : datasets) {
          try {
            if (std::string(dataset.name) == "github") {
              if (fetch_repos_star_range(50000, -1) < 0 ||
                  fetch_repos_star_range(40000, 49999) < 0 ||
                  fetch_repos_star_range(30000, 39999) < 0)
                throw std::runtime_error("Repository fetch failed");
            } else {
              if (fetch_projects(1))
                throw std::runtime_error("Project fetch failed");
            }
            Database writer = open_database();
            sync_vectors(writer.get(), dataset, tokenizer, model, mutex);
          } catch (const std::exception &error) {
            std::cerr << dataset.name << ": " << error.what() << '\n';
          }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    });
    std::cout << "Server running at http://127.0.0.1:1026\n";

    // Serve requests.
    while (true) {
      int client = accept(listener, nullptr, nullptr);
      if (client < 0)
        continue;
      timeval timeout{5, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      try {
        Request request;
        if (!read_request(client, request)) {
          send_response(client, "400 Bad Request",
                        R"({"error":"Invalid request"})");
        } else if (request.method == "GET" && request.path == "/datasets") {
          json names = json::array();
          for (const Dataset &dataset : datasets)
            names.push_back(dataset.name);
          send_response(client, "200 OK", names.dump());
        } else {
          const Dataset *selected = nullptr;
          bool query = false;
          for (const Dataset &dataset : datasets) {
            std::string path = "/datasets/" + std::string(dataset.name);
            if (request.path == path || request.path == path + "/embed") {
              selected = &dataset;
              query = request.path == path + "/embed";
              break;
            }
          }
          if (!selected) {
            send_response(client, "404 Not Found",
                          R"({"error":"Unknown dataset or route"})");
          } else if ((!query && request.method != "GET") ||
                     (query && request.method != "POST")) {
            send_response(client, "405 Method Not Allowed",
                          R"({"error":"Method not allowed"})");
          } else if (!query) {
            send_response(client, "200 OK", pca3(db.get(), *selected).dump());
          } else {
            json body = json::parse(request.body, nullptr, false);
            if (!body.is_object() || !body.contains("description") ||
                !body["description"].is_string() ||
                body["description"].get_ref<const std::string &>().empty() ||
                body["description"].get_ref<const std::string &>().size() >
                    32768) {
              send_response(client, "400 Bad Request",
                            R"({"error":"description must be 1-32768 bytes"})");
            } else {
              std::string description = body["description"];
              torch::Tensor vector =
                  embed(description, tokenizer, model, mutex);
              json response = pca3(db.get(), *selected, vector);
              response["query"]["description"] = description;
              send_response(client, "200 OK", response.dump());
            }
          }
        }
      } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        send_response(client, "500 Internal Server Error",
                      R"({"error":"Request failed"})");
      }
      close(client);
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
