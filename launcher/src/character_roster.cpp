#include "account_session.hpp"
#include "http_client.hpp"
#include "xorstr.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace chronicles::launcher {
namespace {

using json = nlohmann::json;

int bounded_integer(const json &document, const char *field, int minimum, int maximum) {
  const auto &value = document.at(field);
  if (!value.is_number_integer() || value < minimum || value > maximum) {
    throw std::invalid_argument("Invalid character number");
  }
  return value.get<int>();
}

std::string bounded_text(const json &document, const char *field, std::size_t maximum) {
  const auto value = document.at(field).get<std::string>();
  if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos) {
    throw std::invalid_argument("Invalid character text");
  }
  return value;
}

std::string decimal_text(const json &document, const char *field) {
  const auto value = bounded_text(document, field, 20);
  if (!std::ranges::all_of(value,
                           [](char character) { return character >= '0' && character <= '9'; })) {
    throw std::invalid_argument("Invalid character identifier or experience");
  }
  return value;
}

json validated_roster(const json &document) {
  const auto &characters = document.at("characters");
  if (!characters.is_array() || characters.size() > 4) {
    throw std::invalid_argument("Invalid character roster size");
  }
  auto result = json::array();
  std::set<int> slots;
  std::set<std::string> ids;
  for (const auto &character : characters) {
    const auto id = decimal_text(character, "id");
    const auto slot = bounded_integer(character, "slot", 0, 3);
    if (!slots.insert(slot).second || !ids.insert(id).second) {
      throw std::invalid_argument("Duplicate character slot or identifier");
    }
    const auto &equipment = character.at("equipment");
    if (!equipment.is_array() || equipment.size() > 25) {
      throw std::invalid_argument("Invalid character equipment size");
    }
    auto safe_equipment = json::array();
    std::set<int> equipment_slots;
    for (const auto &item : equipment) {
      const auto equipment_slot = bounded_integer(item, "slot", 0, 24);
      if (!equipment_slots.insert(equipment_slot).second) {
        throw std::invalid_argument("Duplicate equipment slot");
      }
      safe_equipment.push_back(
          {{"slot", equipment_slot},
           {"grade", bounded_integer(item, "grade", 0, 255)},
           {"durability", bounded_integer(item, "durability", 0, 2147483647)}});
    }
    result.push_back({{"id", id},
                      {"slot", slot},
                      {"name", bounded_text(character, "name", 128)},
                      {"class_name", bounded_text(character, "class_name", 80)},
                      {"class_id", bounded_integer(character, "class_id", 1, 11)},
                      {"level", bounded_integer(character, "level", 1, 1000)},
                      {"world_id", bounded_integer(character, "world_id", 1, 2147483647)},
                      {"world_name", bounded_text(character, "world_name", 160)},
                      {"experience", decimal_text(character, "experience")},
                      {"equipment", safe_equipment}});
  }
  return result;
}

} // namespace

void AccountSession::refresh_characters(bool force) {
  if (token_.empty() || (!force && now_() < next_roster_refresh_)) {
    return;
  }
  next_roster_refresh_ = now_() + std::chrono::seconds(60);
  try {
    auto document =
        transport_("GET", origin_ + xorstr_("/launcher/v1/characters"), json::object(), token_);
    auto characters = validated_roster(document);
    characters_ = std::move(characters);
    character_status_ = "ready";
    character_error_.clear();
  } catch (const HttpError &error) {
    if (error.status() == 401 || error.status() == 403) {
      expire();
      return;
    }
    character_status_ = "error";
    character_error_ = "Could not refresh characters. Showing the last saved roster received.";
  } catch (const std::exception &) {
    character_status_ = "error";
    character_error_ = "Could not refresh characters. Showing the last saved roster received.";
  }
}

} // namespace chronicles::launcher
