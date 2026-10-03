#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Local dataset server: github and one dataset per hackathon.
// GET /datasets lists them, with the years each hackathon has projects for.
// GET /datasets/github returns its top 1000 repos by stars.
// GET /datasets/<hackathon>[/<year>] returns every year of it, or one.
// POST <dataset path>/embed accepts {"description": "..."}.
// Sync runs in the background. User embeddings stay in memory.

struct Hackathon {
  const char *id, *label;
};

const Hackathon hackathons[] = {
    {"uofthacks", "UofTHacks"},
    {"hackthenorth", "Hack the North"},
    {"treehacks", "TreeHacks"},
    {"hackmit", "HackMIT"},
};

// Embedded once at startup, every point is coloured by the one it is closest to
const char *category_labels[] = {
    "AI & machine learning",    "Web & mobile apps",
    "Developer tools",          "Systems & infrastructure",
    "Data & science",           "Games & graphics",
    "Health",                   "Education & social good",
};

constexpr int github_limit = 1000;
constexpr int nearest_count = 8;

// One dataset as it is served: github, a hackathon, or one year of one
struct Selection {
  std::string name;  // cache key and the "dataset" field
  const Hackathon *hackathon = nullptr; // null for github
  int year = 0;      // 0 for every year
};

// A dataset's PCA, kept until sync changes rows. The body is the GET response
// without its closing brace, so an embed only appends its query to it.
struct View {
  uint64_t generation;
  std::string body;
  torch::Tensor mean, basis, unit; // [768], [768, 3], [points, 768]
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

double rounded(double value) { return std::round(value * 1e4) / 1e4; }

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

bool table_has_column(sqlite3 *db, const char *table, const char *column) {
  auto info = prepare(db, "PRAGMA table_info(" + std::string(table) + ");");
  while (step(info.get()) == SQLITE_ROW)
    if (std::string(reinterpret_cast<const char *>(
            sqlite3_column_text(info.get(), 1))) == column)
      return true;
  return false;
}

// Embed missing vectors, bumping generation now and then so a long sync shows
// up in the served datasets as it goes rather than only at the end.
void sync_vectors(sqlite3 *db, const Hackathon *hackathon,
                  Tokenizer &tokenizer, torch::nn::Sequential &model,
                  std::mutex &mutex, std::atomic<uint64_t> &generation) {
  const char *table = hackathon ? "projects" : "repos";
  if (!table_has_column(db, table, "vector"))
    return;
  auto select =
      hackathon ? prepare(db, "SELECT id, slug FROM projects "
                              "WHERE vector IS NULL AND hackathon = ?1;")
                : prepare(db, "SELECT id, name FROM (SELECT id, name, vector "
                              "FROM repos ORDER BY stars DESC LIMIT " +
                                  std::to_string(github_limit) +
                                  ") WHERE vector IS NULL;");
  if (hackathon)
    sqlite3_bind_text(select.get(), 1, hackathon->id, -1, SQLITE_STATIC);
  std::vector<std::pair<sqlite3_int64, std::string>> pending;
  while (step(select.get()) == SQLITE_ROW) {
    const char *key =
        reinterpret_cast<const char *>(sqlite3_column_text(select.get(), 1));
    pending.emplace_back(sqlite3_column_int64(select.get(), 0), key);
  }
  select.reset();

  auto update = prepare(db, "UPDATE " + std::string(table) +
                                " SET vector = ?1 WHERE id = ?2;");
  int written = 0;
  for (const auto &[id, key] : pending) {
    char *content = nullptr;
    int result = hackathon ? fetch_project_description(key.c_str(), &content)
                           : fetch_readme(key.c_str(), &content);
    std::unique_ptr<char, decltype(&std::free)> text(content, std::free);
    if (result || !content || !*content)
      continue;
    torch::Tensor vector = embed(content, tokenizer, model, mutex);
    sqlite3_bind_blob(update.get(), 1, vector.data_ptr<float>(),
                      vector.numel() * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.get(), 2, id);
    step(update.get());
    sqlite3_reset(update.get());
    if (++written % 100 == 0)
      ++generation;
  }
}

// Years a hackathon has embedded projects for, newest first
json hackathon_years(sqlite3 *db, const Hackathon &hackathon) {
  json years = json::array();
  if (!table_has_column(db, "projects", "year"))
    return years;
  auto select = prepare(db, "SELECT DISTINCT year FROM projects WHERE "
                            "hackathon = ?1 AND year IS NOT NULL AND vector "
                            "IS NOT NULL ORDER BY year DESC;");
  sqlite3_bind_text(select.get(), 1, hackathon.id, -1, SQLITE_STATIC);
  while (step(select.get()) == SQLITE_ROW)
    years.push_back(sqlite3_column_int(select.get(), 0));
  return years;
}

// Category, and its cosine similarity, of a unit vector or a batch of them
std::pair<torch::Tensor, torch::Tensor> categorize(const torch::Tensor &unit,
                                                   const torch::Tensor &labels) {
  auto [similarity, category] = unit.matmul(labels.transpose(0, 1)).max(-1);
  return {category, similarity};
}

std::shared_ptr<const View> build_view(sqlite3 *db, const Selection &selection,
                                       const torch::Tensor &labels,
                                       uint64_t generation) {
  json points = json::array();
  std::vector<float> data;
  const char *table = selection.hackathon ? "projects" : "repos";
  bool available = table_has_column(db, table, "vector") &&
                   (!selection.hackathon ||
                    table_has_column(db, "projects", "hackathon"));
  if (available) {
    auto select =
        selection.hackathon
            ? prepare(db, std::string("SELECT * FROM projects WHERE vector IS "
                                      "NOT NULL AND hackathon = ?1") +
                              (selection.year ? " AND year = ?2" : "") +
                              " ORDER BY id;")
            : prepare(db, "SELECT * FROM repos WHERE vector IS NOT NULL "
                          "ORDER BY stars DESC LIMIT " +
                              std::to_string(github_limit) + ";");
    if (selection.hackathon) {
      sqlite3_bind_text(select.get(), 1, selection.hackathon->id, -1,
                        SQLITE_STATIC);
      if (selection.year)
        sqlite3_bind_int(select.get(), 2, selection.year);
    }
    while (step(select.get()) == SQLITE_ROW) {
      json point;
      for (int col = 0; col < sqlite3_column_count(select.get()); ++col) {
        std::string name = sqlite3_column_name(select.get(), col);
        if (name == "vector") {
          int bytes = sqlite3_column_bytes(select.get(), col);
          if (sqlite3_column_type(select.get(), col) != SQLITE_BLOB ||
              bytes != 768 * sizeof(float))
            throw std::runtime_error("Invalid stored embedding size");
          const float *blob = static_cast<const float *>(
              sqlite3_column_blob(select.get(), col));
          data.insert(data.end(), blob, blob + 768);
        } else if (sqlite3_column_type(select.get(), col) == SQLITE_NULL) {
          point[name] = nullptr;
        } else if (sqlite3_column_type(select.get(), col) == SQLITE_INTEGER) {
          point[name] = sqlite3_column_int64(select.get(), col);
        } else {
          point[name] = reinterpret_cast<const char *>(
              sqlite3_column_text(select.get(), col));
        }
      }
      points.push_back(std::move(point));
    }
  }

  // PCA from the 768 x 768 covariance rather than an SVD of every point, which
  // is what keeps a dataset of thousands of points quick to rebuild.
  int64_t count = points.size();
  torch::Tensor matrix =
      torch::from_blob(data.data(), {count, 768}, torch::kFloat32).clone();
  if (!torch::isfinite(matrix).all().item<bool>())
    throw std::runtime_error("Invalid stored embedding");
  auto view = std::make_shared<View>();
  view->generation = generation;
  view->mean = count ? matrix.mean(0) : torch::zeros({768});
  view->basis = torch::zeros({768, 3});
  torch::Tensor centered = matrix - view->mean;
  if (count > 1) {
    auto [values, vectors] =
        torch::linalg_eigh(centered.transpose(0, 1).matmul(centered));
    int64_t components = std::min<int64_t>(3, count - 1);
    // Eigenvalues come back ascending, the principal components are the last
    view->basis.slice(1, 0, components)
        .copy_(vectors.slice(1, 768 - components).flip(1));
  }
  view->unit = torch::nn::functional::normalize(
      matrix, torch::nn::functional::NormalizeFuncOptions().dim(1));
  torch::Tensor positions = centered.matmul(view->basis).contiguous();
  auto [category, similarity] = categorize(view->unit, labels);
  auto position = positions.accessor<float, 2>();
  auto categories = category.accessor<int64_t, 1>();
  auto similarities = similarity.accessor<float, 1>();
  for (int64_t i = 0; i < count; ++i) {
    points[i]["position"] = {rounded(position[i][0]), rounded(position[i][1]),
                             rounded(position[i][2])};
    points[i]["category"] = categories[i];
    points[i]["similarity"] = rounded(similarities[i]);
  }

  json response = {{"dataset", selection.name},
                   {"categories", std::vector<std::string>(std::begin(category_labels),
                                                std::end(category_labels))},
                   {"points", std::move(points)}};
  if (selection.hackathon) {
    response["hackathon"] = selection.hackathon->id;
    response["year"] = selection.year ? json(selection.year) : json(nullptr);
  }
  view->body = response.dump();
  view->body.pop_back();
  return view;
}

// The query's position on the view's PCA, so the points around it stay where
// the GET put them, and the points nearest it in the full 768 dimensions.
std::string embed_response(const View &view, const torch::Tensor &vector,
                           const torch::Tensor &labels,
                           const std::string &description) {
  torch::Tensor unit = torch::nn::functional::normalize(
      vector, torch::nn::functional::NormalizeFuncOptions().dim(0));
  torch::Tensor position = (vector - view.mean).matmul(view.basis);
  json nearest = json::array();
  int64_t count = view.unit.size(0);
  if (count) {
    torch::Tensor indices =
        std::get<1>(view.unit.matmul(unit).topk(
                        std::min<int64_t>(nearest_count, count)))
            .contiguous();
    for (int64_t i = 0; i < indices.size(0); ++i)
      nearest.push_back(indices[i].item<int64_t>());
  }
  auto [category, similarity] = categorize(unit, labels);
  json query = {{"description", description},
                {"position",
                 {rounded(position[0].item<float>()),
                  rounded(position[1].item<float>()),
                  rounded(position[2].item<float>())}},
                {"nearest", nearest},
                {"category", category.item<int64_t>()},
                {"similarity", rounded(similarity.item<float>())}};
  return view.body + ",\"query\":" + query.dump() + "}";
}

// Resolve a request path to the dataset it names, if any
bool select_dataset(std::string_view path, Selection &selection, bool &query) {
  constexpr std::string_view prefix = "/datasets/";
  if (!path.starts_with(prefix))
    return false;
  path.remove_prefix(prefix.size());
  query = path.ends_with("/embed");
  if (query)
    path.remove_suffix(std::string_view("/embed").size());
  if (path == "github") {
    selection = {"github"};
    return true;
  }
  std::string_view id = path.substr(0, path.find('/'));
  std::string_view year =
      id.size() < path.size() ? path.substr(id.size() + 1) : "";
  for (const Hackathon &hackathon : hackathons) {
    if (id != hackathon.id)
      continue;
    selection = {std::string(path), &hackathon, 0};
    if (year.empty())
      return true;
    auto result = std::from_chars(year.data(), year.data() + year.size(),
                                  selection.year);
    return result.ec == std::errc() &&
           result.ptr == year.data() + year.size() && selection.year > 0;
  }
  return false;
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

    // Embed category labels.
    std::vector<torch::Tensor> label_vectors;
    for (const char *label : category_labels)
      label_vectors.push_back(embed(label, tokenizer, model, mutex));
    const torch::Tensor labels = torch::nn::functional::normalize(
        torch::stack(label_vectors),
        torch::nn::functional::NormalizeFuncOptions().dim(1));

    // Views are rebuilt on first request after sync bumps generation.
    std::atomic<uint64_t> generation = 1;
    std::mutex views_mutex;
    std::map<std::string, std::shared_ptr<const View>> views;
    auto view_of = [&](sqlite3 *handle, const Selection &selection) {
      std::lock_guard lock(views_mutex);
      uint64_t current = generation;
      auto &view = views[selection.name];
      if (!view || view->generation != current)
        view = build_view(handle, selection, labels, current);
      return view;
    };

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
        listen(listener, 64) < 0) {
      close(listener);
      throw std::runtime_error("Listen failed");
    }

