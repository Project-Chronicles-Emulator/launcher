#include "release_servers.hpp"

#include "distribution/distribution.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <set>

namespace chronicles::launcher {

std::vector<ReleaseServer> release_servers(const std::filesystem::path &directory) {
  const auto profile_path = directory / "launcher-config.json";
  if (!std::filesystem::is_regular_file(profile_path)) {
    return {};
  }

  const auto profile = nlohmann::json::parse(distribution::read_bounded_file(profile_path, 65536));
  if (!profile.contains("edition")) {
    return {};
  }

  const auto edition = profile.at("edition").get<std::string>();
  if (edition != "player" && edition != "operator") {
    throw std::invalid_argument("Unknown Project Chronicles launcher edition");
  }

  // A checkout has no hosted service or signing identity. Operators supply both
  // in their distribution profile when publishing a server of their own.
  if (!profile.contains("servers")) {
    return {{"local", "Project Chronicles · Local", "http://127.0.0.1:8080", "",
             "stable", true, "127.0.0.1", 8081}};
  }

  const auto &configured_servers = profile.at("servers");
  if (!configured_servers.is_array() || configured_servers.empty() ||
      configured_servers.size() > 32) {
    throw std::invalid_argument("Configure between one and 32 launcher servers");
  }

  std::vector<ReleaseServer> servers;
  std::set<std::string> identifiers;
  for (const auto &entry : configured_servers) {
    ReleaseServer server;
    server.id = entry.at("id").get<std::string>();
    server.name = entry.at("name").get<std::string>();
    const auto origin =
        distribution::parse_service_origin(entry.at("server_url").get<std::string>());
    server.origin = origin.url;
    server.local = origin.loopback;
    server.patch_public_key = entry.value("patch_public_key", std::string{});
    server.channel = entry.value("patch_channel", std::string("stable"));
    server.world_host = entry.value("world_host", origin.host);
    const auto world_port = entry.value(
        "world_port", nlohmann::json(origin.loopback ? 8081 : 8481));
    if (!world_port.is_number_integer() || world_port < 1 || world_port > 65535) {
      throw std::invalid_argument("World port must be an integer between 1 and 65535");
    }
    server.world_port = world_port.get<int>();

    if (!distribution::valid_channel(server.id) || !identifiers.insert(server.id).second ||
        server.name.empty() || server.name.size() > 128 ||
        !distribution::valid_channel(server.channel) || server.world_host.empty() ||
        server.world_host.size() > 255 ||
        server.world_host.find_first_of("/\\\r\n\t @?#") != std::string::npos) {
      throw std::invalid_argument("Invalid launcher server identity or world endpoint");
    }
    if ((!server.local || !server.patch_public_key.empty()) &&
        !distribution::valid_sha256(server.patch_public_key)) {
      throw std::invalid_argument("Hosted servers require their own pinned patch public key");
    }
    servers.push_back(std::move(server));
  }

  return servers;
}

} // namespace chronicles::launcher