    // Sync datasets, then warm their views so the next GET is served cached.
    std::jthread sync([&](std::stop_token stop) {
      Database writer = open_database();
      while (!stop.stop_requested()) {
        try {
          if (fetch_repos_star_range(50000, -1) < 0 ||
              fetch_repos_star_range(40000, 49999) < 0 ||
              fetch_repos_star_range(30000, 39999) < 0)
            throw std::runtime_error("Repository fetch failed");
          sync_vectors(writer.get(), nullptr, tokenizer, model, mutex,
                       generation);
          ++generation;
          view_of(writer.get(), {"github"});
        } catch (const std::exception &error) {
          std::cerr << "github: " << error.what() << '\n';
        }
        for (const Hackathon &hackathon : hackathons) {
          try {
            if (fetch_hackathon_projects(hackathon.id, hackathon.label, 0))
              std::cerr << hackathon.id << ": some galleries failed\n";
            sync_vectors(writer.get(), &hackathon, tokenizer, model, mutex,
                         generation);
            ++generation;
            view_of(writer.get(), {hackathon.id, &hackathon});
          } catch (const std::exception &error) {
            std::cerr << hackathon.id << ": " << error.what() << '\n';
          }
        }
        // Past hackathons rarely change, so a pass an hour is plenty.
        for (int second = 0; second < 3600 && !stop.stop_requested(); ++second)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    });
    std::cout << "Server running at http://127.0.0.1:1026\n";

    // Serve requests, one thread each so an embed does not hold up a GET.
    auto serve = [&](int client) {
      try {
        Request request;
        std::string_view path;
        Selection selection;
        bool query = false;
        if (read_request(client, request))
          path = std::string_view(request.path).substr(
              0, request.path.find('?'));
        if (path.empty()) {
          send_response(client, "400 Bad Request",
                        R"({"error":"Invalid request"})");
        } else if (request.method == "GET" && path == "/datasets") {
          json list = {{"github", {{"label", "GitHub"}}},
                       {"hackathons", json::array()}};
          for (const Hackathon &hackathon : hackathons)
            list["hackathons"].push_back(
                {{"id", hackathon.id},
                 {"label", hackathon.label},
                 {"years", hackathon_years(db.get(), hackathon)}});
          send_response(client, "200 OK", list.dump());
        } else if (!select_dataset(path, selection, query)) {
          send_response(client, "404 Not Found",
                        R"({"error":"Unknown dataset or route"})");
        } else if ((!query && request.method != "GET") ||
                   (query && request.method != "POST")) {
          send_response(client, "405 Method Not Allowed",
                        R"({"error":"Method not allowed"})");
        } else if (!query) {
          send_response(client, "200 OK", view_of(db.get(), selection)->body + "}");
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
            torch::Tensor vector = embed(description, tokenizer, model, mutex);
            send_response(client, "200 OK",
                          embed_response(*view_of(db.get(), selection), vector,
                                         labels, description));
          }
        }
      } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        send_response(client, "500 Internal Server Error",
                      R"({"error":"Request failed"})");
      }
      close(client);
    };
    while (true) {
      int client = accept(listener, nullptr, nullptr);
      if (client < 0)
        continue;
      timeval timeout{5, 0};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      std::thread(serve, client).detach();
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
